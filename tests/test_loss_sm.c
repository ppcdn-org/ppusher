#include "pp_test.h"
#include "pp_loss.h"
#include "pp_sm.h"

/* ---------------- loss window (port of publish-loss-reconnect.h) ---------------- */

static pp_loss_config cfg(int period, int window, double thr)
{
	pp_loss_config c;
	c.enable = 1;
	c.period_sec = period;
	c.window_sec = window;
	c.threshold_pct = thr;
	return c;
}

static void test_loss_defaults(void)
{
	pp_loss_config c;
	pp_loss_config_default(&c);
	CHECK_INT(c.enable, 1);
	CHECK_INT(c.period_sec, 30);
	CHECK_INT(c.window_sec, 300);
	CHECK(c.threshold_pct == 1.5);
}

static void test_loss_warmup_and_trip(void)
{
	pp_loss_monitor m;
	pp_loss_config c = cfg(1, 5, 1.5);
	int64_t t = 1000;
	int trip = 0, have = 0;
	double pct = 0;

	pp_loss_init(&m, &c);
	/* 10% unrecoverable every second. Nothing may trip before one full window of history exists. */
	trip = pp_loss_record(&m, 10, 0, 100, t, &have, &pct);
	CHECK_INT(trip, 0); /* arms the period clock */
	CHECK_INT(have, 0);
	for (t = 2000; t <= 5000; t += 1000) {
		trip = pp_loss_record(&m, 10, 0, 100, t, &have, &pct);
		CHECK_INT(trip, 0);
		CHECK_INT(have, 1);
	}
	/* window = 5 s, started at t=1000: first evaluation is allowed at t=6000 */
	trip = pp_loss_record(&m, 10, 0, 100, 6000, &have, &pct);
	CHECK_INT(trip, 1);
	CHECK(pct > 9.0 && pct < 11.0);

	/* tripping resets the window: next sample must not trip straight away */
	trip = pp_loss_record(&m, 10, 0, 100, 7000, &have, &pct);
	CHECK_INT(trip, 0);
}

static void test_loss_retrans_not_counted(void)
{
	pp_loss_monitor m;
	pp_loss_config c = cfg(1, 3, 1.5);
	int64_t t;
	int trip = 0;

	pp_loss_init(&m, &c);
	/* 10% raw loss that is entirely recovered by retransmission: unrecoverable = lost - retrans = 0 */
	for (t = 1000; t <= 20000; t += 1000)
		trip |= pp_loss_record(&m, 10, 10, 110, t, NULL, NULL);
	CHECK_INT(trip, 0);
}

static void test_loss_below_threshold(void)
{
	pp_loss_monitor m;
	pp_loss_config c = cfg(1, 3, 1.5);
	int64_t t;
	int trip = 0;

	pp_loss_init(&m, &c);
	for (t = 1000; t <= 30000; t += 1000)
		trip |= pp_loss_record(&m, 1, 0, 1000, t, NULL, NULL); /* 0.1% */
	CHECK_INT(trip, 0);
}

static void test_loss_aggregates_per_period(void)
{
	/* Retransmissions landing one sample late must not be double-penalised: feed the loss in one
	 * 1s slice and the retransmission in the next, within a single 2s period. */
	pp_loss_monitor m;
	pp_loss_config c = cfg(2, 2, 1.5);
	int64_t t;
	int trip = 0;

	pp_loss_init(&m, &c);
	for (t = 1000; t <= 40000; t += 2000) {
		trip |= pp_loss_record(&m, 20, 0, 1000, t, NULL, NULL);
		trip |= pp_loss_record(&m, 0, 20, 20, t + 1000, NULL, NULL);
	}
	CHECK_INT(trip, 0);
}

static void test_loss_window_slides(void)
{
	/* a burst older than the window no longer counts */
	pp_loss_monitor m;
	pp_loss_config c = cfg(1, 4, 1.5);
	int64_t t;
	int trip = 0;

	pp_loss_init(&m, &c);
	for (t = 1000; t <= 3000; t += 1000)
		pp_loss_record(&m, 0, 0, 1000, t, NULL, NULL);
	/* one bad second (5%) amid clean traffic stays under 1.5% averaged over the 4s window? 50/4000 = 1.25% */
	pp_loss_record(&m, 50, 0, 1000, 4000, NULL, NULL);
	for (t = 5000; t <= 30000; t += 1000)
		trip |= pp_loss_record(&m, 0, 0, 1000, t, NULL, NULL);
	CHECK_INT(trip, 0);
}

static void test_loss_disabled_and_reset(void)
{
	pp_loss_monitor m;
	pp_loss_config c = cfg(1, 2, 1.5);
	int64_t t;
	int trip = 0;

	c.enable = 0;
	pp_loss_init(&m, &c);
	for (t = 1000; t <= 20000; t += 1000)
		trip |= pp_loss_record(&m, 90, 0, 100, t, NULL, NULL);
	CHECK_INT(trip, 0);

	c.enable = 1;
	pp_loss_init(&m, &c);
	pp_loss_record(&m, 90, 0, 100, 1000, NULL, NULL);
	pp_loss_record(&m, 90, 0, 100, 2000, NULL, NULL);
	pp_loss_reset(&m); /* new connection: history from the previous one must not count */
	CHECK_INT(pp_loss_record(&m, 90, 0, 100, 3000, NULL, NULL), 0);
	CHECK_INT(m.count, 0);

	/* bogus config is repaired like ppobs: window >= period, threshold > 0 */
	c = cfg(0, 0, -1.0);
	pp_loss_init(&m, &c);
	CHECK_INT(m.period_ms, 1000);
	CHECK_INT(m.window_ms, 1000);
	CHECK(m.threshold_pct == 1.5);
}

/* ---------------- backoff ---------------- */

static void test_backoff(void)
{
	pp_backoff b;
	int d, i, prev = 0;

	pp_backoff_init(&b, 1000, 30000);
	/* no jitter when rnd lands mid-range: rnd % (2*span+1) == span */
	d = pp_backoff_next(&b, 200);
	CHECK(d >= 800 && d <= 1200);
	for (i = 0; i < 12; i++) {
		d = pp_backoff_next(&b, 12345u + (uint32_t)i);
		CHECK(d <= 30000);
		CHECK(d >= 500);
		prev = d;
	}
	CHECK(prev >= 24000); /* saturated at the cap, minus 20% jitter */

	/* grows: second delay ~2s, third ~4s */
	pp_backoff_init(&b, 1000, 30000);
	d = pp_backoff_next(&b, 200); /* jitter-free */
	CHECK(d >= 800 && d <= 1200);
	d = pp_backoff_next(&b, 400);
	CHECK(d >= 1600 && d <= 2400);
	d = pp_backoff_next(&b, 800);
	CHECK(d >= 3200 && d <= 4800);
	pp_backoff_reset(&b);
	d = pp_backoff_next(&b, 200);
	CHECK(d >= 800 && d <= 1200);

	/* jitter bounds over many draws */
	pp_backoff_init(&b, 1000, 1000);
	for (i = 0; i < 1000; i++) {
		d = pp_backoff_next(&b, (uint32_t)i * 2654435761u);
		CHECK(d >= 800 && d <= 1000);
	}

	/* nonsense config is repaired */
	pp_backoff_init(&b, 0, 0);
	CHECK(b.min_ms >= 100);
	CHECK(b.max_ms >= b.min_ms);
}

/* ---------------- session state machine ---------------- */

static pp_sm mk(void)
{
	pp_sm sm;
	pp_sm_init(&sm, 1000, 30000);
	return sm;
}

static void to_streaming(pp_sm *sm, int64_t t)
{
	CHECK_INT(pp_sm_handle(sm, PP_EV_START, t, 0), PP_ACT_FETCH);
	CHECK_INT(pp_sm_handle(sm, PP_EV_FETCH_OK, t, 0), PP_ACT_CONNECT);
	CHECK_INT(pp_sm_handle(sm, PP_EV_CONNECT_OK, t, 0), PP_ACT_NONE);
	CHECK_INT(sm->state, PP_ST_STREAMING);
}

static void test_sm_happy_path(void)
{
	pp_sm sm = mk();
	CHECK_INT(sm.state, PP_ST_IDLE);
	to_streaming(&sm, 1000);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_STOP, 2000, 0), PP_ACT_STOP);
	CHECK_INT(sm.state, PP_ST_STOPPED);
	/* nothing leaves STOPPED */
	CHECK_INT(pp_sm_handle(&sm, PP_EV_START, 3000, 0), PP_ACT_NONE);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_RECONNECT_REQ, 3000, 0), PP_ACT_NONE);
}

static void test_sm_fetch_failures(void)
{
	pp_sm sm = mk();

	CHECK_INT(pp_sm_handle(&sm, PP_EV_START, 1000, 0), PP_ACT_FETCH);
	/* 503 / network: back off, then fetch again (a fresh fetch = a fresh token) */
	CHECK_INT(pp_sm_handle(&sm, PP_EV_FETCH_RETRY, 1000, 200), PP_ACT_WAIT);
	CHECK_INT(sm.state, PP_ST_BACKOFF);
	CHECK(sm.wake_ms >= 1800 && sm.wake_ms <= 2200);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_TICK, 1500, 0), PP_ACT_WAIT);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_TICK, 2300, 0), PP_ACT_FETCH);
	CHECK_INT(sm.state, PP_ST_FETCH);

	/* second failure waits longer */
	CHECK_INT(pp_sm_handle(&sm, PP_EV_FETCH_RETRY, 2300, 400), PP_ACT_WAIT);
	CHECK(sm.wake_ms - 2300 >= 1600);

	/* 402 arrears: fixed slow retry */
	CHECK_INT(pp_sm_handle(&sm, PP_EV_TICK, 100000, 0), PP_ACT_FETCH);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_FETCH_SLOW, 100000, 0), PP_ACT_WAIT);
	CHECK_INT(sm.wake_ms, 160000);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_TICK, 159999, 0), PP_ACT_WAIT);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_TICK, 160000, 0), PP_ACT_FETCH);

	/* 401: fatal, never retried */
	CHECK_INT(pp_sm_handle(&sm, PP_EV_FETCH_FATAL, 160000, 0), PP_ACT_NONE);
	CHECK_INT(sm.state, PP_ST_FATAL);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_TICK, 999999, 0), PP_ACT_NONE);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_START, 999999, 0), PP_ACT_NONE);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_STOP, 999999, 0), PP_ACT_STOP);
}

static void test_sm_connect_fail(void)
{
	pp_sm sm = mk();
	pp_sm_handle(&sm, PP_EV_START, 1000, 0);
	pp_sm_handle(&sm, PP_EV_FETCH_OK, 1000, 0);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_CONNECT_FAIL, 1000, 200), PP_ACT_WAIT);
	CHECK_INT(sm.state, PP_ST_BACKOFF);
	/* after the wait: back to FETCH, not CONNECT - the old token must not be reused */
	CHECK_INT(pp_sm_handle(&sm, PP_EV_TICK, 5000, 0), PP_ACT_FETCH);
}

static void test_sm_link_broken_backoff_and_stability(void)
{
	pp_sm sm = mk();
	int i;

	/* flapping link: connect succeeds, drops after 2 s, repeatedly -> delays keep growing */
	int64_t t = 1000, last_delay = 0;
	to_streaming(&sm, t);
	for (i = 0; i < 4; i++) {
		t += 2000;
		CHECK_INT(pp_sm_handle(&sm, PP_EV_LINK_BROKEN, t, 200), PP_ACT_WAIT);
		CHECK(sm.wake_ms - t > last_delay || i == 0);
		last_delay = sm.wake_ms - t;
		t = sm.wake_ms;
		CHECK_INT(pp_sm_handle(&sm, PP_EV_TICK, t, 0), PP_ACT_FETCH);
		CHECK_INT(pp_sm_handle(&sm, PP_EV_FETCH_OK, t, 0), PP_ACT_CONNECT);
		CHECK_INT(pp_sm_handle(&sm, PP_EV_CONNECT_OK, t, 0), PP_ACT_NONE);
		CHECK_INT(sm.state, PP_ST_STREAMING);
	}
	CHECK(last_delay >= 6400);

	/* a connection that survived >= 30 s clears the backoff: next drop retries in ~1 s */
	sm.connected_ms = t;
	CHECK_INT(pp_sm_handle(&sm, PP_EV_TICK, t + 31000, 0), PP_ACT_NONE);
	CHECK_INT(sm.backoff.attempt, 0);
	t += 40000;
	CHECK_INT(pp_sm_handle(&sm, PP_EV_LINK_BROKEN, t, 200), PP_ACT_WAIT);
	CHECK(sm.wake_ms - t >= 800 && sm.wake_ms - t <= 1200);

	/* link breaking while still connecting also backs off */
	{
		pp_sm s2 = mk();
		pp_sm_handle(&s2, PP_EV_START, 1000, 0);
		pp_sm_handle(&s2, PP_EV_FETCH_OK, 1000, 0);
		CHECK_INT(pp_sm_handle(&s2, PP_EV_LINK_BROKEN, 1100, 200), PP_ACT_WAIT);
	}
}

static void test_sm_forced_reconnect(void)
{
	pp_sm sm = mk();

	to_streaming(&sm, 10000);
	/* TARGET_STATE.reconnect: close and fetch immediately, no backoff, backoff counter untouched */
	CHECK_INT(pp_sm_handle(&sm, PP_EV_RECONNECT_REQ, 20000, 0), PP_ACT_CLOSE_AND_FETCH);
	CHECK_INT(sm.state, PP_ST_FETCH);
	CHECK_INT(sm.backoff.attempt, 0);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_FETCH_OK, 20100, 0), PP_ACT_CONNECT);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_CONNECT_OK, 20200, 0), PP_ACT_NONE);

	/* a second trigger inside the 5 s guard (mmx and the local loss window racing) is swallowed */
	CHECK_INT(pp_sm_handle(&sm, PP_EV_LOSS_TRIP, 22000, 0), PP_ACT_NONE);
	CHECK_INT(sm.state, PP_ST_STREAMING);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_LOSS_TRIP, 25001, 0), PP_ACT_CLOSE_AND_FETCH);

	/* triggers outside STREAMING are ignored */
	CHECK_INT(pp_sm_handle(&sm, PP_EV_RECONNECT_REQ, 99999, 0), PP_ACT_NONE);
	CHECK_INT(sm.state, PP_ST_FETCH);
	{
		pp_sm s2 = mk();
		pp_sm_handle(&s2, PP_EV_START, 1000, 0);
		pp_sm_handle(&s2, PP_EV_FETCH_RETRY, 1000, 0);
		CHECK_INT(pp_sm_handle(&s2, PP_EV_LOSS_TRIP, 1500, 0), PP_ACT_NONE);
		CHECK_INT(s2.state, PP_ST_BACKOFF);
	}
}

static void test_sm_out_of_order_events_ignored(void)
{
	pp_sm sm = mk();
	CHECK_INT(pp_sm_handle(&sm, PP_EV_CONNECT_OK, 1000, 0), PP_ACT_NONE);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_FETCH_OK, 1000, 0), PP_ACT_NONE);
	CHECK_INT(pp_sm_handle(&sm, PP_EV_LINK_BROKEN, 1000, 0), PP_ACT_NONE);
	CHECK_INT(sm.state, PP_ST_IDLE);
}

int main(void)
{
	RUN(test_loss_defaults);
	RUN(test_loss_warmup_and_trip);
	RUN(test_loss_retrans_not_counted);
	RUN(test_loss_below_threshold);
	RUN(test_loss_aggregates_per_period);
	RUN(test_loss_window_slides);
	RUN(test_loss_disabled_and_reset);
	RUN(test_backoff);
	RUN(test_sm_happy_path);
	RUN(test_sm_fetch_failures);
	RUN(test_sm_connect_fail);
	RUN(test_sm_link_broken_backoff_and_stability);
	RUN(test_sm_forced_reconnect);
	RUN(test_sm_out_of_order_events_ignored);
	TEST_DONE();
}
