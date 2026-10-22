/*
 * Session-layer integration test: the real libppusher (ppcenter fetch -> SRT
 * connect -> stream, degrade WebSocket, reconnect state machine) against
 *   - a mock ppcenter (HTTP), handing out a different token on every request
 *   - a mock ppmmx degrade WebSocket (TARGET_STATE / LOSS_SAMPLE pushes)
 *   - a real libsrt listener standing in for the ppmmx SRT ingest
 */
#include "pp_test.h"
#include "ppusher/ppusher.h"
#include "pp_platform.h"
#include "pp_ws.h"

#include <srt/srt.h>

#include <pthread.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define closesock closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define closesock close
#endif

/* ------------------------------------------------------------ looping mock servers */

typedef struct mock mock_t;
typedef void (*handler_fn)(mock_t *m, sock_t c, int nth);
struct mock {
	sock_t ls;
	int port;
	handler_fn fn;
	pthread_t th;
	volatile int stop;
	int accepted;
	int status_override; /* HTTP mock: 0 = normal, else answer with this status */
};

static void *mock_loop(void *a)
{
	mock_t *m = (mock_t *)a;
	while (!m->stop) {
		sock_t c = accept(m->ls, NULL, NULL);
		if (c == (sock_t)-1 || m->stop)
			break;
		m->accepted++;
		m->fn(m, c, m->accepted);
		closesock(c);
	}
	return NULL;
}

static int mock_start(mock_t *m, handler_fn fn)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof sa;
#ifdef _WIN32
	WSADATA w;
	WSAStartup(MAKEWORD(2, 2), &w);
#endif
	memset(m, 0, sizeof *m);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	m->ls = socket(AF_INET, SOCK_STREAM, 0);
	if (bind(m->ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(m->ls, 4) != 0)
		return -1;
	getsockname(m->ls, (struct sockaddr *)&sa, &sl);
	m->port = ntohs(sa.sin_port);
	m->fn = fn;
	return pthread_create(&m->th, NULL, mock_loop, m);
}

static void mock_stop(mock_t *m)
{
	sock_t c = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa;
	m->stop = 1;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)m->port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	connect(c, (struct sockaddr *)&sa, sizeof sa);
	closesock(c);
	pthread_join(m->th, NULL);
	closesock(m->ls);
}

static int send_all(sock_t c, const void *b, size_t n)
{
	const char *p = (const char *)b;
	while (n) {
		int r = send(c, p, (int)n, 0);
		if (r <= 0)
			return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

static int read_request(sock_t c, char *req, size_t cap)
{
	int n = 0, hdr_end = 0, cl = 0;
	memset(req, 0, cap);
	while (n < (int)cap - 1) {
		const char *p;
		if (recv(c, req + n, 1, 0) <= 0)
			break;
		n++;
		if (!hdr_end && n >= 4 && memcmp(req + n - 4, "\r\n\r\n", 4) == 0) {
			hdr_end = n;
			p = strstr(req, "Content-Length: ");
			cl = p ? atoi(p + 16) : 0;
			if (!cl)
				break;
		}
		if (hdr_end && n >= hdr_end + cl)
			break;
	}
	return n;
}

/* ------------------------------------------------------------ shared test state */

static volatile int g_ws_port, g_srt_port;
static char g_tokens_seen[8][64];   /* SRT streamids, in connection order */
static int g_srt_latency[8];        /* receiver TSBPD delay negotiated per connection */
static volatile int g_srt_conns;
static volatile int g_fetches;
static volatile int g_drop_conn;    /* ask the SRT listener to hang up on the current connection */
static volatile long g_srt_bytes[8];
static volatile int g_bad_alignment;
static volatile int g_first_pat_ok;
static char g_ws_bearers[8][64];

/* ------------------------------------------------------------ mock ppcenter */

static void h_ppcenter(mock_t *m, sock_t c, int nth)
{
	char req[4096], body[2048], head[256];
	int n = read_request(c, req, sizeof req), hl, bl;
	(void)n;
	g_fetches = nth;
	if (m->status_override) {
		const char *e = "{\"code\":\"invalid_credentials\",\"message\":\"invalid credentials\"}";
		hl = snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", m->status_override,
			      (int)strlen(e));
		send_all(c, head, (size_t)hl);
		send_all(c, e, strlen(e));
		return;
	}
	bl = snprintf(body, sizeof body,
		      "{\"originNodeId\":1,\"ppcdnPublishEndpoint\":\"127.0.0.1:%d\",\"publishToken\":\"TOKplain-%d\","
		      "\"publishTokens\":{\"h264\":\"TOKh264-%d\",\"hevc\":\"TOKhevc-%d\"},"
		      "\"whipTracks\":{\"h264\":{\"url\":\"http://127.0.0.1:%d/app1/B01/h264/whip\",\"bearerToken\":\"BEAR-%d\"},"
		      "\"hevc\":{\"url\":\"http://127.0.0.1:%d/app1/B01/hevc/whip\",\"bearerToken\":\"BEARhevc-%d\"}}}",
		      g_srt_port, nth, nth, nth, g_ws_port, nth, g_ws_port, nth);
	hl = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", bl);
	send_all(c, head, (size_t)hl);
	send_all(c, body, (size_t)bl);
}

/* ------------------------------------------------------------ mock ppmmx degrade WS */

static void ws_send(sock_t c, const char *p)
{
	uint8_t h[4];
	size_t n = strlen(p), hl = 0;
	h[hl++] = 0x81;
	if (n < 126) {
		h[hl++] = (uint8_t)n;
	} else {
		h[hl++] = 126;
		h[hl++] = (uint8_t)(n >> 8);
		h[hl++] = (uint8_t)n;
	}
	send_all(c, h, hl);
	send_all(c, p, n);
}

typedef enum { WS_BITRATE_THEN_RECONNECT, WS_STEADY, WS_LOSS_STORM } ws_mode;
static volatile ws_mode g_ws_mode;

static void h_ws(mock_t *m, sock_t c, int nth)
{
	char req[2048], key[64] = "", accept[64], resp[400];
	const char *p, *e;
	int i;
	read_request(c, req, sizeof req);
	p = strstr(req, "Sec-WebSocket-Key: ");
	if (!p)
		return;
	p += 19;
	e = strstr(p, "\r\n");
	memcpy(key, p, (size_t)(e - p));
	p = strstr(req, "Authorization: Bearer ");
	if (p && nth <= 8) {
		p += 22;
		e = strstr(p, "\r\n");
		memcpy(g_ws_bearers[nth - 1], p, (size_t)(e - p) < 63 ? (size_t)(e - p) : 63);
	}
	pp_ws_accept_key(key, accept, sizeof accept);
	snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
		 accept);
	send_all(c, resp, strlen(resp));

	if (g_ws_mode == WS_BITRATE_THEN_RECONNECT && nth == 1) {
		ws_send(c, "{\"type\":\"TARGET_STATE\",\"path\":\"app1/B01/h264\",\"codec\":\"h264\",\"layers\":1,\"bitrate_percent\":60,\"latency_ms\":800}");
		pp_sleep_ms(300);
		ws_send(c, "{\"type\":\"TARGET_STATE\",\"path\":\"app1/B01/h264\",\"codec\":\"h264\",\"layers\":1,\"bitrate_percent\":60,\"latency_ms\":800,\"reconnect\":true,\"reason\":\"degrade-latency-raise\"}");
	} else if (g_ws_mode == WS_LOSS_STORM) {
		/* 5% unrecoverable, every 400 ms: over the 1.5% threshold once a full window has elapsed */
		for (i = 0; i < 40 && !m->stop; i++) {
			ws_send(c, "{\"type\":\"LOSS_SAMPLE\",\"path\":\"app1/B01/h264\",\"codec\":\"h264\",\"unrecoverable_delta\":50,\"expected_delta\":1000}");
			pp_sleep_ms(400);
			if (g_fetches > 1 && nth == 1)
				break;
		}
		return;
	} else {
		ws_send(c, "{\"type\":\"TARGET_STATE\",\"path\":\"app1/B01/h264\",\"codec\":\"h264\",\"layers\":1,\"bitrate_percent\":60,\"latency_ms\":800}");
	}
	/* hold the control channel open until the client leaves (read & discard its frames) */
	for (i = 0; i < 100 && !m->stop; i++) {
		uint8_t b[256];
		int r;
		fd_set fds;
		struct timeval tv;
		FD_ZERO(&fds);
		FD_SET(c, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = 200000;
		if (select((int)c + 1, &fds, NULL, NULL, &tv) > 0) {
			r = recv(c, (char *)b, sizeof b, 0);
			if (r <= 0)
				return;
		}
	}
}

/* ------------------------------------------------------------ libsrt listener (stands in for ppmmx) */

static volatile int g_srt_stop;
static SRTSOCKET g_srt_ls;
static pthread_t g_srt_th;

static void *srt_listener(void *a)
{
	(void)a;
	while (!g_srt_stop) {
		struct sockaddr_in peer;
		int plen = sizeof peer;
		char sid[512] = "";
		int sidlen = sizeof sid, idx, rcvto = 200, lat = 0, latlen = sizeof lat;
		uint8_t buf[1500];
		SRTSOCKET acc = srt_accept(g_srt_ls, (struct sockaddr *)&peer, &plen);
		if (acc == SRT_INVALID_SOCK) {
			pp_sleep_ms(10); /* non-blocking listener: poll so shutdown can't wedge in srt_accept */
			continue;
		}
		srt_getsockflag(acc, SRTO_STREAMID, sid, &sidlen);
		srt_getsockflag(acc, SRTO_RCVLATENCY, &lat, &latlen);
		idx = g_srt_conns;
		if (idx < 8) {
			snprintf(g_tokens_seen[idx], sizeof g_tokens_seen[idx], "%.*s", sidlen, sid);
			g_srt_latency[idx] = lat;
		}
		g_srt_conns = idx + 1;
		srt_setsockflag(acc, SRTO_RCVTIMEO, &rcvto, sizeof rcvto);
		for (;;) {
			int r;
			if (g_srt_stop || g_drop_conn) {
				g_drop_conn = 0;
				break;
			}
			r = srt_recvmsg(acc, (char *)buf, sizeof buf);
			if (r == SRT_ERROR) {
				int e = srt_getlasterror(NULL);
				if (e == SRT_ETIMEOUT || e == SRT_EASYNCRCV)
					continue;
				break; /* peer closed / broken */
			}
			if (idx < 8)
				g_srt_bytes[idx] += r;
			if (r % 188 != 0 || buf[0] != 0x47 || (r > 188 && buf[188] != 0x47))
				g_bad_alignment = 1;
			if (idx == 0 && g_srt_bytes[0] == r) /* the very first message of a connection */
				g_first_pat_ok = (buf[1] & 0x5F) == 0x40 && buf[2] == 0x00 && r == 1316;
		}
		srt_close(acc);
	}
	return NULL;
}

static int srt_listener_start(void)
{
	struct sockaddr_in sa;
	int len = sizeof sa;
	srt_startup();
	g_srt_ls = srt_create_socket();
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	{
		int no = 0;
		srt_setsockflag(g_srt_ls, SRTO_RCVSYN, &no, sizeof no);
	}
	if (srt_bind(g_srt_ls, (struct sockaddr *)&sa, sizeof sa) == SRT_ERROR)
		return -1;
	srt_getsockname(g_srt_ls, (struct sockaddr *)&sa, &len);
	g_srt_port = ntohs(sa.sin_port);
	if (srt_listen(g_srt_ls, 4) == SRT_ERROR)
		return -1;
	g_srt_stop = 0;
	g_srt_conns = 0;
	return pthread_create(&g_srt_th, NULL, srt_listener, NULL);
}

static void srt_listener_stop(void)
{
	g_srt_stop = 1;
	srt_close(g_srt_ls);
	pthread_join(g_srt_th, NULL);
	srt_cleanup();
}

/* ------------------------------------------------------------ callbacks */

static volatile int g_bitrate_calls, g_last_kbps, g_kf_calls, g_state;
static volatile int g_saw_fatal;

static void cb_bitrate(int kbps, void *ud) { (void)ud; g_bitrate_calls++; g_last_kbps = kbps; }
static void cb_keyframe(void *ud) { (void)ud; g_kf_calls++; }
static void cb_state(int st, const char *info, void *ud)
{
	(void)info;
	(void)ud;
	g_state = st;
	if (st == PPUSHER_STATE_FATAL)
		g_saw_fatal = 1;
}
static void cb_log(int lvl, const char *msg, void *ud)
{
	(void)ud;
	if (lvl <= PPUSHER_LOG_INFO && getenv("PPUSHER_TEST_VERBOSE"))
		printf("    [log] %s\n", msg);
}

/* a tiny but valid-looking H.264 stream: SPS/PPS/IDR every 15 frames, P frames between */
static size_t make_au(uint8_t *out, int key, int n)
{
	static const uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x40, 0x16, 0xE8, 0x40};
	static const uint8_t pps[] = {0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80};
	size_t o = 0;
	int i;
	if (key) {
		memcpy(out + o, sps, sizeof sps);
		o += sizeof sps;
		memcpy(out + o, pps, sizeof pps);
		o += sizeof pps;
	}
	out[o++] = 0;
	out[o++] = 0;
	out[o++] = 0;
	out[o++] = 1;
	out[o++] = key ? 0x65 : 0x41;
	for (i = 0; i < 1500; i++)
		out[o++] = (uint8_t)((i * 31 + n) | 1);
	return o;
}

static int wait_for(volatile int *v, int target, int ms)
{
	int64_t end = pp_now_ms() + ms;
	while (pp_now_ms() < end) {
		if (*v >= target)
			return 1;
		pp_sleep_ms(20);
	}
	return *v >= target;
}

/* push ~30 fps for `ms`, keyframe on demand + every 15 frames */
static void feed(ppusher_t *p, int ms, int64_t *clock_us)
{
	uint8_t au[4096], op[80];
	int64_t end = pp_now_ms() + ms;
	int n = 0, i;
	memset(op, 7, sizeof op);
	while (pp_now_ms() < end) {
		size_t len = make_au(au, n % 15 == 0, n);
		ppusher_push_video(p, au, len, *clock_us, *clock_us, n % 15 == 0);
		for (i = 0; i < 2; i++)
			ppusher_push_audio(p, op, sizeof op, *clock_us + i * 16000);
		*clock_us += 33333;
		n++;
		pp_sleep_ms(33);
	}
}

static ppusher_t *make(mock_t *center, ppusher_callbacks_t *cb, int degrade)
{
	ppusher_config_t cfg;
	char url[64];
	static char urlbuf[64];
	snprintf(url, sizeof url, "http://127.0.0.1:%d", center->port);
	memcpy(urlbuf, url, sizeof urlbuf);
	ppusher_config_default(&cfg);
	cfg.ppcenter_url = urlbuf;
	cfg.app_id = "app1";
	cfg.app_secret = "sekret";
	cfg.stream_name = "B01";
	cfg.video_codec = PPUSHER_CODEC_H264;
	cfg.audio_codec = PPUSHER_AUDIO_OPUS;
	cfg.audio_channels = 2;
	cfg.video_bitrate_kbps = 2400;
	cfg.enable_degrade = degrade;
	cfg.backoff_min_ms = 200;
	cfg.backoff_max_ms = 800;
	cfg.http_timeout_ms = 2000;
	memset(cb, 0, sizeof *cb);
	cb->set_bitrate = cb_bitrate;
	cb->request_keyframe = cb_keyframe;
	cb->on_state = cb_state;
	cb->on_log = cb_log;
	return ppusher_create(&cfg, cb);
}

static void reset_state(void)
{
	memset(g_tokens_seen, 0, sizeof g_tokens_seen);
	memset(g_srt_latency, 0, sizeof g_srt_latency);
	memset((void *)g_srt_bytes, 0, sizeof g_srt_bytes);
	memset(g_ws_bearers, 0, sizeof g_ws_bearers);
	g_srt_conns = g_fetches = g_bitrate_calls = g_last_kbps = g_kf_calls = g_state = g_saw_fatal = 0;
	g_bad_alignment = g_first_pat_ok = g_drop_conn = 0;
}

/* ------------------------------------------------------------ scenarios */

static void test_reconnect_flow(void)
{
	mock_t center, ws;
	ppusher_callbacks_t cb;
	ppusher_t *p;
	ppusher_stats_t st;
	int64_t clk = 1000000;

	reset_state();
	g_ws_mode = WS_BITRATE_THEN_RECONNECT;
	CHECK_INT(srt_listener_start(), 0);
	mock_start(&ws, h_ws);
	g_ws_port = ws.port;
	mock_start(&center, h_ppcenter);
	p = make(&center, &cb, 1);
	CHECK(p != NULL);
	CHECK_INT(ppusher_start(p), PPUSHER_OK);

	/* connection 1: streamid carries the h264-path token from the 1st ppcenter answer */
	CHECK(wait_for(&g_srt_conns, 1, 4000));
	feed(p, 1500, &clk);
	CHECK_STR(g_tokens_seen[0], "publish:app1/B01/h264:TOKh264-1");
	CHECK(g_kf_calls >= 1);
	CHECK_INT(g_first_pat_ok, 1); /* first message: 1316 bytes starting with a PAT packet */

	/* TARGET_STATE bitrate_percent=60 -> encoder callback with 60% of the nominal 2400 kbps; once, not repeatedly */
	CHECK_INT(g_bitrate_calls, 1);
	CHECK_INT(g_last_kbps, 1440);
	/* the degrade channel authenticated with the whipTracks bearer of the *same* ppcenter answer */
	CHECK_STR(g_ws_bearers[0], "BEAR-1");

	/* reconnect=true was pushed 300 ms after connect: new ppcenter request, new token, new SRT connection */
	CHECK(wait_for(&g_srt_conns, 2, 4000));
	feed(p, 800, &clk);
	CHECK_INT(g_fetches, 2);
	CHECK_STR(g_tokens_seen[1], "publish:app1/B01/h264:TOKh264-2");
	CHECK(g_kf_calls >= 2); /* a keyframe is requested again on the new connection */
	CHECK(g_srt_latency[0] > 0 && g_srt_latency[0] < 500);   /* libsrt default */
	CHECK(g_srt_latency[1] >= 800);                         /* latency_ms from TARGET_STATE applied at the new handshake */
	CHECK_STR(g_ws_bearers[1], "BEAR-2");
	CHECK_INT(g_bitrate_calls, 1); /* identical state re-pushed on the new WS: idempotent */

	/* link dies on the ppmmx side: back off, then fetch AGAIN (never reuse the old token) */
	g_drop_conn = 1;
	CHECK(wait_for(&g_srt_conns, 3, 6000));
	feed(p, 600, &clk);
	CHECK_INT(g_fetches, 3);
	CHECK_STR(g_tokens_seen[2], "publish:app1/B01/h264:TOKh264-3");

	ppusher_get_stats(p, &st);
	CHECK_INT(st.state, PPUSHER_STATE_STREAMING);
	CHECK(st.reconnects >= 2);
	CHECK(st.msgs_sent > 50);
	CHECK(st.degrade_connected == 1);
	CHECK_INT(g_bad_alignment, 0);
	CHECK(g_srt_bytes[0] > 10000 && g_srt_bytes[1] > 10000);

	ppusher_destroy(p);
	mock_stop(&center);
	mock_stop(&ws);
	srt_listener_stop();
}

static void test_auth_failure_is_fatal(void)
{
	mock_t center;
	ppusher_callbacks_t cb;
	ppusher_t *p;

	reset_state();
	CHECK_INT(srt_listener_start(), 0);
	mock_start(&center, h_ppcenter);
	center.status_override = 401;
	p = make(&center, &cb, 1);
	CHECK_INT(ppusher_start(p), PPUSHER_OK);
	CHECK(wait_for(&g_saw_fatal, 1, 3000));
	pp_sleep_ms(1200); /* longer than any backoff: it must not retry */
	CHECK_INT(g_fetches, 1);
	CHECK_INT(g_srt_conns, 0);
	CHECK_INT(ppusher_push_video(p, (const uint8_t *)"\0\0\0\1\x65", 5, 0, 0, 1), PPUSHER_E_NOT_CONNECTED);
	ppusher_destroy(p);
	mock_stop(&center);
	srt_listener_stop();
}

static void test_outage_then_recovery(void)
{
	mock_t center;
	ppusher_callbacks_t cb;
	ppusher_t *p;
	int64_t clk = 5000000;

	reset_state();
	g_ws_mode = WS_STEADY;
	CHECK_INT(srt_listener_start(), 0);
	mock_start(&center, h_ppcenter);
	center.status_override = 503; /* no_healthy_origin */
	p = make(&center, &cb, 0);
	CHECK_INT(ppusher_start(p), PPUSHER_OK);
	CHECK(wait_for(&g_fetches, 3, 5000)); /* keeps retrying with backoff */
	CHECK_INT(g_srt_conns, 0);
	center.status_override = 0; /* the origin comes back */
	CHECK(wait_for(&g_srt_conns, 1, 5000));
	feed(p, 500, &clk);
	CHECK(g_srt_bytes[0] > 5000);
	CHECK_INT(g_state, PPUSHER_STATE_STREAMING);
	ppusher_destroy(p);
	mock_stop(&center);
	srt_listener_stop();
}

static void test_loss_samples_trigger_reconnect(void)
{
	mock_t center, ws;
	ppusher_callbacks_t cb;
	ppusher_config_t cfg;
	ppusher_t *p;
	char url[64];
	int64_t clk = 9000000;

	reset_state();
	g_ws_mode = WS_LOSS_STORM;
	CHECK_INT(srt_listener_start(), 0);
	mock_start(&ws, h_ws);
	g_ws_port = ws.port;
	mock_start(&center, h_ppcenter);
	/* a short window so the test runs in seconds: 1 s period, 2 s window, 1.5 % */
	snprintf(url, sizeof url, "http://127.0.0.1:%d", center.port);
	ppusher_config_default(&cfg);
	cfg.ppcenter_url = url;
	cfg.app_id = "app1";
	cfg.app_secret = "sekret";
	cfg.stream_name = "B01";
	cfg.video_bitrate_kbps = 2400;
	cfg.backoff_min_ms = 200;
	cfg.backoff_max_ms = 800;
	cfg.loss_period_sec = 1;
	cfg.loss_window_sec = 2;
	cfg.loss_threshold_pct = 1.5;
	memset(&cb, 0, sizeof cb);
	cb.request_keyframe = cb_keyframe;
	cb.on_state = cb_state;
	cb.on_log = cb_log;
	p = ppusher_create(&cfg, &cb);
	CHECK(p != NULL);
	ppusher_start(p);
	CHECK(wait_for(&g_srt_conns, 1, 4000));
	feed(p, 200, &clk);
	/* mmx keeps reporting ~5% unrecoverable loss: after one full window we reconnect with a fresh token */
	CHECK(wait_for(&g_srt_conns, 2, 9000));
	feed(p, 300, &clk);
	CHECK(g_fetches >= 2);
	CHECK_STR(g_tokens_seen[1], "publish:app1/B01/h264:TOKh264-2");
	ppusher_destroy(p);
	mock_stop(&center);
	mock_stop(&ws);
	srt_listener_stop();
}

static void test_api_validation(void)
{
	ppusher_config_t cfg;
	ppusher_callbacks_t cb;
	ppusher_t *p;

	memset(&cb, 0, sizeof cb);
	CHECK(ppusher_create(NULL, &cb) == NULL);
	ppusher_config_default(&cfg);
	CHECK(ppusher_create(&cfg, &cb) == NULL); /* no ids */
	cfg.ppcenter_url = "https://api.pp-cdn.org";
	cfg.app_id = "app1";
	cfg.app_secret = "x";
	cfg.stream_name = "B01/hevc";             /* suffix is ours to add: tolerated, stripped */
	p = ppusher_create(&cfg, &cb);
	CHECK(p != NULL);
	ppusher_destroy(p);
	cfg.stream_name = "bad name";
	CHECK(ppusher_create(&cfg, &cb) == NULL);
	cfg.stream_name = "B01";
	cfg.video_codec = 9;
	CHECK(ppusher_create(&cfg, &cb) == NULL);
	cfg.video_codec = PPUSHER_CODEC_H264;
	cfg.audio_channels = 6;
	CHECK(ppusher_create(&cfg, &cb) == NULL);
	cfg.audio_channels = 2;
	p = ppusher_create(&cfg, &cb);
	CHECK(p != NULL);
	/* never started: frames are rejected, not buffered */
	CHECK_INT(ppusher_push_video(p, (const uint8_t *)"\0\0\0\1\x65", 5, 0, 0, 1), PPUSHER_E_NOT_CONNECTED);
	CHECK_INT(ppusher_push_video(p, NULL, 0, 0, 0, 1), PPUSHER_E_ARG);
	ppusher_destroy(p);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	srt_setloglevel(LOG_CRIT); /* the listener's recv timeouts make libsrt chatty */
	RUN(test_api_validation);
	RUN(test_reconnect_flow);
	RUN(test_auth_failure_is_fatal);
	RUN(test_outage_then_recovery);
	RUN(test_loss_samples_trigger_reconnect);
	TEST_DONE();
}
