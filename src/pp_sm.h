#ifndef PP_SM_H
#define PP_SM_H

#include <stdint.h>

/* ---- exponential backoff with jitter ---- */

typedef struct {
	int min_ms, max_ms;
	int attempt;
} pp_backoff;

void pp_backoff_init(pp_backoff *b, int min_ms, int max_ms);
void pp_backoff_reset(pp_backoff *b);
/* delay = min(max, min * 2^attempt), +-20% jitter derived from `rnd`; advances attempt. */
int pp_backoff_next(pp_backoff *b, uint32_t rnd);

/* ---- session reconnect state machine (pure: events in, actions out) ---- */

typedef enum {
	PP_ST_IDLE = 0,
	PP_ST_FETCH,     /* asking ppcenter for a node + fresh tokens */
	PP_ST_CONNECT,   /* SRT handshake */
	PP_ST_STREAMING,
	PP_ST_BACKOFF,
	PP_ST_FATAL,     /* credentials rejected: do not retry */
	PP_ST_STOPPED
} pp_state;

typedef enum {
	PP_EV_START,
	PP_EV_FETCH_OK,
	PP_EV_FETCH_RETRY, /* network error, 5xx, 503 no_healthy_origin ... */
	PP_EV_FETCH_SLOW,  /* 402 account in arrears: retry slowly */
	PP_EV_FETCH_FATAL, /* 400/401/403 */
	PP_EV_CONNECT_OK,
	PP_EV_CONNECT_FAIL,
	PP_EV_LINK_BROKEN,
	PP_EV_RECONNECT_REQ, /* TARGET_STATE.reconnect from ppmmx */
	PP_EV_LOSS_TRIP,     /* loss window over threshold */
	PP_EV_TICK,
	PP_EV_STOP
} pp_event;

typedef enum {
	PP_ACT_NONE = 0,
	PP_ACT_FETCH,           /* run a ppcenter fetch now */
	PP_ACT_CONNECT,         /* open the SRT connection now */
	PP_ACT_CLOSE_AND_FETCH, /* close the live SRT link, then fetch (fresh token) */
	PP_ACT_WAIT,            /* nothing until sm->wake_ms */
	PP_ACT_STOP
} pp_action;

typedef struct {
	pp_state state;
	pp_backoff backoff;
	int64_t wake_ms;          /* BACKOFF expiry */
	int64_t connected_ms;     /* when STREAMING began */
	int64_t last_forced_ms;   /* last mmx/loss triggered reconnect */
	int stable_cleared;       /* backoff already reset for this connection */
	int stable_ms;            /* connection age that clears backoff (30s) */
	int min_forced_gap_ms;    /* min spacing of forced reconnects (5s) */
	int slow_retry_ms;        /* 402 retry delay (60s) */
} pp_sm;

void pp_sm_init(pp_sm *sm, int backoff_min_ms, int backoff_max_ms);
pp_action pp_sm_handle(pp_sm *sm, pp_event ev, int64_t now_ms, uint32_t rnd);

#endif
