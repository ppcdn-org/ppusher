/*
 * libppusher - SRT publisher for PPCDN (ppcenter + ppmmx), for embedded Linux boxes.
 *
 * Feed it H.264/HEVC Annex-B access units and Opus (or AAC) frames from your
 * hardware encoder; it muxes MPEG-TS, publishes over SRT, and handles the
 * ppcenter token exchange, the ppmmx degrade/loss control channel and
 * reconnects. See README.zh-CN.md and doc/design.zh-CN.md.
 *
 * Threading: ppusher_push_video/audio may be called from encoder threads at any
 * time. Callbacks run on library threads and must return quickly.
 *
 * License: MIT.
 */
#ifndef PPUSHER_H
#define PPUSHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPUSHER_VERSION "0.1.0"

/* return codes */
#define PPUSHER_OK 0
#define PPUSHER_E_ARG (-1)
#define PPUSHER_E_NOMEM (-2)
#define PPUSHER_E_NOT_CONNECTED (-3) /* frame dropped: not streaming (yet) */
#define PPUSHER_E_DROPPED (-4)       /* frame dropped: waiting for a keyframe or send queue is full */
#define PPUSHER_E_MUX (-5)           /* malformed input (e.g. not Annex-B) */

#define PPUSHER_CODEC_H264 1
#define PPUSHER_CODEC_HEVC 2

#define PPUSHER_AUDIO_NONE 0
#define PPUSHER_AUDIO_OPUS 1
#define PPUSHER_AUDIO_AAC 2

/* session states reported through on_state() */
#define PPUSHER_STATE_IDLE 0
#define PPUSHER_STATE_FETCHING 1   /* asking ppcenter for a node and fresh tokens */
#define PPUSHER_STATE_CONNECTING 2 /* SRT handshake */
#define PPUSHER_STATE_STREAMING 3
#define PPUSHER_STATE_BACKOFF 4
#define PPUSHER_STATE_FATAL 5      /* credentials rejected (HTTP 400/401/403): not retried */
#define PPUSHER_STATE_STOPPED 6

#define PPUSHER_LOG_ERROR 0
#define PPUSHER_LOG_WARN 1
#define PPUSHER_LOG_INFO 2
#define PPUSHER_LOG_DEBUG 3

typedef struct ppusher ppusher_t;

typedef struct {
	/* --- ppcenter (required unless direct_endpoint is set) --- */
	const char *ppcenter_url; /* "https://api.pp-cdn.org" */
	const char *app_id;
	const char *app_secret;
	const char *stream_name;  /* without /h264 or /hevc */
	const char *region;       /* optional requestRegion */

	/* --- media --- */
	int video_codec;          /* PPUSHER_CODEC_* */
	int audio_codec;          /* PPUSHER_AUDIO_* */
	int audio_channels;       /* Opus: 1 or 2; AAC: channel config */
	int audio_sample_rate;    /* AAC only (ADTS header), e.g. 48000 */
	int video_bitrate_kbps;   /* nominal encoder bitrate; set_bitrate() gets this * bitrate_percent/100 */

	/* --- behaviour (ppusher_config_default() sets the defaults noted here) --- */
	int codec_path;           /* 1: publish on <stream>/h264|hevc (default; pplayer needs it). 0: bare path */
	int enable_degrade;       /* 1: open the ppmmx degrade WebSocket (default) */
	int enable_sei_timestamp; /* 0: inject OBS-ABSTS-SEI-v1 (UTC ms) into every video AU for latency measurement */
	const char *passphrase;   /* SRT link encryption, 10..79 chars; NULL = off */
	int srt_latency_ms;       /* 0: libsrt default. ppmmx's TARGET_STATE.latency_ms can raise it on reconnect */
	int loss_reconnect_enable; /* 1 */
	int loss_period_sec;       /* 30 */
	int loss_window_sec;       /* 300 */
	double loss_threshold_pct; /* 1.5 */
	int backoff_min_ms;        /* 1000 */
	int backoff_max_ms;        /* 30000 */
	int queue_kb;              /* send queue size, 2048 */
	int tls_verify;            /* 1; needs ca_file or system CA paths on the box */
	const char *ca_file;       /* PEM bundle for ppcenter / wss, NULL = default paths */
	int http_timeout_ms;       /* 8000 */

	/* --- direct mode: skip ppcenter (self-hosted ppmmx, lab tests) --- */
	const char *direct_endpoint; /* "host:port" of the SRT listener */
	const char *direct_token;    /* streamid token, may be NULL */
	const char *direct_ws_url;   /* ws(s)://.../<app>/<stream>/<codec>/ws/whip, may be NULL */
	const char *direct_ws_token;
} ppusher_config_t;

typedef struct {
	/* New target encoder bitrate (kbps), from TARGET_STATE.bitrate_percent. Apply without restarting the encoder. */
	void (*set_bitrate)(int kbps, void *ud);
	/* Called after every (re)connect and while waiting for the first IDR: please emit a keyframe now. */
	void (*request_keyframe)(void *ud);
	void (*on_state)(int state, const char *info, void *ud);
	void (*on_log)(int level, const char *msg, void *ud); /* NULL: log to stderr */
	void *ud;
} ppusher_callbacks_t;

typedef struct {
	int state;
	uint32_t reconnects;       /* completed (re)connect cycles after the first */
	int bitrate_percent;       /* last TARGET_STATE */
	int degrade_connected;     /* degrade WebSocket is up */
	uint64_t msgs_sent;        /* 1316-byte SRT messages */
	uint64_t msgs_dropped;     /* send queue overflow / SRT send timeout */
	uint64_t frames_dropped;   /* video AUs dropped waiting for a keyframe or on overflow */
	double last_loss_pct;      /* last local loss period, % */
	int rtt_ms;
} ppusher_stats_t;

void ppusher_config_default(ppusher_config_t *cfg);
ppusher_t *ppusher_create(const ppusher_config_t *cfg, const ppusher_callbacks_t *cb);
/* Starts the session thread; returns immediately. */
int ppusher_start(ppusher_t *p);

/*
 * annexb: one complete access unit with start codes. B-frames are not
 * supported (pts_us must equal dts_us; pass the same value for both).
 * Timestamps are microseconds on any monotonic clock shared with audio.
 */
int ppusher_push_video(ppusher_t *p, const uint8_t *annexb, size_t len, int64_t pts_us, int64_t dts_us, int is_key);
/* One raw Opus packet, or one raw AAC access unit (no ADTS header). */
int ppusher_push_audio(ppusher_t *p, const uint8_t *frame, size_t len, int64_t pts_us);

int ppusher_get_stats(ppusher_t *p, ppusher_stats_t *out);
void ppusher_stop(ppusher_t *p);
void ppusher_destroy(ppusher_t *p);

#ifdef __cplusplus
}
#endif

#endif
