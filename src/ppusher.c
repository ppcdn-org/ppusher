/*
 * Session layer: ppcenter fetch -> SRT connect -> stream, with the degrade
 * WebSocket on the side and the reconnect state machine (pp_sm) deciding what
 * happens next. See doc/design.zh-CN.md section 4.
 */
#include "ppusher/ppusher.h"

#include "pp_degrade.h"
#include "pp_loss.h"
#include "pp_platform.h"
#include "pp_ppcenter.h"
#include "pp_sm.h"
#include "pp_streamid.h"
#include "pp_ts.h"
#include "pp_ws.h"

#include <srt/srt.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

#define SRT_DEFAULT_PORT 7890
#define KEYFRAME_RETRY_MS 1000
#define SRT_STATS_MS 1000

typedef struct {
	char url[PP_WSURL_MAX];
	char token[PP_TOKEN_MAX];
	unsigned gen;
} ws_target;

struct ppusher {
	ppusher_config_t cfg;
	ppusher_callbacks_t cb;
	/* owned copies of the config strings */
	char s_url[256], s_app[128], s_secret[256], s_stream[128], s_region[64], s_pass[96], s_ca[256];
	char s_dep[256], s_dtok[PP_TOKEN_MAX], s_dws[PP_WSURL_MAX], s_dwst[PP_TOKEN_MAX];

	pthread_t th_session, th_ws;
	int started, ws_started;
	volatile int stop;

	/* everything below `mu` is shared between the session thread, the WS thread and API callers */
	pthread_mutex_t mu;
	int state_pub;
	ws_target wt;
	int ws_connected;
	pp_target_tracker tgt;
	int bitrate_pct;
	int bitrate_dirty;
	int ev_reconnect, ev_loss;
	int latency_hint_ms;
	pp_loss_monitor loss;
	pp_loss_config loss_cfg;
	double last_loss_pct;
	int rtt_ms;
	uint32_t reconnects;
	int have_streamed;

	/* media path */
	pthread_mutex_t mux_mu;
	pp_ts *ts;
	volatile int accept_media;
	volatile int need_key;
	int64_t last_audio_ms;

	/* send queue (ring of 1316-byte messages) */
	pthread_mutex_t q_mu;
	pthread_cond_t q_cv;
	uint8_t *q;
	size_t q_cap, q_head, q_count;
	uint64_t msgs_sent, msgs_dropped, frames_dropped;

	/* session-thread-only */
	pp_sm sm;
	SRTSOCKET sock;
	pp_publish_decision *dec;
	char streamid[PP_STREAMID_MAX + 8];
	char endpoint[256];
	int64_t last_kf_req_ms, last_stats_ms;
	int64_t prev_sent, prev_loss, prev_retrans;
	int stats_primed;
	int tail_timeouts;
};

/* ------------------------------------------------------------------ logging */

static void plog(ppusher_t *p, int lvl, const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (p->cb.on_log)
		p->cb.on_log(lvl, msg, p->cb.ud);
	else if (lvl <= PPUSHER_LOG_INFO)
		fprintf(stderr, "[ppusher] %s\n", msg);
}

static void set_state(ppusher_t *p, int st, const char *info)
{
	pthread_mutex_lock(&p->mu);
	p->state_pub = st;
	pthread_mutex_unlock(&p->mu);
	if (p->cb.on_state)
		p->cb.on_state(st, info ? info : "", p->cb.ud);
}

/* ------------------------------------------------------------------ config */

void ppusher_config_default(ppusher_config_t *c)
{
	memset(c, 0, sizeof *c);
	c->video_codec = PPUSHER_CODEC_H264;
	c->audio_codec = PPUSHER_AUDIO_OPUS;
	c->audio_channels = 2;
	c->audio_sample_rate = 48000;
	c->codec_path = 1;
	c->enable_degrade = 1;
	c->loss_reconnect_enable = 1;
	c->loss_period_sec = 30;
	c->loss_window_sec = 300;
	c->loss_threshold_pct = 1.5;
	c->backoff_min_ms = 1000;
	c->backoff_max_ms = 30000;
	c->queue_kb = 2048;
	c->tls_verify = 1;
	c->http_timeout_ms = 8000;
}

static int copy_str(char *dst, size_t cap, const char *src)
{
	size_t n;
	if (!src) {
		dst[0] = 0;
		return 0;
	}
	n = strlen(src);
	if (n >= cap)
		return -1;
	memcpy(dst, src, n + 1);
	return 0;
}

ppusher_t *ppusher_create(const ppusher_config_t *cfg, const ppusher_callbacks_t *cb)
{
	ppusher_t *p;
	int direct;

	if (!cfg)
		return NULL;
	direct = cfg->direct_endpoint && *cfg->direct_endpoint;
	if (!cfg->app_id || !cfg->stream_name || (!direct && (!cfg->ppcenter_url || !cfg->app_secret)))
		return NULL;
	if (cfg->video_codec != PPUSHER_CODEC_H264 && cfg->video_codec != PPUSHER_CODEC_HEVC)
		return NULL;
	if (cfg->audio_codec == PPUSHER_AUDIO_OPUS && cfg->audio_channels != 1 && cfg->audio_channels != 2)
		return NULL;

	p = (ppusher_t *)calloc(1, sizeof *p);
	if (!p)
		return NULL;
	p->cfg = *cfg;
	if (cb)
		p->cb = *cb;
	if (copy_str(p->s_url, sizeof p->s_url, cfg->ppcenter_url) || copy_str(p->s_app, sizeof p->s_app, cfg->app_id) ||
	    copy_str(p->s_secret, sizeof p->s_secret, cfg->app_secret) ||
	    copy_str(p->s_stream, sizeof p->s_stream, cfg->stream_name) ||
	    copy_str(p->s_region, sizeof p->s_region, cfg->region) || copy_str(p->s_pass, sizeof p->s_pass, cfg->passphrase) ||
	    copy_str(p->s_ca, sizeof p->s_ca, cfg->ca_file) || copy_str(p->s_dep, sizeof p->s_dep, cfg->direct_endpoint) ||
	    copy_str(p->s_dtok, sizeof p->s_dtok, cfg->direct_token) || copy_str(p->s_dws, sizeof p->s_dws, cfg->direct_ws_url) ||
	    copy_str(p->s_dwst, sizeof p->s_dwst, cfg->direct_ws_token)) {
		free(p);
		return NULL;
	}
	/* a user may paste "<stream>/hevc"; the path suffix is ours to add */
	pp_strip_codec_suffix(p->s_stream);
	if (!pp_stream_name_valid(p->s_app) || !pp_stream_name_valid(p->s_stream)) {
		free(p);
		return NULL;
	}
	p->cfg.ppcenter_url = p->s_url;
	p->cfg.app_id = p->s_app;
	p->cfg.app_secret = p->s_secret;
	p->cfg.stream_name = p->s_stream;
	p->cfg.region = p->s_region;
	p->cfg.passphrase = p->s_pass[0] ? p->s_pass : NULL;
	p->cfg.ca_file = p->s_ca[0] ? p->s_ca : NULL;

	if (p->cfg.queue_kb < 64)
		p->cfg.queue_kb = 64;
	p->q_cap = (size_t)p->cfg.queue_kb * 1024 / PP_TS_MSG_SIZE;
	p->q = (uint8_t *)malloc(p->q_cap * PP_TS_MSG_SIZE);
	p->dec = (pp_publish_decision *)calloc(1, sizeof *p->dec);
	if (!p->q || !p->dec) {
		free(p->q);
		free(p->dec);
		free(p);
		return NULL;
	}
	pthread_mutex_init(&p->mu, NULL);
	pthread_mutex_init(&p->mux_mu, NULL);
	pthread_mutex_init(&p->q_mu, NULL);
	pthread_cond_init(&p->q_cv, NULL);

	p->loss_cfg.enable = p->cfg.loss_reconnect_enable;
	p->loss_cfg.period_sec = p->cfg.loss_period_sec;
	p->loss_cfg.window_sec = p->cfg.loss_window_sec;
	p->loss_cfg.threshold_pct = p->cfg.loss_threshold_pct;
	pp_loss_init(&p->loss, &p->loss_cfg);
	pp_sm_init(&p->sm, p->cfg.backoff_min_ms, p->cfg.backoff_max_ms);
	p->bitrate_pct = 100;
	p->sock = SRT_INVALID_SOCK;
	srt_startup();
	return p;
}

/* ------------------------------------------------------------------ send queue / mux sink */

static void q_push(ppusher_t *p, const uint8_t *msg)
{
	pthread_mutex_lock(&p->q_mu);
	if (p->q_count == p->q_cap) {
		p->msgs_dropped++; /* overflow: the newest message is dropped */
		pthread_mutex_unlock(&p->q_mu);
		return;
	}
	memcpy(p->q + ((p->q_head + p->q_count) % p->q_cap) * PP_TS_MSG_SIZE, msg, PP_TS_MSG_SIZE);
	p->q_count++;
	pthread_cond_signal(&p->q_cv);
	pthread_mutex_unlock(&p->q_mu);
}

static void mux_sink(const uint8_t *msg, size_t len, void *ud)
{
	(void)len;
	q_push((ppusher_t *)ud, msg);
}

static void q_clear(ppusher_t *p)
{
	pthread_mutex_lock(&p->q_mu);
	p->q_head = p->q_count = 0;
	pthread_mutex_unlock(&p->q_mu);
}

/* wait up to ms for a message; returns 1 and copies it into out */
static int q_pop(ppusher_t *p, uint8_t *out, int ms)
{
	int got = 0;
	pthread_mutex_lock(&p->q_mu);
	if (p->q_count == 0 && ms > 0) {
		struct timespec ts;
		int64_t t = pp_wall_ms() + ms;
		ts.tv_sec = (time_t)(t / 1000);
		ts.tv_nsec = (long)(t % 1000) * 1000000L;
		pthread_cond_timedwait(&p->q_cv, &p->q_mu, &ts);
	}
	if (p->q_count > 0) {
		memcpy(out, p->q + p->q_head * PP_TS_MSG_SIZE, PP_TS_MSG_SIZE);
		p->q_head = (p->q_head + 1) % p->q_cap;
		p->q_count--;
		got = 1;
	}
	pthread_mutex_unlock(&p->q_mu);
	return got;
}

static size_t q_fill_pct(ppusher_t *p)
{
	size_t n;
	pthread_mutex_lock(&p->q_mu);
	n = p->q_count * 100 / p->q_cap;
	pthread_mutex_unlock(&p->q_mu);
	return n;
}

/* ------------------------------------------------------------------ media API */

static void req_keyframe(ppusher_t *p)
{
	p->last_kf_req_ms = pp_now_ms();
	if (p->cb.request_keyframe)
		p->cb.request_keyframe(p->cb.ud);
}

int ppusher_push_video(ppusher_t *p, const uint8_t *au, size_t len, int64_t pts_us, int64_t dts_us, int is_key)
{
	int rc = PPUSHER_OK, want_kf = 0;

	if (!p || !au || !len)
		return PPUSHER_E_ARG;
	if (!p->accept_media)
		return PPUSHER_E_NOT_CONNECTED;
	pthread_mutex_lock(&p->mux_mu);
	if (!p->ts) {
		rc = PPUSHER_E_NOT_CONNECTED;
	} else if (p->need_key && !is_key) {
		p->frames_dropped++;
		rc = PPUSHER_E_DROPPED;
	} else if (!is_key && q_fill_pct(p) >= 75) {
		/* the link can't keep up: shed non-key frames until the next IDR */
		p->frames_dropped++;
		p->need_key = 1;
		want_kf = 1;
		rc = PPUSHER_E_DROPPED;
	} else {
		int r = pp_ts_write_video(p->ts, au, len, pts_us * 9 / 100, dts_us * 9 / 100, is_key,
					  p->cfg.enable_sei_timestamp ? pp_wall_ms() : 0);
		if (r == 0)
			p->need_key = 0;
		else
			rc = r == -2 ? PPUSHER_E_NOMEM : PPUSHER_E_MUX;
	}
	pthread_mutex_unlock(&p->mux_mu);
	if (want_kf)
		req_keyframe(p);
	return rc;
}

int ppusher_push_audio(ppusher_t *p, const uint8_t *fr, size_t len, int64_t pts_us)
{
	int rc = PPUSHER_OK;
	if (!p || !fr || !len)
		return PPUSHER_E_ARG;
	if (!p->accept_media || p->cfg.audio_codec == PPUSHER_AUDIO_NONE)
		return p->accept_media ? PPUSHER_E_ARG : PPUSHER_E_NOT_CONNECTED;
	pthread_mutex_lock(&p->mux_mu);
	if (!p->ts) {
		rc = PPUSHER_E_NOT_CONNECTED;
	} else {
		int r = pp_ts_write_audio(p->ts, fr, len, pts_us * 9 / 100);
		if (r != 0)
			rc = PPUSHER_E_MUX;
		p->last_audio_ms = pp_now_ms();
	}
	pthread_mutex_unlock(&p->mux_mu);
	return rc;
}

/* audio-only tails would otherwise wait for the next video AU to complete a 1316-byte message */
static void flush_stale_audio(ppusher_t *p)
{
	if (pthread_mutex_trylock(&p->mux_mu) != 0)
		return;
	if (p->ts && pp_ts_pending(p->ts) > 0 && pp_now_ms() - p->last_audio_ms >= 25)
		pp_ts_flush(p->ts);
	pthread_mutex_unlock(&p->mux_mu);
}

/* ------------------------------------------------------------------ degrade WebSocket thread */

static void ws_handle_message(ppusher_t *p, const char *json, size_t n)
{
	pp_degrade_msg m;

	if (pp_degrade_parse(json, n, &m) != 0)
		return;
	if (m.type == PP_MSG_TARGET_STATE) {
		int flags;
		pthread_mutex_lock(&p->mu);
		flags = pp_target_apply(&p->tgt, &m);
		if (m.latency_ms > 0)
			p->latency_hint_ms = m.latency_ms;
		if (flags & PP_TGT_BITRATE_CHANGED) {
			p->bitrate_pct = m.bitrate_percent;
			p->bitrate_dirty = 1;
		}
		if (flags & PP_TGT_RECONNECT)
			p->ev_reconnect = 1;
		pthread_mutex_unlock(&p->mu);
		plog(p, PPUSHER_LOG_INFO, "TARGET_STATE layers=%d bitrate=%d%% latency=%dms%s%s%s", m.layers,
		     m.bitrate_percent, m.latency_ms, m.reconnect ? " reconnect" : "", m.reason[0] ? " reason=" : "",
		     m.reason);
	} else if (m.type == PP_MSG_LOSS_SAMPLE) {
		int trip, have = 0;
		double pct = 0;
		pthread_mutex_lock(&p->mu);
		/* net of retransmission already: retrans = 0 (see publish-degrade-protocol 6.4a) */
		trip = pp_loss_record(&p->loss, m.unrecoverable_delta, 0, m.expected_delta, pp_now_ms(), &have, &pct);
		if (have)
			p->last_loss_pct = pct;
		if (trip)
			p->ev_loss = 1;
		pthread_mutex_unlock(&p->mu);
		if (have && pct > 0.5)
			plog(p, PPUSHER_LOG_INFO, "mmx-reported loss %.2f%% (unrecoverable %llu / %llu)", pct,
			     (unsigned long long)m.unrecoverable_delta, (unsigned long long)m.expected_delta);
		if (trip)
			plog(p, PPUSHER_LOG_WARN, "windowed unrecoverable loss over %.2f%%: reconnecting",
			     p->cfg.loss_threshold_pct);
	} else if (m.type == PP_MSG_ALERT) {
		plog(p, PPUSHER_LOG_WARN, "ALERT from ppmmx: %s", m.reason);
	}
}

static void *ws_thread(void *arg)
{
	ppusher_t *p = (ppusher_t *)arg;
	pp_backoff bo;
	int64_t next_try = 0;
	char *buf = (char *)malloc(65536);

	pp_backoff_init(&bo, 2000, 30000); /* publish-degrade-protocol.zh-CN.md 7.3 */
	while (!p->stop && buf) {
		ws_target t;
		pp_ws *ws;
		char err[160] = "";
		pp_net_opts o;
		int retarget = 0;

		pthread_mutex_lock(&p->mu);
		t = p->wt;
		pthread_mutex_unlock(&p->mu);
		if (!t.url[0] || pp_now_ms() < next_try) {
			pp_sleep_ms(100);
			continue;
		}
		memset(&o, 0, sizeof o);
		o.verify = p->cfg.tls_verify;
		o.ca_file = p->cfg.ca_file;
		o.timeout_ms = 8000;
		ws = pp_ws_connect(t.url, t.token, &o, err, sizeof err);
		if (!ws) {
			int d = pp_backoff_next(&bo, pp_rand32());
			plog(p, PPUSHER_LOG_WARN, "degrade WebSocket: %s (retry in %d ms)", err, d);
			next_try = pp_now_ms() + d;
			continue;
		}
		pp_backoff_reset(&bo);
		plog(p, PPUSHER_LOG_INFO, "degrade WebSocket connected");
		{
			char hello[96];
			snprintf(hello, sizeof hello, "{\"type\":\"CLIENT_INFO\",\"client\":\"ppusher\",\"version\":\"%s\"}",
				 PPUSHER_VERSION);
			pp_ws_send_text(ws, hello, strlen(hello));
		}
		pthread_mutex_lock(&p->mu);
		p->ws_connected = 1;
		pthread_mutex_unlock(&p->mu);
		for (;;) {
			int r;
			pthread_mutex_lock(&p->mu);
			retarget = p->wt.gen != t.gen;
			pthread_mutex_unlock(&p->mu);
			if (p->stop || retarget)
				break;
			r = pp_ws_recv(ws, buf, 65536, 500);
			if (r == PP_IO_TIMEOUT)
				continue;
			if (r < 0)
				break;
			ws_handle_message(p, buf, (size_t)r);
		}
		pthread_mutex_lock(&p->mu);
		p->ws_connected = 0;
		pthread_mutex_unlock(&p->mu);
		pp_ws_close(ws);
		plog(p, PPUSHER_LOG_INFO, "degrade WebSocket closed");
		/* we closed it ourselves because the session moved on (new token / new node): reconnect at once */
		next_try = retarget ? 0 : pp_now_ms() + pp_backoff_next(&bo, pp_rand32());
	}
	free(buf);
	return NULL;
}

static void ws_set_target(ppusher_t *p, const char *url, const char *token)
{
	pthread_mutex_lock(&p->mu);
	snprintf(p->wt.url, sizeof p->wt.url, "%s", url ? url : "");
	snprintf(p->wt.token, sizeof p->wt.token, "%s", token ? token : "");
	p->wt.gen++;
	pthread_mutex_unlock(&p->mu);
}

/* ------------------------------------------------------------------ SRT */

static void link_close(ppusher_t *p)
{
	ws_set_target(p, NULL, NULL);
	p->accept_media = 0;
	if (p->sock != SRT_INVALID_SOCK) {
		srt_close(p->sock);
		p->sock = SRT_INVALID_SOCK;
	}
	pthread_mutex_lock(&p->mux_mu);
	if (p->ts) {
		pp_ts_destroy(p->ts);
		p->ts = NULL;
	}
	pthread_mutex_unlock(&p->mux_mu);
	q_clear(p);
}

static int do_fetch_event(ppusher_t *p)
{
	pp_publish_decision *d = p->dec;
	char err[256] = "";
	int codec = p->cfg.codec_path ? p->cfg.video_codec : PP_CODEC_NONE;
	const char *token;
	char mask[48];
	int rc, sr;

	set_state(p, PPUSHER_STATE_FETCHING, "");
	if (p->s_dep[0]) {
		memset(d, 0, sizeof *d);
		snprintf(d->endpoint, sizeof d->endpoint, "%s", p->s_dep);
		snprintf(d->token_codec[1], PP_TOKEN_MAX, "%s", p->s_dtok);
		snprintf(d->token_codec[2], PP_TOKEN_MAX, "%s", p->s_dtok);
		snprintf(d->token, PP_TOKEN_MAX, "%s", p->s_dtok);
		snprintf(d->ws_url[p->cfg.video_codec], PP_WSURL_MAX, "%s", p->s_dws);
		snprintf(d->ws_token[p->cfg.video_codec], PP_TOKEN_MAX, "%s", p->s_dwst);
		rc = PP_FETCH_OK;
	} else {
		pp_ppcenter_cfg c;
		memset(&c, 0, sizeof c);
		c.url = p->s_url;
		c.app_id = p->s_app;
		c.app_secret = p->s_secret;
		c.stream_name = p->s_stream;
		c.region = p->s_region;
		c.want_degrade = p->cfg.enable_degrade && p->cfg.codec_path;
		c.tls_verify = p->cfg.tls_verify;
		c.ca_file = p->cfg.ca_file;
		c.timeout_ms = p->cfg.http_timeout_ms;
		rc = pp_ppcenter_fetch(&c, d, err, sizeof err);
	}
	if (rc != PP_FETCH_OK) {
		plog(p, rc == PP_FETCH_FATAL ? PPUSHER_LOG_ERROR : PPUSHER_LOG_WARN, "%s", err);
		return rc == PP_FETCH_SLOW ? PP_EV_FETCH_SLOW : rc == PP_FETCH_FATAL ? PP_EV_FETCH_FATAL : PP_EV_FETCH_RETRY;
	}

	token = pp_decision_token(d, codec);
	sr = pp_streamid_build(p->streamid, sizeof p->streamid, p->s_app, p->s_stream, codec, token);
	if (sr != PP_SID_OK) {
		plog(p, PPUSHER_LOG_ERROR, "cannot build streamid (%s)", sr == PP_SID_TOO_LONG ? "too long" : "invalid token or name");
		return PP_EV_FETCH_FATAL;
	}
	snprintf(p->endpoint, sizeof p->endpoint, "%s", d->endpoint);
	pp_mask_secret(token, mask, sizeof mask);
	plog(p, PPUSHER_LOG_INFO, "publish node %s, path %s/%s%s%s, token %s", d->endpoint, p->s_app, p->s_stream,
	     codec ? "/" : "", pp_codec_name(codec), mask);
	if (!token[0])
		plog(p, PPUSHER_LOG_WARN, "ppcenter returned no token for this path (mmx must not require one)");
	return PP_EV_FETCH_OK;
}

static int do_connect_event(ppusher_t *p)
{
	char host[256], portstr[16];
	int port, v, lat;
	struct addrinfo hints, *res = NULL, *ai;
	pp_ts_config tc;
	int ok = 0;

	set_state(p, PPUSHER_STATE_CONNECTING, p->endpoint);
	if (pp_split_endpoint(p->endpoint, host, sizeof host, &port, SRT_DEFAULT_PORT) != 0) {
		plog(p, PPUSHER_LOG_ERROR, "bad SRT endpoint \"%s\"", p->endpoint);
		return PP_EV_CONNECT_FAIL;
	}
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	snprintf(portstr, sizeof portstr, "%d", port);
	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
		plog(p, PPUSHER_LOG_WARN, "cannot resolve %s", host);
		return PP_EV_CONNECT_FAIL;
	}

	pthread_mutex_lock(&p->mu);
	lat = p->cfg.srt_latency_ms > p->latency_hint_ms ? p->cfg.srt_latency_ms : p->latency_hint_ms;
	pthread_mutex_unlock(&p->mu);

	for (ai = res; ai && !ok && !p->stop; ai = ai->ai_next) {
		SRTSOCKET s = srt_create_socket();
		int live = SRTT_LIVE, payload = PP_TS_MSG_SIZE, yes = 1, ct = 5000, sndto = 1000, idle = 5000;
		if (s == SRT_INVALID_SOCK)
			continue;
		srt_setsockflag(s, SRTO_TRANSTYPE, &live, sizeof live);
		srt_setsockflag(s, SRTO_PAYLOADSIZE, &payload, sizeof payload);
		srt_setsockflag(s, SRTO_STREAMID, p->streamid, (int)strlen(p->streamid));
		srt_setsockflag(s, SRTO_CONNTIMEO, &ct, sizeof ct);
		srt_setsockflag(s, SRTO_SNDTIMEO, &sndto, sizeof sndto);
		srt_setsockflag(s, SRTO_PEERIDLETIMEO, &idle, sizeof idle);
		srt_setsockflag(s, SRTO_SENDER, &yes, sizeof yes);
		if (lat > 0)
			srt_setsockflag(s, SRTO_LATENCY, &lat, sizeof lat); /* TSBPD delay we ask of the receiver */
		if (p->cfg.passphrase && p->cfg.passphrase[0])
			srt_setsockflag(s, SRTO_PASSPHRASE, p->cfg.passphrase, (int)strlen(p->cfg.passphrase));
		if (srt_connect(s, ai->ai_addr, (int)ai->ai_addrlen) == SRT_ERROR) {
			int rej = srt_getrejectreason(s);
			plog(p, PPUSHER_LOG_WARN, "SRT connect to %s:%d failed: %s%s%s", host, port, srt_getlasterror_str(),
			     rej != SRT_REJ_UNKNOWN ? " / " : "", rej != SRT_REJ_UNKNOWN ? srt_rejectreason_str(rej) : "");
			srt_close(s);
			continue;
		}
		p->sock = s;
		ok = 1;
	}
	freeaddrinfo(res);
	if (!ok)
		return PP_EV_CONNECT_FAIL;

	memset(&tc, 0, sizeof tc);
	tc.video_codec = p->cfg.video_codec == PPUSHER_CODEC_HEVC ? PP_CODEC_HEVC : PP_CODEC_H264;
	tc.audio_codec = p->cfg.audio_codec;
	tc.audio_channels = p->cfg.audio_channels;
	tc.audio_sample_rate = p->cfg.audio_sample_rate;
	tc.sei_timestamp = p->cfg.enable_sei_timestamp;
	tc.sink = mux_sink;
	tc.ud = p;
	q_clear(p);
	pthread_mutex_lock(&p->mux_mu);
	p->ts = pp_ts_create(&tc);
	if (p->ts)
		pp_ts_write_tables(p->ts); /* PAT+PMT lead the stream */
	if (p->ts)
		pp_ts_flush(p->ts);
	p->need_key = 1;
	p->last_audio_ms = pp_now_ms();
	pthread_mutex_unlock(&p->mux_mu);
	if (!p->ts) {
		srt_close(p->sock);
		p->sock = SRT_INVALID_SOCK;
		return PP_EV_CONNECT_FAIL;
	}

	pthread_mutex_lock(&p->mu);
	pp_loss_init(&p->loss, &p->loss_cfg); /* a loss window from the previous connection must not count */
	p->ev_reconnect = p->ev_loss = 0;
	if (p->have_streamed)
		p->reconnects++;
	p->have_streamed = 1;
	pthread_mutex_unlock(&p->mu);
	p->stats_primed = 0;
	p->last_stats_ms = 0;
	p->tail_timeouts = 0;

	p->accept_media = 1;
	set_state(p, PPUSHER_STATE_STREAMING, p->endpoint);
	plog(p, PPUSHER_LOG_INFO, "SRT connected to %s:%d", host, port);
	req_keyframe(p);
	v = p->cfg.video_codec;
	if (p->cfg.enable_degrade && p->cfg.codec_path && p->dec->ws_url[v][0])
		ws_set_target(p, p->dec->ws_url[v], p->dec->ws_token[v]);
	else if (p->cfg.enable_degrade)
		plog(p, PPUSHER_LOG_INFO, "no degrade control channel offered for this node; running without it");
	return PP_EV_CONNECT_OK;
}

/* One slice of STREAMING (<= ~50 ms). Returns an event for the state machine, or -1 for none. */
static int streaming_step(ppusher_t *p)
{
	uint8_t msg[PP_TS_MSG_SIZE];
	int budget = 64, ev_ws = 0;
	int64_t now;
	int dirty = 0, pct = 100;

	while (budget-- > 0 && !p->stop) {
		if (!q_pop(p, msg, budget == 63 ? 40 : 0))
			break;
		if (srt_sendmsg(p->sock, (const char *)msg, PP_TS_MSG_SIZE, -1, 1) == SRT_ERROR) {
			int e = srt_getlasterror(NULL);
			if (e == SRT_ETIMEOUT || e == SRT_EASYNCSND) {
				pthread_mutex_lock(&p->q_mu);
				p->msgs_dropped++;
				pthread_mutex_unlock(&p->q_mu);
				if (++p->tail_timeouts > 5) {
					plog(p, PPUSHER_LOG_WARN, "SRT send stalled for too long");
					return PP_EV_LINK_BROKEN;
				}
				continue;
			}
			plog(p, PPUSHER_LOG_WARN, "SRT send failed: %s", srt_getlasterror_str());
			return PP_EV_LINK_BROKEN;
		}
		p->tail_timeouts = 0;
		pthread_mutex_lock(&p->q_mu);
		p->msgs_sent++;
		pthread_mutex_unlock(&p->q_mu);
	}
	flush_stale_audio(p);

	now = pp_now_ms();
	if (p->need_key && now - p->last_kf_req_ms >= KEYFRAME_RETRY_MS)
		req_keyframe(p);

	if (now - p->last_stats_ms >= SRT_STATS_MS) {
		SRT_TRACEBSTATS st;
		p->last_stats_ms = now;
		if (srt_getsockstate(p->sock) != SRTS_CONNECTED) {
			plog(p, PPUSHER_LOG_WARN, "SRT connection lost");
			return PP_EV_LINK_BROKEN;
		}
		if (srt_bistats(p->sock, &st, 0, 1) == 0) {
			int64_t sent = st.pktSentTotal, loss = st.pktSndLossTotal, retrans = st.pktRetransTotal;
			int use_local, trip = 0, have = 0;
			double pct_s = 0;
			pthread_mutex_lock(&p->mu);
			p->rtt_ms = (int)st.msRTT;
			use_local = !p->ws_connected;
			pthread_mutex_unlock(&p->mu);
			if (!p->stats_primed || sent < p->prev_sent || loss < p->prev_loss || retrans < p->prev_retrans) {
				p->stats_primed = 1;
			} else if (use_local) {
				/* sender-side view (ppobs mpegts_sample_srt_loss); stands down while mmx's LOSS_SAMPLE flows */
				int64_t dl = loss - p->prev_loss;
				pthread_mutex_lock(&p->mu);
				trip = pp_loss_record(&p->loss, (uint64_t)(dl < 0 ? 0 : dl), (uint64_t)(retrans - p->prev_retrans),
						      (uint64_t)(sent - p->prev_sent), now, &have, &pct_s);
				if (have)
					p->last_loss_pct = pct_s;
				pthread_mutex_unlock(&p->mu);
				if (have && pct_s > 0.5)
					plog(p, PPUSHER_LOG_INFO, "SRT loss %.2f%% this period", pct_s);
				if (trip) {
					plog(p, PPUSHER_LOG_WARN, "SRT windowed unrecoverable loss over %.2f%%: reconnecting",
					     p->cfg.loss_threshold_pct);
					return PP_EV_LOSS_TRIP;
				}
			}
			p->prev_sent = sent;
			p->prev_loss = loss;
			p->prev_retrans = retrans;
		}
	}

	pthread_mutex_lock(&p->mu);
	if (p->bitrate_dirty) {
		dirty = 1;
		pct = p->bitrate_pct;
		p->bitrate_dirty = 0;
	}
	if (p->ev_reconnect) {
		p->ev_reconnect = 0;
		ev_ws = PP_EV_RECONNECT_REQ;
	} else if (p->ev_loss) {
		p->ev_loss = 0;
		ev_ws = PP_EV_LOSS_TRIP;
	}
	pthread_mutex_unlock(&p->mu);
	if (dirty && p->cb.set_bitrate && p->cfg.video_bitrate_kbps > 0) {
		int kbps = (int)((int64_t)p->cfg.video_bitrate_kbps * pct / 100);
		plog(p, PPUSHER_LOG_INFO, "encoder bitrate -> %d kbps (%d%%)", kbps, pct);
		p->cb.set_bitrate(kbps, p->cb.ud);
	}
	if (ev_ws) {
		if (ev_ws == PP_EV_RECONNECT_REQ)
			plog(p, PPUSHER_LOG_INFO, "ppmmx asked for a reconnect (new SRT latency applies on the next handshake)");
		return ev_ws;
	}
	return -1;
}

static void *session_thread(void *arg)
{
	ppusher_t *p = (ppusher_t *)arg;
	pp_action act = pp_sm_handle(&p->sm, PP_EV_START, pp_now_ms(), 0);

	(void)act;
	while (!p->stop) {
		int ev = -1;
		pp_action a = PP_ACT_NONE;
		int64_t now = pp_now_ms();

		switch (p->sm.state) {
		case PP_ST_FETCH:
			ev = do_fetch_event(p);
			break;
		case PP_ST_CONNECT:
			ev = do_connect_event(p);
			break;
		case PP_ST_STREAMING:
			ev = streaming_step(p);
			if (ev < 0)
				a = pp_sm_handle(&p->sm, PP_EV_TICK, now, 0);
			break;
		case PP_ST_BACKOFF:
			pp_sleep_ms(50);
			a = pp_sm_handle(&p->sm, PP_EV_TICK, pp_now_ms(), 0);
			break;
		default:
			pp_sleep_ms(100);
			break;
		}
		(void)a;
		if (p->stop)
			break;
		if (ev >= 0) {
			int prev = p->sm.state;
			a = pp_sm_handle(&p->sm, (pp_event)ev, pp_now_ms(), pp_rand32());
			if (a == PP_ACT_CLOSE_AND_FETCH || ev == PP_EV_LINK_BROKEN || ev == PP_EV_CONNECT_FAIL)
				link_close(p);
			if (p->sm.state == PP_ST_BACKOFF && prev != PP_ST_BACKOFF) {
				char info[48];
				snprintf(info, sizeof info, "retry in %lld ms", (long long)(p->sm.wake_ms - pp_now_ms()));
				set_state(p, PPUSHER_STATE_BACKOFF, info);
				plog(p, PPUSHER_LOG_INFO, "reconnecting: %s", info);
			} else if (p->sm.state == PP_ST_FATAL) {
				set_state(p, PPUSHER_STATE_FATAL, "credentials rejected");
				plog(p, PPUSHER_LOG_ERROR, "giving up: ppcenter rejected the credentials; fix them and restart");
			}
		}
	}
	link_close(p);
	pp_sm_handle(&p->sm, PP_EV_STOP, pp_now_ms(), 0);
	set_state(p, PPUSHER_STATE_STOPPED, "");
	return NULL;
}

/* ------------------------------------------------------------------ lifecycle */

int ppusher_start(ppusher_t *p)
{
	if (!p || p->started)
		return PPUSHER_E_ARG;
	p->stop = 0;
	if (pthread_create(&p->th_session, NULL, session_thread, p) != 0)
		return PPUSHER_E_NOMEM;
	p->started = 1;
	if (p->cfg.enable_degrade) {
		if (pthread_create(&p->th_ws, NULL, ws_thread, p) == 0)
			p->ws_started = 1;
	}
	return PPUSHER_OK;
}

void ppusher_stop(ppusher_t *p)
{
	if (!p || !p->started)
		return;
	p->stop = 1;
	pthread_mutex_lock(&p->q_mu);
	pthread_cond_broadcast(&p->q_cv);
	pthread_mutex_unlock(&p->q_mu);
	pthread_join(p->th_session, NULL);
	if (p->ws_started)
		pthread_join(p->th_ws, NULL);
	p->started = p->ws_started = 0;
}

void ppusher_destroy(ppusher_t *p)
{
	if (!p)
		return;
	ppusher_stop(p);
	pthread_mutex_destroy(&p->mu);
	pthread_mutex_destroy(&p->mux_mu);
	pthread_mutex_destroy(&p->q_mu);
	pthread_cond_destroy(&p->q_cv);
	srt_cleanup();
	memset(p->s_secret, 0, sizeof p->s_secret);
	free(p->q);
	free(p->dec);
	free(p);
}

int ppusher_get_stats(ppusher_t *p, ppusher_stats_t *o)
{
	if (!p || !o)
		return PPUSHER_E_ARG;
	memset(o, 0, sizeof *o);
	pthread_mutex_lock(&p->mu);
	o->state = p->state_pub;
	o->reconnects = p->reconnects;
	o->bitrate_percent = p->bitrate_pct;
	o->degrade_connected = p->ws_connected;
	o->last_loss_pct = p->last_loss_pct;
	o->rtt_ms = p->rtt_ms;
	pthread_mutex_unlock(&p->mu);
	pthread_mutex_lock(&p->q_mu);
	o->msgs_sent = p->msgs_sent;
	o->msgs_dropped = p->msgs_dropped;
	pthread_mutex_unlock(&p->q_mu);
	o->frames_dropped = p->frames_dropped;
	return PPUSHER_OK;
}
