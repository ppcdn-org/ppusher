#ifndef PP_LOSS_H
#define PP_LOSS_H

#include <stdint.h>

/*
 * Local fast-reconnect loss window. Direct port of ppobs
 * plugins/common/publish-loss-reconnect.h (same aggregation, same warm-up).
 *
 *  - unrecoverable loss is computed once per *period* from the period's
 *    aggregates, not per raw sample (retransmissions lag losses by ~1 RTT);
 *  - nothing is evaluated until a full window of history has accumulated.
 */

#define PP_LOSS_MAX_SAMPLES 1024

typedef struct {
	int enable;
	int period_sec;
	int window_sec;
	double threshold_pct;
} pp_loss_config;

typedef struct {
	int enable;
	int64_t period_ms;
	int64_t window_ms;
	double threshold_pct;
	int started;
	int64_t start_ms;
	int armed;
	int64_t next_sample_ms;
	uint64_t acc_lost, acc_retrans, acc_sent;
	int head, count;
	int64_t sample_ms[PP_LOSS_MAX_SAMPLES];
	uint64_t sample_lost[PP_LOSS_MAX_SAMPLES];
	uint64_t sample_expected[PP_LOSS_MAX_SAMPLES];
} pp_loss_monitor;

void pp_loss_config_default(pp_loss_config *c); /* enabled, 30s, 300s, 1.5% */
void pp_loss_init(pp_loss_monitor *m, const pp_loss_config *c);
void pp_loss_reset(pp_loss_monitor *m);

/*
 * Feed per-interval deltas (not cumulative totals). Returns 1 when the
 * window's aggregate unrecoverable-loss rate exceeds the threshold and the
 * caller should reconnect. When a full period elapsed *have_sample is set and
 * *sample_pct carries that period's own loss rate.
 *
 * mmx LOSS_SAMPLE: record(unrecoverable_delta, 0, expected_delta)
 * local SRT stats: record(lost, retrans, sent)
 */
int pp_loss_record(pp_loss_monitor *m, uint64_t lost, uint64_t retrans, uint64_t sent, int64_t now_ms,
		   int *have_sample, double *sample_pct);

#endif
