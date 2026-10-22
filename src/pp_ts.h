#ifndef PP_TS_H
#define PP_TS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal MPEG-TS muxer producing exactly what ppmmx's SRT ingest accepts
 * (see doc/design.zh-CN.md section 2.6): 1316-byte (7 x 188) SRT messages,
 * PAT+PMT first and periodically, H.264/HEVC Annex-B video with AUD, Opus
 * (registration + extension descriptors, control header) or AAC/ADTS audio.
 *
 * Not thread-safe: callers serialise access.
 */

#define PP_TS_PACKET 188
#define PP_TS_MSG_SIZE 1316

#define PP_PID_PMT 0x1000
#define PP_PID_VIDEO 0x100
#define PP_PID_AUDIO 0x101

#define PP_AUDIO_NONE 0
#define PP_AUDIO_OPUS 1
#define PP_AUDIO_AAC 2

typedef void (*pp_ts_sink_fn)(const uint8_t *msg, size_t len, void *ud);

typedef struct {
	int video_codec; /* PP_CODEC_H264 / PP_CODEC_HEVC */
	int audio_codec; /* PP_AUDIO_* */
	int audio_channels;
	int audio_sample_rate; /* AAC only */
	int sei_timestamp;     /* inject OBS-ABSTS-SEI-v1 into every video AU */
	pp_ts_sink_fn sink;
	void *ud;
} pp_ts_config;

typedef struct pp_ts pp_ts;

pp_ts *pp_ts_create(const pp_ts_config *cfg);
void pp_ts_destroy(pp_ts *ts);

/* Emit PAT+PMT now. */
int pp_ts_write_tables(pp_ts *ts);

/*
 * One access unit of Annex-B video. pts/dts are 90 kHz ticks (B-frames are
 * not supported: pass dts == pts). wall_ms is the UTC ms put in the SEI
 * (ignored unless sei_timestamp). Returns 0, -1 (bad input), -2 (no memory).
 */
int pp_ts_write_video(pp_ts *ts, const uint8_t *annexb, size_t len, int64_t pts90, int64_t dts90, int is_key,
		      int64_t wall_ms);

/* One raw Opus packet, or one raw AAC access unit (ADTS header added here). */
int pp_ts_write_audio(pp_ts *ts, const uint8_t *frame, size_t len, int64_t pts90);

/* Bytes of the SRT message currently being filled (0 = nothing pending). */
size_t pp_ts_pending(const pp_ts *ts);

/* Pad the partially filled SRT message with null packets and hand it to the sink. */
void pp_ts_flush(pp_ts *ts);

/* exposed for tests */
uint32_t pp_crc32_mpeg(const uint8_t *p, size_t n);

#endif
