#include "pp_loss.h"

#include <string.h>

void pp_loss_config_default(pp_loss_config *c)
{
	c->enable = 1;
	c->period_sec = 30;
	c->window_sec = 300;
	c->threshold_pct = 1.5;
}

void pp_loss_init(pp_loss_monitor *m, const pp_loss_config *c)
{
	int period = c->period_sec < 1 ? 1 : c->period_sec;
	int window = c->window_sec < period ? period : c->window_sec;
	memset(m, 0, sizeof *m);
	m->enable = c->enable;
	m->period_ms = (int64_t)period * 1000;
	m->window_ms = (int64_t)window * 1000;
	m->threshold_pct = c->threshold_pct > 0.0 ? c->threshold_pct : 1.5;
}

void pp_loss_reset(pp_loss_monitor *m)
{
	m->started = 0;
	m->start_ms = 0;
	m->armed = 0;
	m->next_sample_ms = 0;
	m->acc_lost = m->acc_retrans = m->acc_sent = 0;
	m->head = 0;
	m->count = 0;
}

int pp_loss_record(pp_loss_monitor *m, uint64_t lost, uint64_t retrans, uint64_t sent, int64_t now_ms,
		   int *have_sample, double *sample_pct)
{
	uint64_t p_lost, p_retrans, p_sent, p_unrec, p_expected, win_lost = 0, win_expected = 0;
	int64_t cutoff;
	double win_pct;
	int i;

	if (have_sample)
		*have_sample = 0;
	if (sample_pct)
		*sample_pct = 0.0;
	if (!m->enable)
		return 0;

	if (!m->started) {
		m->started = 1;
		m->start_ms = now_ms;
	}

	m->acc_lost += lost;
	m->acc_retrans += retrans;
	m->acc_sent += sent;

	if (!m->armed) {
		m->armed = 1;
		m->next_sample_ms = now_ms + m->period_ms;
		return 0;
	}
	if (now_ms < m->next_sample_ms)
		return 0;
	m->next_sample_ms = now_ms + m->period_ms;

	p_lost = m->acc_lost;
	p_retrans = m->acc_retrans;
	p_sent = m->acc_sent;
	m->acc_lost = m->acc_retrans = m->acc_sent = 0;

	p_unrec = p_lost > p_retrans ? p_lost - p_retrans : 0;
	p_expected = p_sent > p_retrans ? p_sent - p_retrans : 0;

	m->sample_ms[m->head] = now_ms;
	m->sample_lost[m->head] = p_unrec;
	m->sample_expected[m->head] = p_expected;
	m->head = (m->head + 1) % PP_LOSS_MAX_SAMPLES;
	if (m->count < PP_LOSS_MAX_SAMPLES)
		m->count++;

	if (have_sample)
		*have_sample = 1;
	if (sample_pct && p_expected)
		*sample_pct = (double)p_unrec * 100.0 / (double)p_expected;

	if (now_ms - m->start_ms < m->window_ms)
		return 0;

	cutoff = now_ms - m->window_ms;
	while (m->count > 0) {
		int oldest = (m->head - m->count + PP_LOSS_MAX_SAMPLES) % PP_LOSS_MAX_SAMPLES;
		if (m->sample_ms[oldest] >= cutoff)
			break;
		m->count--;
	}
	for (i = 0; i < m->count; i++) {
		int idx = (m->head - 1 - i + PP_LOSS_MAX_SAMPLES * 2) % PP_LOSS_MAX_SAMPLES;
		win_lost += m->sample_lost[idx];
		win_expected += m->sample_expected[idx];
	}
	if (win_expected == 0)
		return 0;
	win_pct = (double)win_lost * 100.0 / (double)win_expected;
	if (win_pct > m->threshold_pct) {
		pp_loss_reset(m);
		return 1;
	}
	return 0;
}
