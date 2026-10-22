#ifndef TS_DEMUX_H
#define TS_DEMUX_H

#include <stddef.h>
#include <stdint.h>

/*
 * Tiny MPEG-TS demuxer for the CLI: pulls H.264/HEVC access units and Opus /
 * AAC frames out of a transport stream so they can be handed to libppusher
 * exactly like a hardware encoder would. Not a general-purpose demuxer.
 */

#define TSD_VIDEO_NONE 0
#define TSD_VIDEO_H264 1
#define TSD_VIDEO_HEVC 2
#define TSD_AUDIO_NONE 0
#define TSD_AUDIO_OPUS 1
#define TSD_AUDIO_AAC 2

typedef struct {
	/* layer = index of the video elementary stream in PMT order (0 = first = highest quality) */
	void (*on_video)(int layer, const uint8_t *au, size_t len, int64_t pts90, int64_t dts90, void *ud);
	void (*on_audio)(const uint8_t *frame, size_t len, int64_t pts_us, void *ud);
	void *ud;
} tsd_callbacks;

typedef struct tsd tsd;

tsd *tsd_create(const tsd_callbacks *cb);
void tsd_destroy(tsd *d);
void tsd_feed(tsd *d, const uint8_t *data, size_t n);
/* deliver a PES that has no following PUSI yet (end of stream) */
void tsd_flush(tsd *d);

/* stream info, valid once tsd_ready() */
int tsd_ready(const tsd *d);
int tsd_video_codec(const tsd *d);
int tsd_video_layers(const tsd *d); /* same-codec video streams in the PMT, max 4 */
int tsd_audio_codec(const tsd *d);
int tsd_audio_channels(const tsd *d);
int tsd_audio_rate(const tsd *d);

#endif
