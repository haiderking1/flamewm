#include "player.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <pthread.h>
#include <spa/param/audio/format-utils.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uchardet/uchardet.h>

#include "subs.h"

// the engine. three threads: a reader splitting the file into the video's
// and the sound's packets, a decoder for each. frames go into the window's
// pictures (converted on the way, straight into the memory flame reads),
// sound into a ring pipewire plays from. a seek bumps the serial: whatever
// came before it, queued, decoded or in the ring, is dropped on sight

#define RATE 48000
#define CHANNELS 2
#define RING_FRAMES (RATE / 2) // half a second of sound ahead, at most
#define VIDEO_PACKETS 96 // queued ahead, at most
#define AUDIO_PACKETS 256

enum { PIC_FREE, PIC_FILLING, PIC_READY, PIC_SHOWN };
enum { ITEM_PACKET, ITEM_FLUSH, ITEM_END };

struct item {
	struct item *next;
	int kind, serial;
	AVPacket *packet;
};

struct queue {
	struct item *head, *tail;
	int len;
	pthread_mutex_t lock;
	pthread_cond_t cond;
};

// a track to pick: a stream of the file's, or (subtitles) a file beside it
struct track {
	int stream; // -1: file
	char *file;
	char name[96];
};

#define TRACKS 32

struct player {
	AVFormatContext *fmt;
	int vstream, astream;
	char *path;
	struct track audio[TRACKS], subtitles[TRACKS];
	int naudio, nsubtitles;
	int audio_on, subtitles_on; // which of them (-1: none)
	// another sound picked: its stream (the reader's from its next seek on,
	// -1: as it is), its decoder (the sound's from the flush that seek sends)
	int astream_next;
	AVCodecContext *actx_next;
	// the subtitles shown, under sub_lock: decoded by the reader as it reads
	// them (sstream's packets), or all at once (a file)
	pthread_mutex_t sub_lock;
	int sstream;
	AVCodecContext *sctx;
	struct subs *subs;
	bool cover; // the video's a still: a song's cover
	AVCodecContext *vctx, *actx;
	struct gpu *gpu; // decoding on the gpu (vulkan), NULL: on the cpu
	double start; // the file's first timestamp: what's 0 here
	double duration, frame_time;
	int width, height;
	double aspect;
	char *title;
	void (*wake)(void *data);
	void *wake_data;

	pthread_t reader, vdecoder, adecoder;
	bool reader_up, vdecoder_up, adecoder_up;
	atomic_bool quit;
	struct queue vq, aq;

	// under lock
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int serial;
	bool seek_wanted;
	double seek_to;
	bool playing, reader_eof, vdone, adone, shown_since_seek;
	struct player_picture *pictures;
	int npictures;
	// the clock: clock_pts heard (or shown) at clock_at, moving while playing
	double clock_pts, clock_at;

	// the sound's ring, float stereo, under ring_lock
	pthread_mutex_t ring_lock;
	pthread_cond_t ring_cond;
	float *ring;
	size_t ring_read, ring_len;
	double ring_pts; // the time of the sound at ring_read
	int ring_serial;
	atomic_bool sounding; // playing, as the pipewire thread sees it
	double volume;
	struct pw_thread_loop *loop;
	struct pw_stream *stream;
	struct spa_hook listener;
};

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void wake(struct player *p) {
	if (p->wake) {
		p->wake(p->wake_data);
	}
}

// ---- packet queues ----

static void queue_init(struct queue *q) {
	*q = (struct queue){0};
	pthread_mutex_init(&q->lock, NULL);
	pthread_cond_init(&q->cond, NULL);
}

static void queue_clear_locked(struct queue *q) {
	while (q->head) {
		struct item *it = q->head;
		q->head = it->next;
		av_packet_free(&it->packet);
		free(it);
	}
	q->tail = NULL;
	q->len = 0;
}

static void queue_finish(struct queue *q) {
	queue_clear_locked(q);
	pthread_mutex_destroy(&q->lock);
	pthread_cond_destroy(&q->cond);
}

static void queue_put(struct queue *q, int kind, int serial, AVPacket *packet, bool flush) {
	struct item *it = calloc(1, sizeof(*it));
	if (!it) {
		av_packet_free(&packet);
		return;
	}
	*it = (struct item){.kind = kind, .serial = serial, .packet = packet};
	pthread_mutex_lock(&q->lock);
	if (flush) {
		queue_clear_locked(q); // what came before a seek is of no use
	}
	if (q->tail) {
		q->tail->next = it;
	} else {
		q->head = it;
	}
	q->tail = it;
	q->len++;
	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->lock);
}

static struct item *queue_get(struct player *p, struct queue *q) {
	pthread_mutex_lock(&q->lock);
	while (!q->head && !atomic_load(&p->quit)) {
		pthread_cond_wait(&q->cond, &q->lock);
	}
	struct item *it = q->head;
	if (it) {
		q->head = it->next;
		q->tail = q->head ? q->tail : NULL;
		q->len--;
	}
	pthread_mutex_unlock(&q->lock);
	return it;
}

static int queue_len(struct queue *q) {
	pthread_mutex_lock(&q->lock);
	int len = q->len;
	pthread_mutex_unlock(&q->lock);
	return len;
}

static void item_free(struct item *it) {
	if (it) {
		av_packet_free(&it->packet);
		free(it);
	}
}

// ---- the clock ----

static double clock_locked(const struct player *p) {
	double t = p->clock_pts;
	if (p->playing) {
		t += now() - p->clock_at;
	}
	if (p->duration > 0 && t > p->duration) {
		t = p->duration;
	}
	return t < 0 ? 0 : t;
}

double player_time(const struct player *p) {
	pthread_mutex_lock((pthread_mutex_t *)&p->lock);
	double t = clock_locked(p);
	pthread_mutex_unlock((pthread_mutex_t *)&p->lock);
	return t;
}

// ---- subtitles ----

// a subtitle stream's packet, decoded into what's shown (when it's the one
// on)
static void subtitle_packet(struct player *p, AVPacket *packet) {
	pthread_mutex_lock(&p->sub_lock);
	if (p->sctx && packet->stream_index == p->sstream) {
		AVSubtitle sub;
		int got = 0;
		if (avcodec_decode_subtitle2(p->sctx, &sub, &got, packet) >= 0 && got) {
			AVRational tb = p->fmt->streams[p->sstream]->time_base;
			double start = packet->pts != AV_NOPTS_VALUE ? packet->pts * av_q2d(tb) - p->start : 0;
			subs_add(p->subs, &sub, start, p->sctx->width, p->sctx->height);
			avsubtitle_free(&sub);
			wake(p); // shown now, if it's due (paused, say)
		}
	}
	pthread_mutex_unlock(&p->sub_lock);
}

// ---- the reader ----

static void *reader(void *data) {
	struct player *p = data;
	while (!atomic_load(&p->quit)) {
		pthread_mutex_lock(&p->lock);
		if (p->seek_wanted) {
			int serial = p->serial;
			double to = p->seek_to;
			p->seek_wanted = false;
			p->reader_eof = false;
			if (p->astream_next >= 0) {
				// another sound: its packets from here on (the decoder
				// changes over at the flush)
				p->astream = p->astream_next;
				p->astream_next = -1;
			}
			pthread_mutex_unlock(&p->lock);
			// the keyframe at or before it: the decoders go on from there to it
			int64_t ts = (int64_t)((to + p->start) * AV_TIME_BASE);
			avformat_seek_file(p->fmt, -1, INT64_MIN, ts, ts, 0);
			queue_put(&p->vq, ITEM_FLUSH, serial, NULL, true);
			queue_put(&p->aq, ITEM_FLUSH, serial, NULL, true);
			continue;
		}
		if (p->reader_eof || queue_len(&p->vq) > VIDEO_PACKETS
				|| queue_len(&p->aq) > AUDIO_PACKETS) {
			// all read, or enough queued: till a seek or a little later
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_nsec += 10 * 1000 * 1000;
			if (until.tv_nsec >= 1000000000) {
				until.tv_sec++;
				until.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&p->cond, &p->lock, &until);
			pthread_mutex_unlock(&p->lock);
			continue;
		}
		int serial = p->serial;
		pthread_mutex_unlock(&p->lock);
		AVPacket *packet = av_packet_alloc();
		if (!packet) {
			break;
		}
		if (av_read_frame(p->fmt, packet) < 0) {
			av_packet_free(&packet);
			queue_put(&p->vq, ITEM_END, serial, NULL, false);
			queue_put(&p->aq, ITEM_END, serial, NULL, false);
			pthread_mutex_lock(&p->lock);
			p->reader_eof = p->serial == serial;
			pthread_mutex_unlock(&p->lock);
			continue;
		}
		if (packet->stream_index == p->vstream && !p->cover) {
			queue_put(&p->vq, ITEM_PACKET, serial, packet, false);
		} else if (packet->stream_index == p->astream) {
			queue_put(&p->aq, ITEM_PACKET, serial, packet, false);
		} else {
			subtitle_packet(p, packet);
			av_packet_free(&packet);
		}
	}
	return NULL;
}

// ---- the picture ----

// a frame's pixels to rgb the way it says it was made: its colour space
// (hd's bt.709 when it doesn't say and it's hd) and its range (the "jpeg"
// formats are full range, said in the format itself, which swscale won't
// take any more)
static enum AVPixelFormat plain_format(enum AVPixelFormat format, bool *full) {
	switch (format) {
	case AV_PIX_FMT_YUVJ420P:
		*full = true;
		return AV_PIX_FMT_YUV420P;
	case AV_PIX_FMT_YUVJ422P:
		*full = true;
		return AV_PIX_FMT_YUV422P;
	case AV_PIX_FMT_YUVJ444P:
		*full = true;
		return AV_PIX_FMT_YUV444P;
	case AV_PIX_FMT_YUVJ440P:
		*full = true;
		return AV_PIX_FMT_YUV440P;
	default:
		return format;
	}
}

static void colours(struct SwsContext *sws, const AVFrame *frame) {
	bool full = frame->color_range == AVCOL_RANGE_JPEG;
	plain_format(frame->format, &full);
	// not said: full range comes from a jpeg's world (bt.601), hd video
	// from bt.709's
	int space = frame->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709
		: frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL
		? SWS_CS_BT2020 : frame->colorspace == AVCOL_SPC_UNSPECIFIED && !full
		&& frame->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
	const int *coefficients = sws_getCoefficients(space);
	sws_setColorspaceDetails(sws, coefficients, full, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
		1 << 16, 1 << 16);
}

// frame into a free picture, shown when it's due. false: a seek came first
static bool picture_put(struct player *p, AVFrame *frame, double pts, int serial,
		struct SwsContext **sws) {
	pthread_mutex_lock(&p->lock);
	struct player_picture *pic = NULL;
	while (!atomic_load(&p->quit) && serial == p->serial) {
		for (int i = 0; !pic && i < p->npictures; i++) {
			pic = p->pictures[i].state == PIC_FREE ? &p->pictures[i] : NULL;
		}
		if (pic) {
			break;
		}
		pthread_cond_wait(&p->cond, &p->lock);
	}
	if (!pic) {
		pthread_mutex_unlock(&p->lock);
		return false;
	}
	pic->state = PIC_FILLING;
	pthread_mutex_unlock(&p->lock);

	bool made = false;
	if (p->gpu) {
		// on the gpu: our pass into the picture's image
		made = gpu_convert(p->gpu, frame, (int)(pic - p->pictures));
	} else if (pic->pixels) {
		bool full = false;
		enum AVPixelFormat format = plain_format(frame->format, &full);
		*sws = sws_getCachedContext(*sws, frame->width, frame->height, format, p->width,
			p->height, AV_PIX_FMT_BGR0, SWS_BICUBIC, NULL, NULL, NULL);
		if (*sws) {
			colours(*sws, frame);
			uint8_t *dst[4] = {pic->pixels};
			int dst_stride[4] = {pic->stride};
			sws_scale(*sws, (const uint8_t *const *)frame->data, frame->linesize, 0,
				frame->height, dst, dst_stride);
			made = true;
		}
	}

	pthread_mutex_lock(&p->lock);
	if (serial == p->serial && made) {
		pic->pts = pts;
		pic->serial = serial;
		pic->state = PIC_READY;
	} else {
		pic->state = PIC_FREE;
	}
	pthread_mutex_unlock(&p->lock);
	// no wake: the window looks for frames every refresh it plays (paused
	// after a seek too, player_seeking), its own drawing isn't for them
	return true;
}

static double frame_pts(const struct player *p, const AVFrame *frame, AVRational tb,
		double fallback) {
	int64_t ts = frame->best_effort_timestamp;
	return ts == AV_NOPTS_VALUE ? fallback : (double)ts * av_q2d(tb) - p->start;
}

static void *vdecoder(void *data) {
	struct player *p = data;
	AVFrame *frame = av_frame_alloc();
	struct SwsContext *sws = NULL;
	AVRational tb = p->fmt->streams[p->vstream]->time_base;
	int serial = 0;
	double target = 0, last = 0;
	// after a seek: nothing's shown before a keyframe. how many broken
	// frames went by unshown, a second's worth at most
	bool need_key = false;
	int skipped = 0;
	if (p->cover) {
		// a song's cover: one picture, shown for all of it
		// its one packet, then the end: a threaded decoder holds the frame
		// back till it knows there's no more coming
		AVPacket *pic = &p->fmt->streams[p->vstream]->attached_pic;
		if (avcodec_send_packet(p->vctx, pic) >= 0 && avcodec_send_packet(p->vctx, NULL) >= 0
				&& avcodec_receive_frame(p->vctx, frame) >= 0) {
			picture_put(p, frame, 0, 0, &sws);
		}
		pthread_mutex_lock(&p->lock);
		p->vdone = true;
		pthread_mutex_unlock(&p->lock);
	}
	while (!p->cover && !atomic_load(&p->quit)) {
		struct item *it = queue_get(p, &p->vq);
		if (!it) {
			break;
		}
		if (it->kind == ITEM_FLUSH) {
			avcodec_flush_buffers(p->vctx);
			serial = it->serial;
			pthread_mutex_lock(&p->lock);
			target = p->seek_to;
			pthread_mutex_unlock(&p->lock);
			need_key = true;
			item_free(it);
			continue;
		}
		if (it->serial != serial) {
			item_free(it); // from before a seek
			continue;
		}
		avcodec_send_packet(p->vctx, it->kind == ITEM_END ? NULL : it->packet);
		while (avcodec_receive_frame(p->vctx, frame) >= 0) {
			double pts = frame_pts(p, frame, tb, last + p->frame_time);
			last = pts;
			// after a seek, a frame from before the next keyframe is made of
			// pictures never decoded: on the gpu that's memory nobody wrote,
			// a yellow and green net over the video. one the decoder says is
			// broken is no better. they're not shown, a second of them at
			// most (a stream that never says keyframe still plays)
			if (frame->flags & AV_FRAME_FLAG_KEY) {
				need_key = false;
			}
			bool broken = need_key || (frame->flags & AV_FRAME_FLAG_CORRUPT)
				|| frame->decode_error_flags;
			int skip_most = p->frame_time > 0 ? (int)(1 / p->frame_time) : 60;
			if (broken && skipped < (skip_most > 24 ? skip_most : 24)) {
				skipped++;
				av_frame_unref(frame);
				continue;
			}
			if (skipped) {
				fprintf(stderr, "flameplay: %d broken frames not shown\n", skipped);
			}
			skipped = 0;
			need_key = false;
			// an exact seek: the frames on the way from the keyframe aren't shown
			if (pts + p->frame_time / 2 >= target) {
				picture_put(p, frame, pts, serial, &sws);
			}
			av_frame_unref(frame);
		}
		if (it->kind == ITEM_END) {
			avcodec_flush_buffers(p->vctx);
			pthread_mutex_lock(&p->lock);
			p->vdone |= serial == p->serial;
			pthread_mutex_unlock(&p->lock);
			wake(p);
		}
		item_free(it);
	}
	sws_freeContext(sws);
	av_frame_free(&frame);
	return NULL;
}

// ---- the sound ----

// n frames of sound at pts into the ring. false: a seek came first
static bool ring_put(struct player *p, const float *samples, size_t n, double pts, int serial) {
	pthread_mutex_lock(&p->ring_lock);
	while (!atomic_load(&p->quit) && serial == p->ring_serial && RING_FRAMES - p->ring_len < n) {
		pthread_cond_wait(&p->ring_cond, &p->ring_lock);
	}
	if (atomic_load(&p->quit) || serial != p->ring_serial) {
		pthread_mutex_unlock(&p->ring_lock);
		return false;
	}
	if (!p->ring_len) {
		p->ring_pts = pts;
	}
	for (size_t i = 0; i < n; i++) {
		size_t at = (p->ring_read + p->ring_len + i) % RING_FRAMES;
		p->ring[at * CHANNELS] = samples[i * CHANNELS];
		p->ring[at * CHANNELS + 1] = samples[i * CHANNELS + 1];
	}
	p->ring_len += n;
	pthread_mutex_unlock(&p->ring_lock);
	return true;
}

static struct SwrContext *resampler(struct player *p) {
	struct SwrContext *swr = NULL;
	AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
	if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, RATE, &p->actx->ch_layout,
			p->actx->sample_fmt, p->actx->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0) {
		swr_free(&swr);
	}
	return swr;
}

static void *adecoder(void *data) {
	struct player *p = data;
	AVFrame *frame = av_frame_alloc();
	struct SwrContext *swr = resampler(p);
	int stream = p->astream; // what actx decodes
	AVRational tb = p->fmt->streams[stream]->time_base;
	float *out = NULL;
	int out_cap = 0, serial = 0;
	double target = 0, next = 0;
	while (swr && !atomic_load(&p->quit)) {
		struct item *it = queue_get(p, &p->aq);
		if (!it) {
			break;
		}
		if (it->kind == ITEM_FLUSH) {
			pthread_mutex_lock(&p->lock);
			if (p->actx_next && p->astream != stream) {
				// another track: its own decoder, its packets from here on
				avcodec_free_context(&p->actx);
				p->actx = p->actx_next;
				p->actx_next = NULL;
				stream = p->astream;
				tb = p->fmt->streams[stream]->time_base;
			}
			pthread_mutex_unlock(&p->lock);
			avcodec_flush_buffers(p->actx);
			swr_free(&swr);
			swr = resampler(p); // nothing of before the seek left in it
			serial = it->serial;
			pthread_mutex_lock(&p->lock);
			target = p->seek_to;
			pthread_mutex_unlock(&p->lock);
			next = target;
			item_free(it);
			continue;
		}
		if (it->serial != serial) {
			item_free(it);
			continue;
		}
		avcodec_send_packet(p->actx, it->kind == ITEM_END ? NULL : it->packet);
		while (avcodec_receive_frame(p->actx, frame) >= 0) {
			double pts = frame_pts(p, frame, tb, next);
			int want = swr_get_out_samples(swr, frame->nb_samples);
			if (want > out_cap) {
				float *grown = realloc(out, (size_t)want * CHANNELS * sizeof(float));
				if (!grown) {
					av_frame_unref(frame);
					continue;
				}
				out = grown;
				out_cap = want;
			}
			uint8_t *dst[1] = {(uint8_t *)out};
			int n = swr_convert(swr, dst, want, (const uint8_t **)frame->extended_data,
				frame->nb_samples);
			av_frame_unref(frame);
			if (n <= 0) {
				continue;
			}
			next = pts + (double)n / RATE;
			// an exact seek: the sound before it's cut off
			int skip = pts < target ? (int)((target - pts) * RATE) : 0;
			if (skip >= n) {
				continue;
			}
			ring_put(p, out + (size_t)skip * CHANNELS, (size_t)(n - skip),
				pts + (double)skip / RATE, serial);
		}
		if (it->kind == ITEM_END) {
			avcodec_flush_buffers(p->actx);
			pthread_mutex_lock(&p->lock);
			p->adone |= serial == p->serial;
			pthread_mutex_unlock(&p->lock);
			wake(p);
		}
		item_free(it);
	}
	free(out);
	swr_free(&swr);
	av_frame_free(&frame);
	return NULL;
}

// pipewire wants sound: what's in the ring, silence for the rest. the
// clock is set by it: this buffer's first sound is heard once what's queued
// ahead of it has played
static void on_process(void *data) {
	struct player *p = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(p->stream);
	if (!b) {
		return;
	}
	struct spa_data *d = &b->buffer->datas[0];
	float *dst = d->data;
	if (!dst) {
		pw_stream_queue_buffer(p->stream, b);
		return;
	}
	size_t stride = CHANNELS * sizeof(float);
	size_t n = d->maxsize / stride;
	if (b->requested && b->requested < n) {
		n = b->requested;
	}
	struct pw_time t;
	double delay = 0;
	if (pw_stream_get_time_n(p->stream, &t, sizeof(t)) == 0 && t.rate.denom) {
		delay = (double)t.delay * t.rate.num / t.rate.denom;
	}
	size_t took = 0;
	double heard = 0;
	int serial = 0;
	pthread_mutex_lock(&p->ring_lock);
	if (atomic_load(&p->sounding)) {
		took = p->ring_len < n ? p->ring_len : n;
		heard = p->ring_pts;
		serial = p->ring_serial;
		float volume = (float)p->volume;
		for (size_t i = 0; i < took; i++) {
			size_t at = (p->ring_read + i) % RING_FRAMES;
			dst[i * CHANNELS] = p->ring[at * CHANNELS] * volume;
			dst[i * CHANNELS + 1] = p->ring[at * CHANNELS + 1] * volume;
		}
		p->ring_read = (p->ring_read + took) % RING_FRAMES;
		p->ring_len -= took;
		p->ring_pts += (double)took / RATE;
		pthread_cond_broadcast(&p->ring_cond);
	}
	pthread_mutex_unlock(&p->ring_lock);
	memset(dst + took * CHANNELS, 0, (n - took) * stride);
	d->chunk->offset = 0;
	d->chunk->stride = (int32_t)stride;
	d->chunk->size = (uint32_t)(n * stride);
	pw_stream_queue_buffer(p->stream, b);
	if (took) {
		pthread_mutex_lock(&p->lock);
		if (serial == p->serial && p->playing) {
			p->clock_pts = heard;
			p->clock_at = now() + delay;
		}
		pthread_mutex_unlock(&p->lock);
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

static bool sound_start(struct player *p) {
	pw_init(NULL, NULL);
	p->loop = pw_thread_loop_new("flameplay-sound", NULL);
	if (!p->loop) {
		return false;
	}
	struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Movie",
		PW_KEY_APP_NAME, "flameplay", PW_KEY_NODE_NAME, "flameplay",
		PW_KEY_NODE_LATENCY, "1024/48000", NULL);
	p->stream = pw_stream_new_simple(pw_thread_loop_get_loop(p->loop), "flameplay", props,
		&stream_events, p);
	if (!p->stream) {
		return false;
	}
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[1] = {
		spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &SPA_AUDIO_INFO_RAW_INIT(
			.format = SPA_AUDIO_FORMAT_F32, .channels = CHANNELS, .rate = RATE,
			.position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR})),
	};
	if (pw_stream_connect(p->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
			params, 1) < 0) {
		return false;
	}
	return pw_thread_loop_start(p->loop) == 0;
}

static void sound_stop(struct player *p) {
	if (p->loop) {
		pw_thread_loop_stop(p->loop);
	}
	if (p->stream) {
		pw_stream_destroy(p->stream);
	}
	if (p->loop) {
		pw_thread_loop_destroy(p->loop);
	}
	p->stream = NULL;
	p->loop = NULL;
}

// ---- open, close ----

static AVCodecContext *decoder_for(AVStream *stream, int threads) {
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
	if (!ctx || avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
		avcodec_free_context(&ctx);
		return NULL;
	}
	ctx->thread_count = threads;
	ctx->pkt_timebase = stream->time_base;
	if (avcodec_open2(ctx, codec, NULL) < 0) {
		avcodec_free_context(&ctx);
	}
	return ctx;
}

// the decoder's frames on the gpu when it can (vulkan), else whatever it
// decodes to on the cpu
static enum AVPixelFormat pick_format(AVCodecContext *ctx, const enum AVPixelFormat *formats) {
	for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++) {
		if (*f == AV_PIX_FMT_VULKAN && ctx->hw_device_ctx) {
			return *f;
		}
	}
	for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++) {
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*f);
		if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
			return *f;
		}
	}
	return formats[0];
}

// back to the start of the file, nothing of what was read kept
static void rewind_file(struct player *p) {
	int64_t start = p->fmt->start_time != AV_NOPTS_VALUE ? p->fmt->start_time : 0;
	avformat_seek_file(p->fmt, -1, INT64_MIN, start, start, 0);
	if (p->vctx) {
		avcodec_flush_buffers(p->vctx);
	}
}

// the video decoded on the gpu: a vulkan decoder, and its first frame
// really coming out as one (a codec or profile the gpu can't do comes out
// on the cpu, and then it's decoded there all along)
// a decoder for the codec that decodes on vulkan: not always the one ffmpeg
// likes best (av1's is dav1d, cpu only)
static const AVCodec *vulkan_decoder(enum AVCodecID id) {
	void *it = NULL;
	for (const AVCodec *codec; (codec = av_codec_iterate(&it));) {
		if (codec->id != id || !av_codec_is_decoder(codec)) {
			continue;
		}
		for (int i = 0;; i++) {
			const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
			if (!config) {
				break;
			}
			if (config->device_type == AV_HWDEVICE_TYPE_VULKAN
					&& config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
				return codec;
			}
		}
	}
	return NULL;
}

static bool open_gpu(struct player *p, AVStream *vs) {
	AVBufferRef *device = NULL;
	const AVCodec *codec = vulkan_decoder(vs->codecpar->codec_id);
	if (!codec) {
		return false;
	}
	struct gpu *gpu = gpu_create(&device);
	AVCodecContext *ctx = gpu ? avcodec_alloc_context3(codec) : NULL;
	if (!ctx || avcodec_parameters_to_context(ctx, vs->codecpar) < 0) {
		avcodec_free_context(&ctx);
		av_buffer_unref(&device);
		gpu_destroy(gpu);
		return false;
	}
	ctx->hw_device_ctx = device;
	ctx->get_format = pick_format;
	ctx->pkt_timebase = vs->time_base;
	// the gpu not taking it (a profile it can't, 10-bit h264...) is no
	// error: it's decoded on the cpu, ffmpeg's complaints kept to debug
	ctx->log_level_offset = AV_LOG_DEBUG - AV_LOG_ERROR;
	bool on_gpu = false;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	if (packet && frame && avcodec_open2(ctx, codec, NULL) >= 0) {
		bool got = false;
		for (int n = 0; !got && n < 500 && av_read_frame(p->fmt, packet) >= 0; n++) {
			if (packet->stream_index == vs->index && avcodec_send_packet(ctx, packet) >= 0) {
				got = avcodec_receive_frame(ctx, frame) >= 0;
			}
			av_packet_unref(packet);
		}
		if (!got && avcodec_send_packet(ctx, NULL) >= 0) {
			got = avcodec_receive_frame(ctx, frame) >= 0;
		}
		on_gpu = got && frame->format == AV_PIX_FMT_VULKAN;
	}
	av_frame_free(&frame);
	av_packet_free(&packet);
	if (!on_gpu) {
		avcodec_free_context(&ctx);
		gpu_destroy(gpu);
		rewind_file(p);
		return false;
	}
	ctx->log_level_offset = 0;
	p->vctx = ctx;
	p->gpu = gpu;
	rewind_file(p);
	return true;
}

bool player_gpu(const struct player *p) {
	return p->gpu != NULL;
}

bool player_gpu_images(struct player *p, int count, struct gpu_image *out) {
	return p->gpu && gpu_images(p->gpu, p->width, p->height, count, out);
}

void player_use_software(struct player *p) {
	if (!p->gpu) {
		return;
	}
	avcodec_free_context(&p->vctx);
	gpu_destroy(p->gpu);
	p->gpu = NULL;
	p->vctx = decoder_for(p->fmt->streams[p->vstream], 0);
	rewind_file(p);
	if (!p->vctx) {
		p->vstream = -1;
	}
}

// ---- tracks ----

// a language's name from its code (iso 639: "en", "eng", "ger"...), NULL
// for one it doesn't know
static const char *language(const char *code) {
	static const char *const names[][3] = {
		{"en", "eng", "English"}, {"es", "spa", "Spanish"}, {"fr", "fre", "French"},
		{"fr", "fra", "French"}, {"de", "ger", "German"}, {"de", "deu", "German"},
		{"it", "ita", "Italian"}, {"pt", "por", "Portuguese"}, {"ru", "rus", "Russian"},
		{"ja", "jpn", "Japanese"}, {"zh", "chi", "Chinese"}, {"zh", "zho", "Chinese"},
		{"ko", "kor", "Korean"}, {"ar", "ara", "Arabic"}, {"hi", "hin", "Hindi"},
		{"tr", "tur", "Turkish"}, {"nl", "dut", "Dutch"}, {"nl", "nld", "Dutch"},
		{"sv", "swe", "Swedish"}, {"no", "nor", "Norwegian"}, {"nb", "nob", "Norwegian"},
		{"da", "dan", "Danish"}, {"fi", "fin", "Finnish"}, {"pl", "pol", "Polish"},
		{"cs", "cze", "Czech"}, {"cs", "ces", "Czech"}, {"el", "gre", "Greek"},
		{"el", "ell", "Greek"}, {"he", "heb", "Hebrew"}, {"hu", "hun", "Hungarian"},
		{"ro", "rum", "Romanian"}, {"ro", "ron", "Romanian"}, {"uk", "ukr", "Ukrainian"},
		{"th", "tha", "Thai"}, {"vi", "vie", "Vietnamese"}, {"id", "ind", "Indonesian"},
		{"ms", "may", "Malay"}, {"ms", "msa", "Malay"}, {"fa", "per", "Persian"},
		{"fa", "fas", "Persian"}, {"ur", "urd", "Urdu"}, {"bn", "ben", "Bengali"},
		{"ta", "tam", "Tamil"}, {"bg", "bul", "Bulgarian"}, {"hr", "hrv", "Croatian"},
		{"sr", "srp", "Serbian"}, {"sk", "slo", "Slovak"}, {"sk", "slk", "Slovak"},
		{"sl", "slv", "Slovenian"}, {"ca", "cat", "Catalan"}, {"eu", "baq", "Basque"},
		{"gl", "glg", "Galician"}, {"is", "ice", "Icelandic"}, {"et", "est", "Estonian"},
		{"lv", "lav", "Latvian"}, {"lt", "lit", "Lithuanian"}, {"tl", "tgl", "Tagalog"},
	};
	if (!code) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!strcasecmp(code, names[i][0]) || !strcasecmp(code, names[i][1])
				|| !strcasecmp(code, names[i][2])) {
			return names[i][2];
		}
	}
	return NULL;
}

// a stream's name to pick it by: its language and its own title (one of
// them if they say the same), how many channels a sound has
static void stream_name(AVStream *st, int n, char *out, size_t size) {
	const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
	const AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
	const char *l = lang ? language(lang->value) : NULL;
	const char *t = title && title->value[0] ? title->value : NULL;
	if (l && t && !strcasestr(t, l)) {
		snprintf(out, size, "%s — %s", l, t);
	} else if (t || l) {
		snprintf(out, size, "%s", t ? t : l);
	} else {
		snprintf(out, size, "Track %d", n);
	}
	if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
		int ch = st->codecpar->ch_layout.nb_channels;
		const char *layout = ch == 1 ? "Mono" : ch == 2 ? "Stereo" : ch == 6 ? "5.1"
			: ch == 8 ? "7.1" : NULL;
		size_t len = strlen(out);
		if (layout && len < size) {
			snprintf(out + len, size - len, " · %s", layout);
		}
	}
	if (st->disposition & AV_DISPOSITION_FORCED) {
		size_t len = strlen(out);
		if (len < size) {
			snprintf(out + len, size - len, " (Forced)");
		}
	}
}

// subtitle files beside path, named like it: movie.srt, movie.en.srt
static void find_subtitle_files(struct player *p, const char *path) {
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	const char *file = slash ? slash + 1 : path;
	if (slash) {
		*slash = '\0';
	} else {
		snprintf(dir, sizeof(dir), ".");
	}
	char base[NAME_MAX + 1];
	snprintf(base, sizeof(base), "%s", file);
	char *dot = strrchr(base, '.');
	if (dot && dot != base) {
		*dot = '\0';
	}
	size_t base_len = strlen(base);
	DIR *d = opendir(slash ? (dir[0] ? dir : "/") : ".");
	for (struct dirent *e; d && (e = readdir(d)) && p->nsubtitles < TRACKS;) {
		const char *ext = strrchr(e->d_name, '.');
		if (!ext || strncmp(e->d_name, base, base_len) || e->d_name[base_len] != '.'
				|| (strcasecmp(ext, ".srt") && strcasecmp(ext, ".ass") && strcasecmp(ext, ".ssa")
				&& strcasecmp(ext, ".vtt"))) {
			continue;
		}
		struct track *t = &p->subtitles[p->nsubtitles];
		size_t len = strlen(dir) + strlen(e->d_name) + 2;
		t->file = malloc(len);
		if (!t->file) {
			break;
		}
		snprintf(t->file, len, "%s/%s", slash ? (dir[0] ? dir : "") : ".", e->d_name);
		t->stream = -1;
		// its language, between the name and the extension, else its name
		char middle[64] = "";
		const char *after = e->d_name + base_len + 1;
		if (after < ext) {
			snprintf(middle, sizeof(middle), "%.*s", (int)(ext - after), after);
		}
		const char *l = language(middle);
		snprintf(t->name, sizeof(t->name), "%s", l ? l : e->d_name);
		p->nsubtitles++;
	}
	if (d) {
		closedir(d);
	}
}

static int by_file(const void *a, const void *b) {
	return strcmp(((const struct track *)a)->file, ((const struct track *)b)->file);
}

// subtitles named alike (an .ass and an .srt of one language): what kind
// each is after its name
static void tell_apart(struct player *p) {
	for (int i = 0; i < p->nsubtitles; i++) {
		bool same = false;
		for (int j = 0; j < p->nsubtitles; j++) {
			same |= j != i && !strcmp(p->subtitles[i].name, p->subtitles[j].name);
		}
		if (!same) {
			continue;
		}
		struct track *t = &p->subtitles[i];
		char kind[16] = "";
		if (t->file) {
			const char *ext = strrchr(t->file, '.');
			snprintf(kind, sizeof(kind), "%s", ext ? ext + 1 : "");
		} else {
			snprintf(kind, sizeof(kind), "%s",
				avcodec_get_name(p->fmt->streams[t->stream]->codecpar->codec_id));
		}
		for (char *c = kind; *c; c++) {
			*c = (char)toupper((unsigned char)*c);
		}
		size_t len = strlen(t->name);
		if (kind[0] && len < sizeof(t->name)) {
			snprintf(t->name + len, sizeof(t->name) - len, " · %s", kind);
		}
	}
}

static void find_tracks(struct player *p, const char *path) {
	for (unsigned i = 0; i < p->fmt->nb_streams; i++) {
		AVStream *st = p->fmt->streams[i];
		enum AVMediaType type = st->codecpar->codec_type;
		// sounds to pick when there's sound at all (pipewire)
		if (type == AVMEDIA_TYPE_AUDIO && p->astream >= 0 && p->naudio < TRACKS) {
			struct track *t = &p->audio[p->naudio];
			t->stream = (int)i;
			stream_name(st, p->naudio + 1, t->name, sizeof(t->name));
			p->audio_on = (int)i == p->astream ? p->naudio : p->audio_on;
			p->naudio++;
		} else if (type == AVMEDIA_TYPE_SUBTITLE && p->nsubtitles < TRACKS
				&& avcodec_find_decoder(st->codecpar->codec_id)) {
			struct track *t = &p->subtitles[p->nsubtitles];
			t->stream = (int)i;
			stream_name(st, p->nsubtitles + 1, t->name, sizeof(t->name));
			p->nsubtitles++;
		}
	}
	int streams = p->nsubtitles;
	find_subtitle_files(p, path);
	// the files in name order (the full .ass before the plain .srt)
	qsort(p->subtitles + streams, (size_t)(p->nsubtitles - streams), sizeof(struct track), by_file);
	tell_apart(p);
}

// the encoding of a text file that isn't utf-8 (old .srt files: windows'
// arabic, cyrillic, western...), found by what its bytes look like
// (uchardet). false: utf-8, or utf-16 saying so, which ffmpeg reads itself
static bool text_encoding(const char *file, char *out, size_t size) {
	FILE *f = fopen(file, "rbe");
	if (!f) {
		return false;
	}
	static char buf[1 << 20];
	size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	if (n >= 2 && (((unsigned char)buf[0] == 0xff && (unsigned char)buf[1] == 0xfe)
			|| ((unsigned char)buf[0] == 0xfe && (unsigned char)buf[1] == 0xff))) {
		return false;
	}
	bool utf8 = true;
	for (size_t i = 0, follow = 0; utf8 && i < n; i++) {
		unsigned char c = (unsigned char)buf[i];
		if (follow) {
			utf8 = (c & 0xc0) == 0x80;
			follow--;
		} else if (c >= 0x80) {
			follow = c >= 0xf0 && c < 0xf8 ? 3 : c >= 0xe0 && c < 0xf0 ? 2
				: c >= 0xc2 && c < 0xe0 ? 1 : 0;
			// cut off at the end of what was read: still utf-8
			utf8 = follow > 0 && (i + follow < n || n == sizeof(buf));
		}
	}
	if (utf8) {
		return false;
	}
	uchardet_t detect = uchardet_new();
	const char *found = NULL;
	if (detect && uchardet_handle_data(detect, buf, n) == 0) {
		uchardet_data_end(detect);
		found = uchardet_get_charset(detect);
	}
	// nothing it could tell: windows' western, what most are
	snprintf(out, size, "%s", found && *found && strcasecmp(found, "ASCII") ? found : "CP1252");
	if (detect) {
		uchardet_delete(detect);
	}
	return true;
}

// a subtitle file read whole into subs, under sub_lock
static bool load_subtitle_file(struct player *p, const char *file) {
	AVFormatContext *f = NULL;
	if (avformat_open_input(&f, file, NULL, NULL) < 0) {
		return false;
	}
	int si = avformat_find_stream_info(f, NULL) >= 0
		? av_find_best_stream(f, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0) : -1;
	const AVCodec *codec = si >= 0 ? avcodec_find_decoder(f->streams[si]->codecpar->codec_id)
		: NULL;
	AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
	AVDictionary *options = NULL;
	char encoding[64];
	if (text_encoding(file, encoding, sizeof(encoding))) {
		av_dict_set(&options, "sub_charenc", encoding, 0);
	}
	bool ok = ctx && avcodec_parameters_to_context(ctx, f->streams[si]->codecpar) >= 0;
	if (ok) {
		ctx->pkt_timebase = f->streams[si]->time_base;
		ok = avcodec_open2(ctx, codec, &options) >= 0;
	}
	av_dict_free(&options);
	if (ok) {
		bool styled = codec->id == AV_CODEC_ID_ASS || codec->id == AV_CODEC_ID_SSA;
		subs_reset(p->subs, styled ? (const char *)ctx->subtitle_header : NULL, p->width,
			p->height);
		AVPacket *packet = av_packet_alloc();
		AVRational tb = f->streams[si]->time_base;
		while (packet && av_read_frame(f, packet) >= 0) {
			AVSubtitle sub;
			int got = 0;
			if (packet->stream_index == si && avcodec_decode_subtitle2(ctx, &sub, &got, packet) >= 0
					&& got) {
				// beside the video: its times are the video's from its start
				subs_add(p->subs, &sub, packet->pts != AV_NOPTS_VALUE ? packet->pts * av_q2d(tb) : 0,
					ctx->width, ctx->height);
				avsubtitle_free(&sub);
			}
			av_packet_unref(packet);
		}
		av_packet_free(&packet);
	}
	avcodec_free_context(&ctx);
	avformat_close_input(&f);
	return ok;
}

// fonts the file brings (a styled track's), for libass
static void add_fonts(struct player *p) {
	for (unsigned i = 0; i < p->fmt->nb_streams; i++) {
		AVStream *st = p->fmt->streams[i];
		const AVDictionaryEntry *name = av_dict_get(st->metadata, "filename", NULL, 0);
		const AVDictionaryEntry *mime = av_dict_get(st->metadata, "mimetype", NULL, 0);
		bool font = st->codecpar->codec_id == AV_CODEC_ID_TTF
			|| st->codecpar->codec_id == AV_CODEC_ID_OTF || (mime && strstr(mime->value, "font"));
		if (st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT && font && name
				&& st->codecpar->extradata_size > 0) {
			subs_add_font(p->subs, name->value, st->codecpar->extradata,
				(size_t)st->codecpar->extradata_size);
		}
	}
}

// subtitles track i on (-1: off). refresh: what's read already read again
// (a stream's subtitles come as the file's read)
static void subtitles_on(struct player *p, int i, bool refresh) {
	pthread_mutex_lock(&p->sub_lock);
	avcodec_free_context(&p->sctx);
	p->sstream = -1;
	p->subtitles_on = -1;
	if (i >= 0 && i < p->nsubtitles && !p->subs && (p->subs = subs_create())) {
		add_fonts(p);
	}
	bool seek = false;
	if (i >= 0 && i < p->nsubtitles && p->subs) {
		struct track *t = &p->subtitles[i];
		if (t->stream >= 0) {
			AVStream *st = p->fmt->streams[t->stream];
			AVCodecContext *ctx = decoder_for(st, 0);
			if (ctx) {
				bool styled = ctx->codec_id == AV_CODEC_ID_ASS || ctx->codec_id == AV_CODEC_ID_SSA;
				subs_reset(p->subs, styled ? (const char *)ctx->subtitle_header : NULL, p->width,
					p->height);
				p->sctx = ctx;
				p->sstream = t->stream;
				p->subtitles_on = i;
				seek = refresh;
			}
		} else if (load_subtitle_file(p, t->file)) {
			p->subtitles_on = i;
		}
	}
	pthread_mutex_unlock(&p->sub_lock);
	if (seek) {
		player_seek(p, player_time(p));
	}
}

int player_tracks(struct player *p, enum player_track_kind kind, struct player_track *out,
		int max) {
	pthread_mutex_lock(&p->sub_lock);
	bool audio = kind == PLAYER_AUDIO;
	int n = audio ? p->naudio : p->nsubtitles, on = audio ? p->audio_on : p->subtitles_on;
	int i = 0;
	for (; i < n && i < max; i++) {
		snprintf(out[i].name, sizeof(out[i].name), "%s",
			audio ? p->audio[i].name : p->subtitles[i].name);
		out[i].on = i == on;
	}
	pthread_mutex_unlock(&p->sub_lock);
	return i;
}

void player_set_track(struct player *p, enum player_track_kind kind, int i) {
	if (kind == PLAYER_SUBTITLES) {
		subtitles_on(p, i, true);
		return;
	}
	if (i < 0 || i >= p->naudio || i == p->audio_on || p->astream < 0) {
		return;
	}
	// its decoder made now (a track that can't be decoded isn't taken), the
	// sound's from a seek to here: the reader and the decoder change over
	AVCodecContext *ctx = decoder_for(p->fmt->streams[p->audio[i].stream], 1);
	if (!ctx) {
		return;
	}
	pthread_mutex_lock(&p->lock);
	avcodec_free_context(&p->actx_next);
	p->actx_next = ctx;
	p->astream_next = p->audio[i].stream;
	pthread_mutex_unlock(&p->lock);
	pthread_mutex_lock(&p->sub_lock);
	p->audio_on = i;
	pthread_mutex_unlock(&p->sub_lock);
	player_seek(p, player_time(p));
}

struct subs *player_subs(struct player *p) {
	pthread_mutex_lock(&p->sub_lock);
	struct subs *subs = p->subtitles_on >= 0 ? p->subs : NULL;
	pthread_mutex_unlock(&p->sub_lock);
	return subs;
}

struct player *player_open(const char *path, void (*wake_f)(void *data), void *data,
		char *error, size_t error_size) {
	struct player *p = calloc(1, sizeof(*p));
	if (!p) {
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	p->wake = wake_f;
	p->wake_data = data;
	p->vstream = p->astream = p->sstream = p->astream_next = -1;
	p->audio_on = p->subtitles_on = -1;
	p->volume = 1;
	p->frame_time = 1.0 / 30;
	queue_init(&p->vq);
	queue_init(&p->aq);
	pthread_mutex_init(&p->lock, NULL);
	pthread_cond_init(&p->cond, NULL);
	pthread_mutex_init(&p->ring_lock, NULL);
	pthread_cond_init(&p->ring_cond, NULL);
	pthread_mutex_init(&p->sub_lock, NULL);
	if (avformat_open_input(&p->fmt, path, NULL, NULL) < 0
			|| avformat_find_stream_info(p->fmt, NULL) < 0) {
		snprintf(error, error_size, "not something it can play");
		player_close(p);
		return NULL;
	}
	p->start = p->fmt->start_time != AV_NOPTS_VALUE ? (double)p->fmt->start_time / AV_TIME_BASE
		: 0;
	p->duration = p->fmt->duration > 0 ? (double)p->fmt->duration / AV_TIME_BASE : 0;
	const AVDictionaryEntry *title = av_dict_get(p->fmt->metadata, "title", NULL, 0);
	p->title = title && title->value[0] ? strdup(title->value) : NULL;

	p->vstream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	p->astream = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (p->vstream >= 0) {
		AVStream *vs = p->fmt->streams[p->vstream];
		p->cover = vs->disposition & AV_DISPOSITION_ATTACHED_PIC;
		// on the gpu if it can be (FLAMEPLAY_HWDEC=no: never), else on the cpu
		// with as many threads as it likes
		const char *hwdec = getenv("FLAMEPLAY_HWDEC");
		bool gpu = !p->cover && !(hwdec && !strcmp(hwdec, "no")) && open_gpu(p, vs);
		if (!gpu) {
			p->vctx = decoder_for(vs, 0);
		}
		if (p->vctx) {
			p->width = p->vctx->width;
			p->height = p->vctx->height;
			AVRational sar = av_guess_sample_aspect_ratio(p->fmt, vs, NULL);
			double s = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1;
			p->aspect = p->height ? p->width * s / p->height : 16.0 / 9;
			AVRational fr = av_guess_frame_rate(p->fmt, vs, NULL);
			p->frame_time = fr.num > 0 && fr.den > 0 ? 1 / av_q2d(fr) : 1.0 / 30;
		}
		if (!p->vctx || p->width <= 0 || p->height <= 0) {
			avcodec_free_context(&p->vctx);
			p->vstream = -1;
		}
	}
	if (p->astream >= 0) {
		p->actx = decoder_for(p->fmt->streams[p->astream], 1);
		p->ring = calloc(RING_FRAMES * CHANNELS, sizeof(float));
		if (!p->actx || !p->ring || !sound_start(p)) {
			// no sound to be had (no pipewire): it plays silent, on the wall clock
			sound_stop(p);
			avcodec_free_context(&p->actx);
			p->astream = -1;
		}
	}
	if (p->vstream < 0 && p->astream < 0) {
		snprintf(error, error_size, "no picture or sound in it it can play");
		player_close(p);
		return NULL;
	}
	find_tracks(p, path);
	// subtitles on from the start: a file put beside it, else what the file
	// says has to be (forced: what's said in another language)
	int first = -1;
	for (int i = 0; p->vstream >= 0 && !p->cover && i < p->nsubtitles; i++) {
		struct track *t = &p->subtitles[i];
		if (t->stream < 0 || (first < 0
				&& p->fmt->streams[t->stream]->disposition & AV_DISPOSITION_FORCED)) {
			first = i;
			if (t->stream < 0) {
				break;
			}
		}
	}
	if (first >= 0) {
		subtitles_on(p, first, false);
	}
	return p;
}

void player_set_pictures(struct player *p, struct player_picture *pictures, int count) {
	pthread_mutex_lock(&p->lock);
	p->pictures = pictures;
	p->npictures = count;
	for (int i = 0; i < count; i++) {
		pictures[i].state = PIC_FREE;
	}
	pthread_mutex_unlock(&p->lock);
	// it starts now: reading, decoding, filling
	p->reader_up = !pthread_create(&p->reader, NULL, reader, p);
	if (p->vstream >= 0) {
		p->vdecoder_up = !pthread_create(&p->vdecoder, NULL, vdecoder, p);
	} else {
		p->vdone = true;
	}
	if (p->astream >= 0) {
		p->adecoder_up = !pthread_create(&p->adecoder, NULL, adecoder, p);
	} else {
		p->adone = true;
	}
}

void player_close(struct player *p) {
	if (!p) {
		return;
	}
	atomic_store(&p->quit, true);
	pthread_mutex_lock(&p->lock);
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->lock);
	struct queue *qs[] = {&p->vq, &p->aq};
	for (int i = 0; i < 2; i++) {
		pthread_mutex_lock(&qs[i]->lock);
		pthread_cond_broadcast(&qs[i]->cond);
		pthread_mutex_unlock(&qs[i]->lock);
	}
	pthread_mutex_lock(&p->ring_lock);
	pthread_cond_broadcast(&p->ring_cond);
	pthread_mutex_unlock(&p->ring_lock);
	if (p->reader_up) {
		pthread_join(p->reader, NULL);
	}
	if (p->vdecoder_up) {
		pthread_join(p->vdecoder, NULL);
	}
	if (p->adecoder_up) {
		pthread_join(p->adecoder, NULL);
	}
	sound_stop(p);
	avcodec_free_context(&p->vctx);
	gpu_destroy(p->gpu); // after the decoder: its frames were the gpu's
	avcodec_free_context(&p->actx);
	avcodec_free_context(&p->actx_next);
	avcodec_free_context(&p->sctx);
	subs_destroy(p->subs);
	for (int i = 0; i < p->nsubtitles; i++) {
		free(p->subtitles[i].file);
	}
	avformat_close_input(&p->fmt);
	queue_finish(&p->vq);
	queue_finish(&p->aq);
	free(p->ring);
	free(p->title);
	pthread_mutex_destroy(&p->lock);
	pthread_cond_destroy(&p->cond);
	pthread_mutex_destroy(&p->ring_lock);
	pthread_cond_destroy(&p->ring_cond);
	pthread_mutex_destroy(&p->sub_lock);
	free(p);
}

// ---- what it is ----

void player_size(const struct player *p, int *width, int *height) {
	*width = p->width;
	*height = p->height;
}

double player_aspect(const struct player *p) {
	return p->aspect > 0 ? p->aspect : 16.0 / 9;
}

double player_duration(const struct player *p) {
	return p->duration;
}

bool player_has_video(const struct player *p) {
	return p->vstream >= 0;
}

bool player_has_cover(const struct player *p) {
	return p->cover;
}

bool player_has_audio(const struct player *p) {
	return p->astream >= 0;
}

const char *player_title(const struct player *p) {
	return p->title;
}

// ---- playing ----

void player_play(struct player *p) {
	if (player_ended(p)) {
		player_seek(p, 0); // played to the end: again from the start
	}
	pthread_mutex_lock(&p->lock);
	if (!p->playing) {
		p->playing = true;
		p->clock_at = now();
	}
	pthread_mutex_unlock(&p->lock);
	atomic_store(&p->sounding, true);
}

void player_pause(struct player *p) {
	pthread_mutex_lock(&p->lock);
	if (p->playing) {
		p->clock_pts = clock_locked(p);
		p->playing = false;
	}
	pthread_mutex_unlock(&p->lock);
	atomic_store(&p->sounding, false);
}

bool player_playing(const struct player *p) {
	pthread_mutex_lock((pthread_mutex_t *)&p->lock);
	bool playing = p->playing;
	pthread_mutex_unlock((pthread_mutex_t *)&p->lock);
	return playing;
}

bool player_seeking(const struct player *p) {
	struct player *q = (struct player *)p;
	pthread_mutex_lock(&q->lock);
	bool seeking = !q->shown_since_seek && !q->vdone;
	pthread_mutex_unlock(&q->lock);
	return seeking;
}

bool player_ended(const struct player *p) {
	struct player *q = (struct player *)p;
	pthread_mutex_lock(&q->lock);
	bool done = q->reader_eof && q->vdone && q->adone && !q->seek_wanted;
	for (int i = 0; done && !q->cover && i < q->npictures; i++) {
		done = q->pictures[i].state != PIC_READY && q->pictures[i].state != PIC_FILLING;
	}
	double t = clock_locked(q);
	pthread_mutex_unlock(&q->lock);
	pthread_mutex_lock(&q->ring_lock);
	done = done && q->ring_len == 0;
	pthread_mutex_unlock(&q->ring_lock);
	// what's left of the clock: the last frame's own time on screen (a
	// song's cover: the song's to the end)
	double left = q->cover ? 0.05 : q->frame_time + 0.05;
	return done && (q->duration <= 0 || t >= q->duration - left);
}

void player_seek(struct player *p, double t) {
	t = t < 0 ? 0 : p->duration > 0 && t > p->duration ? p->duration : t;
	pthread_mutex_lock(&p->lock);
	p->serial++;
	p->seek_to = t;
	p->seek_wanted = true;
	p->reader_eof = false;
	p->vdone = p->cover || p->vstream < 0;
	p->adone = p->astream < 0;
	p->clock_pts = t;
	p->clock_at = now();
	p->shown_since_seek = p->cover;
	// what was decoded for before is of no use: its pictures free
	for (int i = 0; i < p->npictures; i++) {
		if (!p->cover && p->pictures[i].state == PIC_READY) {
			p->pictures[i].state = PIC_FREE;
		}
	}
	int serial = p->serial;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->lock);
	pthread_mutex_lock(&p->ring_lock);
	p->ring_serial = serial;
	p->ring_len = 0;
	p->ring_pts = t;
	pthread_cond_broadcast(&p->ring_cond);
	pthread_mutex_unlock(&p->ring_lock);
}

void player_set_volume(struct player *p, double volume) {
	pthread_mutex_lock(&p->ring_lock);
	p->volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
	pthread_mutex_unlock(&p->ring_lock);
}

struct player_picture *player_due(struct player *p) {
	pthread_mutex_lock(&p->lock);
	double t = clock_locked(p);
	struct player_picture *best = NULL, *first = NULL;
	for (int i = 0; i < p->npictures; i++) {
		struct player_picture *pic = &p->pictures[i];
		if (pic->state != PIC_READY) {
			continue;
		}
		if (pic->serial != p->serial) {
			pic->state = PIC_FREE; // from before a seek
			continue;
		}
		if (pic->pts <= t + 0.004 && (!best || pic->pts > best->pts)) {
			best = pic;
		}
		if (!first || pic->pts < first->pts) {
			first = pic;
		}
	}
	// just after a seek (paused, say): the frame it went to, right away
	if (!best && !p->shown_since_seek) {
		best = first;
	}
	if (best) {
		for (int i = 0; i < p->npictures; i++) {
			struct player_picture *pic = &p->pictures[i];
			if (pic->state == PIC_READY && pic != best && pic->pts < best->pts) {
				pic->state = PIC_FREE; // late: skipped
			}
		}
		best->state = PIC_SHOWN;
		p->shown_since_seek = true;
		pthread_cond_broadcast(&p->cond);
	}
	pthread_mutex_unlock(&p->lock);
	return best;
}

void player_picture_done(struct player *p, struct player_picture *picture) {
	pthread_mutex_lock(&p->lock);
	if (picture->state == PIC_SHOWN) {
		picture->state = PIC_FREE;
		pthread_cond_broadcast(&p->cond);
	}
	pthread_mutex_unlock(&p->lock);
}
