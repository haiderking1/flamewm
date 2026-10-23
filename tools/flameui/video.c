#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "internal.h"

// a video's thumbnail: a frame from about a tenth of the way in (a scene,
// not the black it starts on), its cover if it has one (music files' album
// art, a video's poster). ffmpeg's libraries read it

// how far in, at most: a three hour film's frame from three minutes in
#define SEEK_MAX_S 180
// dark frames passed over looking for a lit one, at most
#define DARK_SKIP 48

bool fui_media_name(const char *path) {
	static const char *const exts[] = {"mp4", "mkv", "webm", "mov", "avi", "m4v", "mpg", "mpeg",
		"wmv", "flv", "ts", "3gp", "ogv", "mp3", "flac", "m4a", "ogg", "opus", "aac", "wav"};
	const char *dot = strrchr(path, '.');
	for (size_t i = 0; dot && i < sizeof(exts) / sizeof(exts[0]); i++) {
		if (!strcasecmp(dot + 1, exts[i])) {
			return true;
		}
	}
	return false;
}

// how bright a frame is on average, a coarse look at its luma (yuv) or its
// green (anything else): dark ones get passed over
static int brightness(const AVFrame *frame) {
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
	if (!desc || !frame->data[0]) {
		return 255;
	}
	bool rgb = desc->flags & AV_PIX_FMT_FLAG_RGB;
	int step = rgb ? desc->comp[1].step : 1, offset = rgb ? desc->comp[1].offset : 0;
	long sum = 0, n = 0;
	for (int y = 0; y < frame->height; y += frame->height / 16 + 1) {
		const uint8_t *row = frame->data[rgb ? desc->comp[1].plane : 0] + (long)y * frame->linesize[0];
		for (int x = 0; x < frame->width; x += frame->width / 16 + 1) {
			sum += row[x * step + offset];
			n++;
		}
	}
	return n ? (int)(sum / n) : 255;
}

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

// frame into out, w x h, cairo's ARGB32 (opaque)
static bool convert(const AVFrame *frame, int w, int h, struct image *out) {
	bool full = false;
	struct SwsContext *sws = sws_getContext(frame->width, frame->height,
		plain_format(frame->format, &full), w, h, AV_PIX_FMT_BGRA, SWS_BICUBIC, NULL, NULL, NULL);
	if (!sws) {
		return false;
	}
	colours(sws, frame);
	*out = (struct image){.width = w, .height = h, .stride = w * 4};
	out->data = malloc((size_t)out->stride * (size_t)h);
	if (!out->data) {
		sws_freeContext(sws);
		return false;
	}
	uint8_t *dst[4] = {(uint8_t *)out->data};
	int dst_stride[4] = {out->stride};
	sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, dst,
		dst_stride);
	sws_freeContext(sws);
	// whatever the source said about alpha, a video's frame is opaque
	for (int i = 0; i < w * h; i++) {
		out->data[i] |= 0xff000000u;
	}
	return true;
}

bool fui_media_frame(const char *path, int max_width, int max_height, struct image *out) {
	AVFormatContext *fmt = NULL;
	if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
		return false;
	}
	bool ok = false;
	AVCodecContext *ctx = NULL;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc(), *best = av_frame_alloc();
	const AVCodec *codec = NULL;
	if (!packet || !frame || !best || avformat_find_stream_info(fmt, NULL) < 0) {
		goto out;
	}
	int index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if (index < 0 || !codec) {
		goto out;
	}
	AVStream *stream = fmt->streams[index];
	ctx = avcodec_alloc_context3(codec);
	if (!ctx || avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
		goto out;
	}
	ctx->thread_count = 1; // a thumbnail thread of its own already
	if (avcodec_open2(ctx, codec, NULL) < 0) {
		goto out;
	}
	bool cover = stream->disposition & AV_DISPOSITION_ATTACHED_PIC;
	if (cover) {
		// the cover is a picture of its own, one packet
		if (avcodec_send_packet(ctx, &stream->attached_pic) < 0
				|| avcodec_receive_frame(ctx, best) < 0) {
			goto out;
		}
	} else {
		if (fmt->duration > 0) {
			int64_t at = fmt->duration / 10;
			int64_t most = (int64_t)SEEK_MAX_S * AV_TIME_BASE;
			av_seek_frame(fmt, -1, at < most ? at : most, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(ctx);
		}
		// the first frame from there that isn't nearly black, else the
		// brightest of the ones looked at
		int looked = 0, best_light = -1, packets = 0;
		while (looked < DARK_SKIP && packets++ < 2000 && av_read_frame(fmt, packet) >= 0) {
			if (packet->stream_index == index && avcodec_send_packet(ctx, packet) >= 0) {
				while (avcodec_receive_frame(ctx, frame) >= 0) {
					looked++;
					int light = brightness(frame);
					if (light > best_light) {
						best_light = light;
						av_frame_unref(best);
						av_frame_move_ref(best, frame);
					} else {
						av_frame_unref(frame);
					}
					if (best_light >= 28) {
						looked = DARK_SKIP;
						break;
					}
				}
			}
			av_packet_unref(packet);
		}
		if (best_light < 0) {
			goto out;
		}
	}
	// its shape as shown: wide pixels (anamorphic) made square
	double sar = best->sample_aspect_ratio.num > 0 && best->sample_aspect_ratio.den > 0
		? av_q2d(best->sample_aspect_ratio) : 1;
	double shown_w = best->width * sar, shown_h = best->height;
	double s = shown_w / max_width > shown_h / max_height ? max_width / shown_w : max_height / shown_h;
	s = s > 1 ? 1 : s;
	int w = (int)(shown_w * s + 0.5), h = (int)(shown_h * s + 0.5);
	ok = w > 0 && h > 0 && convert(best, w, h, out);
out:
	av_frame_free(&frame);
	av_frame_free(&best);
	av_packet_free(&packet);
	avcodec_free_context(&ctx);
	avformat_close_input(&fmt);
	return ok;
}
