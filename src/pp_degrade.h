#ifndef PP_DEGRADE_H
#define PP_DEGRADE_H

#include <stddef.h>
#include <stdint.h>

/* Messages pushed by ppmmx on the degrade WebSocket (ppmmx internal/degrade/degrade.go). */
#define PP_MSG_UNKNOWN 0
#define PP_MSG_TARGET_STATE 1
#define PP_MSG_LOSS_SAMPLE 2
#define PP_MSG_ALERT 3

typedef struct {
	int type;
	char path[192];
	char codec[8];
	/* TARGET_STATE */
	int layers;
	int bitrate_percent; /* clamped to 1..100, 100 when absent */
	int latency_ms;
	int reconnect;
	char reason[160]; /* TARGET_STATE.reason or ALERT.reason */
	/* LOSS_SAMPLE */
	uint64_t unrecoverable_delta;
	uint64_t expected_delta;
} pp_degrade_msg;

/* Returns 0 on success (msg->type may still be PP_MSG_UNKNOWN for an unrelated type), -1 on malformed JSON. */
int pp_degrade_parse(const char *json, size_t n, pp_degrade_msg *msg);

/*
 * whipTracks[codec].url -> degrade control URL:
 *   https://h/app/stream/codec/whip -> wss://h/app/stream/codec/ws/whip
 *   http://...                      -> ws://...
 * Anything else (other scheme, no trailing /whip, query/fragment) is rejected rather than guessed.
 */
int pp_degrade_ws_url(const char *whip_url, char *out, size_t outsz);

/* Idempotent TARGET_STATE application: remembers the last applied state. */
#define PP_TGT_BITRATE_CHANGED 1
#define PP_TGT_LAYERS_CHANGED 2
#define PP_TGT_RECONNECT 4

typedef struct {
	int have;
	int layers;
	int bitrate_percent;
} pp_target_tracker;

/* Returns a PP_TGT_* bit set; 0 means "nothing to do" (identical state, no reconnect). */
int pp_target_apply(pp_target_tracker *t, const pp_degrade_msg *m);

#endif
