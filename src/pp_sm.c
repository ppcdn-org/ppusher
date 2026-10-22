#include "pp_sm.h"

#include <string.h>

void pp_backoff_init(pp_backoff *b, int min_ms, int max_ms)
{
	if (min_ms < 100)
		min_ms = 100;
	if (max_ms < min_ms)
		max_ms = min_ms;
	b->min_ms = min_ms;
	b->max_ms = max_ms;
	b->attempt = 0;
}

void pp_backoff_reset(pp_backoff *b)
{
	b->attempt = 0;
}

int pp_backoff_next(pp_backoff *b, uint32_t rnd)
{
	int64_t d = b->min_ms;
	int i, jitter, span;

	for (i = 0; i < b->attempt && d < b->max_ms; i++)
		d *= 2;
	if (d > b->max_ms)
		d = b->max_ms;
	if (b->attempt < 30)
		b->attempt++;

	span = (int)(d / 5); /* +-20% */
	if (span > 0) {
		jitter = (int)(rnd % (uint32_t)(2 * span + 1)) - span;
		d += jitter;
	}
	if (d > b->max_ms)
		d = b->max_ms;
	if (d < b->min_ms / 2)
		d = b->min_ms / 2;
	return (int)d;
}

void pp_sm_init(pp_sm *sm, int backoff_min_ms, int backoff_max_ms)
{
	memset(sm, 0, sizeof *sm);
	sm->state = PP_ST_IDLE;
	pp_backoff_init(&sm->backoff, backoff_min_ms, backoff_max_ms);
	sm->stable_ms = 30000;
	sm->min_forced_gap_ms = 5000;
	sm->slow_retry_ms = 60000;
}

static pp_action enter_backoff(pp_sm *sm, int64_t now_ms, uint32_t rnd, int fixed_ms)
{
	int delay = fixed_ms > 0 ? fixed_ms : pp_backoff_next(&sm->backoff, rnd);
	if (fixed_ms > 0)
		pp_backoff_next(&sm->backoff, rnd); /* still counts as an attempt */
	sm->state = PP_ST_BACKOFF;
	sm->wake_ms = now_ms + delay;
	return PP_ACT_WAIT;
}

pp_action pp_sm_handle(pp_sm *sm, pp_event ev, int64_t now_ms, uint32_t rnd)
{
	if (sm->state == PP_ST_STOPPED)
		return PP_ACT_NONE;
	if (ev == PP_EV_STOP) {
		sm->state = PP_ST_STOPPED;
		return PP_ACT_STOP;
	}
	if (sm->state == PP_ST_FATAL)
		return PP_ACT_NONE;

	switch (ev) {
	case PP_EV_START:
		if (sm->state != PP_ST_IDLE)
			return PP_ACT_NONE;
		sm->state = PP_ST_FETCH;
		return PP_ACT_FETCH;

	case PP_EV_FETCH_OK:
		if (sm->state != PP_ST_FETCH)
			return PP_ACT_NONE;
		sm->state = PP_ST_CONNECT;
		return PP_ACT_CONNECT;

	case PP_EV_FETCH_RETRY:
		if (sm->state != PP_ST_FETCH)
			return PP_ACT_NONE;
		return enter_backoff(sm, now_ms, rnd, 0);

	case PP_EV_FETCH_SLOW:
		if (sm->state != PP_ST_FETCH)
			return PP_ACT_NONE;
		return enter_backoff(sm, now_ms, rnd, sm->slow_retry_ms);

	case PP_EV_FETCH_FATAL:
		if (sm->state != PP_ST_FETCH)
			return PP_ACT_NONE;
		sm->state = PP_ST_FATAL;
		return PP_ACT_NONE;

	case PP_EV_CONNECT_OK:
		if (sm->state != PP_ST_CONNECT)
			return PP_ACT_NONE;
		sm->state = PP_ST_STREAMING;
		sm->connected_ms = now_ms;
		sm->stable_cleared = 0;
		return PP_ACT_NONE;

	case PP_EV_CONNECT_FAIL:
		if (sm->state != PP_ST_CONNECT)
			return PP_ACT_NONE;
		return enter_backoff(sm, now_ms, rnd, 0);

	case PP_EV_LINK_BROKEN:
		if (sm->state != PP_ST_STREAMING && sm->state != PP_ST_CONNECT)
			return PP_ACT_NONE;
		if (sm->state == PP_ST_STREAMING && !sm->stable_cleared && now_ms - sm->connected_ms >= sm->stable_ms)
			pp_backoff_reset(&sm->backoff);
		return enter_backoff(sm, now_ms, rnd, 0);

	case PP_EV_RECONNECT_REQ:
	case PP_EV_LOSS_TRIP:
		if (sm->state != PP_ST_STREAMING)
			return PP_ACT_NONE;
		if (sm->last_forced_ms != 0 && now_ms - sm->last_forced_ms < sm->min_forced_gap_ms)
			return PP_ACT_NONE;
		sm->last_forced_ms = now_ms;
		sm->state = PP_ST_FETCH;
		return PP_ACT_CLOSE_AND_FETCH;

	case PP_EV_TICK:
		if (sm->state == PP_ST_STREAMING && !sm->stable_cleared && now_ms - sm->connected_ms >= sm->stable_ms) {
			sm->stable_cleared = 1;
			pp_backoff_reset(&sm->backoff);
		}
		if (sm->state == PP_ST_BACKOFF) {
			if (now_ms >= sm->wake_ms) {
				sm->state = PP_ST_FETCH;
				return PP_ACT_FETCH;
			}
			return PP_ACT_WAIT;
		}
		return PP_ACT_NONE;

	default:
		return PP_ACT_NONE;
	}
}
