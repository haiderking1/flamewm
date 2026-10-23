#include "player.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
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

struct player {
	AVFormatContext *fmt;
	int vstream, astream;
	bool cover; // the video's a still: a song's cover
	AVCodecContext *vctx, *actx;
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

	bool full = false;
	enum AVPixelFormat format = plain_format(frame->format, &full);
	*sws = sws_getCachedContext(*sws, frame->width, frame->height, format, p->width,
		p->height, AV_PIX_FMT_BGR0, SWS_BICUBIC, NULL, NULL, NULL);
	if (*sws) {
		colours(*sws, frame);
		uint8_t *dst[4] = {pic->pixels};
		int dst_stride[4] = {pic->stride};
		sws_scale(*sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
			dst, dst_stride);
	}

	pthread_mutex_lock(&p->lock);
	if (serial == p->serial && *sws) {
		pic->pts = pts;
		pic->serial = serial;
		pic->state = PIC_READY;
	} else {
		pic->state = PIC_FREE;
	}
	pthread_mutex_unlock(&p->lock);
	wake(p);
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
	AVRational tb = p->fmt->streams[p->astream]->time_base;
	float *out = NULL;
	int out_cap = 0, serial = 0;
	double target = 0, next = 0;
	while (swr && !atomic_load(&p->quit)) {
		struct item *it = queue_get(p, &p->aq);
		if (!it) {
			break;
		}
		if (it->kind == ITEM_FLUSH) {
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

struct player *player_open(const char *path, void (*wake_f)(void *data), void *data,
		char *error, size_t error_size) {
	struct player *p = calloc(1, sizeof(*p));
	if (!p) {
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	p->wake = wake_f;
	p->wake_data = data;
	p->vstream = p->astream = -1;
	p->volume = 1;
	p->frame_time = 1.0 / 30;
	queue_init(&p->vq);
	queue_init(&p->aq);
	pthread_mutex_init(&p->lock, NULL);
	pthread_cond_init(&p->cond, NULL);
	pthread_mutex_init(&p->ring_lock, NULL);
	pthread_cond_init(&p->ring_cond, NULL);
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
		p->vctx = decoder_for(vs, 0); // 0: as many threads as it likes
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
	avcodec_free_context(&p->actx);
	avformat_close_input(&p->fmt);
	queue_finish(&p->vq);
	queue_finish(&p->aq);
	free(p->ring);
	free(p->title);
	pthread_mutex_destroy(&p->lock);
	pthread_cond_destroy(&p->cond);
	pthread_mutex_destroy(&p->ring_lock);
	pthread_cond_destroy(&p->ring_cond);
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
