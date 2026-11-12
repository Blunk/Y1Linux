// SPDX-License-Identifier: GPL-2.0-only
/* Decode a file's video twice through FFmpeg, on the hardware decoder and in software, and compare
 * every picture: `vdec-ffmpeg file [frames]`. Qualification tool for the h264_y2vdec path; runs on
 * the player. Exit status 0: every picture identical. */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static enum AVPixelFormat pick_hardware(AVCodecContext *c, const enum AVPixelFormat *formats)
{
	for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++)
		if (*f == AV_PIX_FMT_DRM_PRIME) return *f;
	return formats[0];
}

/* FNV-1a over the visible picture as planar 4:2:0, whichever layout the frame has. */
static unsigned long long hash_frame(const AVFrame *f)
{
	unsigned long long h = 1469598103934665603ULL;
	int w = f->width, hgt = f->height;
#define MIX(byte) (h = (h ^ (byte)) * 1099511628211ULL)
	if (f->format == AV_PIX_FMT_DRM_PRIME) {
		int across = f->linesize[0] / 16;
		for (int y = 0; y < hgt; y++)
			for (int x = 0; x < w; x++)
				MIX(f->data[0][((y / 32 * across + x / 16) * 32 + y % 32) * 16 + x % 16]);
		for (int plane = 0; plane < 2; plane++)
			for (int y = 0; y < hgt / 2; y++)
				for (int x = 0; x < w / 2; x++)
					MIX(f->data[1][((y / 16 * across + x / 8) * 16 + y % 16) * 16 + (x % 8) * 2 + plane]);
	} else {
		for (int y = 0; y < hgt; y++)
			for (int x = 0; x < w; x++) MIX(f->data[0][y * f->linesize[0] + x]);
		for (int plane = 1; plane < 3; plane++)
			for (int y = 0; y < hgt / 2; y++)
				for (int x = 0; x < w / 2; x++) MIX(f->data[plane][y * f->linesize[plane] + x]);
	}
	return h;
}

struct run { unsigned long long *hashes; int frames, corrupt, hardware; double ms; };

static int decode(const char *file, int hardware, int limit, struct run *run)
{
	AVFormatContext *in = NULL;
	const AVCodec *codec;
	AVCodecContext *c;
	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int stream, ret, capacity = 0, draining = 0;
	double start;
	if (avformat_open_input(&in, file, NULL, NULL) || avformat_find_stream_info(in, NULL) < 0) return -1;
	stream = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if (stream < 0) return -1;
	c = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(c, in->streams[stream]->codecpar);
	if (hardware) c->get_format = pick_hardware;
	else c->thread_count = 4;
	if (avcodec_open2(c, codec, NULL) < 0) return -1;
	start = now_ms();
	while (!draining || 1) {
		ret = draining ? AVERROR_EOF : av_read_frame(in, packet);
		if (ret < 0) { draining = 1; avcodec_send_packet(c, NULL); }
		else if (packet->stream_index == stream) avcodec_send_packet(c, packet);
		if (ret >= 0) av_packet_unref(packet);
		while ((ret = avcodec_receive_frame(c, frame)) >= 0) {
			if (run->frames == capacity) {
				capacity = capacity ? capacity * 2 : 256;
				run->hashes = realloc(run->hashes, capacity * sizeof(*run->hashes));
			}
			run->hardware |= frame->format == AV_PIX_FMT_DRM_PRIME;
			run->corrupt += !!(frame->flags & AV_FRAME_FLAG_CORRUPT);
			run->ms += 0;
			run->hashes[run->frames++] = hash_frame(frame);
			av_frame_unref(frame);
			if (limit && run->frames >= limit) goto done;
		}
		if (ret == AVERROR_EOF) break;
	}
done:
	run->ms = now_ms() - start;
	printf("%s: %d pictures, %d marked corrupt, %.1f ms (%.1f per picture incl. hashing), %s %dx%d\n",
	       hardware ? (run->hardware ? "hardware" : "hardware refused, software used") : "software",
	       run->frames, run->corrupt, run->ms, run->frames ? run->ms / run->frames : 0, codec->name,
	       c->width, c->height);
	avcodec_free_context(&c);
	avformat_close_input(&in);
	av_packet_free(&packet);
	av_frame_free(&frame);
	return 0;
}

int main(int argc, char **argv)
{
	struct run hw = { 0 }, sw = { 0 };
	int limit = argc > 2 ? atoi(argv[2]) : 0, differ = 0;
	if (argc < 2) return fprintf(stderr, "usage: vdec-ffmpeg file [frames]\n"), 2;
	av_log_set_level(AV_LOG_WARNING);
	if (decode(argv[1], 1, limit, &hw) || decode(argv[1], 0, limit, &sw)) return fprintf(stderr, "cannot decode\n"), 2;
	for (int i = 0; i < hw.frames && i < sw.frames; i++)
		if (hw.hashes[i] != sw.hashes[i]) {
			if (differ++ < 10) printf("picture %d differs\n", i);
		}
	printf("%d of %d pictures differ%s\n", differ, sw.frames, hw.frames != sw.frames ? " (counts differ)" : "");
	return differ || hw.frames != sw.frames || !hw.hardware;
}
