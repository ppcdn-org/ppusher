/* HTTP / WebSocket / ppcenter client against in-process loopback mock servers. */
#include "pp_test.h"
#include "pp_degrade.h"
#include "pp_http.h"
#include "pp_net.h"
#include "pp_platform.h"
#include "pp_ppcenter.h"
#include "pp_streamid.h"
#include "pp_ws.h"

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

/* ------------------------------------------------------------ tiny mock server */

typedef void (*handler_fn)(sock_t c);

typedef struct {
	sock_t ls;
	int port;
	handler_fn fn;
	pthread_t th;
} mock_t;

static char g_req[8192]; /* last request seen by an HTTP handler */
static int g_req_n;
static char g_ws_bearer[2048];
static char g_ws_got[4][256]; /* text messages the WS handler received from the client */
static int g_ws_pong_ok, g_ws_closed_seen;

static void *mock_thread(void *a)
{
	mock_t *m = (mock_t *)a;
	sock_t c = accept(m->ls, NULL, NULL);
	if (c != (sock_t)-1) {
		m->fn(c);
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
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	m->ls = socket(AF_INET, SOCK_STREAM, 0);
	if (bind(m->ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(m->ls, 1) != 0)
		return -1;
	getsockname(m->ls, (struct sockaddr *)&sa, &sl);
	m->port = ntohs(sa.sin_port);
	m->fn = fn;
	return pthread_create(&m->th, NULL, mock_thread, m);
}

static void mock_stop(mock_t *m)
{
	/* unblock accept() if the client never connected */
	sock_t c = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa;
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

static int recv_n(sock_t c, uint8_t *b, size_t n)
{
	while (n) {
		int r = recv(c, (char *)b, (int)n, 0);
		if (r <= 0)
			return -1;
		b += r;
		n -= (size_t)r;
	}
	return 0;
}

/* read an HTTP request (headers + Content-Length body) into g_req */
static void read_http_request(sock_t c)
{
	int n = 0, hdr_end = 0, cl = 0;
	memset(g_req, 0, sizeof g_req);
	while (n < (int)sizeof g_req - 1) {
		int r = recv(c, g_req + n, 1, 0);
		if (r <= 0)
			break;
		n++;
		if (!hdr_end && n >= 4 && memcmp(g_req + n - 4, "\r\n\r\n", 4) == 0) {
			const char *p = strstr(g_req, "Content-Length: ");
			hdr_end = n;
			cl = p ? atoi(p + 16) : 0;
			if (cl == 0)
				break;
		}
		if (hdr_end && n >= hdr_end + cl)
			break;
	}
	g_req_n = n;
}

/* ------------------------------------------------------------ canned replies */

static const char *PPCENTER_OK =
	"{\"originNodeId\":7,\"ppcdnPublishEndpoint\":\"origin.example.org:7890\",\"publishToken\":\"TOKplain\","
	"\"publishTokens\":{\"h264\":\"TOKh264\",\"hevc\":\"TOKhevc\"},"
	"\"whipTracks\":{\"h264\":{\"url\":\"https://origin.example.org/app1/B01/h264/whip\",\"bearerToken\":\"BEARh264\"},"
	"\"hevc\":{\"url\":\"https://origin.example.org/app1/B01/hevc/whip\",\"bearerToken\":\"BEARhevc\"}}}";

static void reply(sock_t c, int status, const char *reason, const char *body, int chunked)
{
	char h[512];
	size_t bl = strlen(body);
	int n;
	if (chunked) {
		char *ck = (char *)malloc(bl + 64);
		size_t half = bl / 2, o = 0;
		n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
			     status, reason);
		send_all(c, h, (size_t)n);
		o += (size_t)sprintf(ck + o, "%lx\r\n", (unsigned long)half);
		memcpy(ck + o, body, half);
		o += half;
		memcpy(ck + o, "\r\n", 2);
		o += 2;
		o += (size_t)sprintf(ck + o, "%lx\r\n", (unsigned long)(bl - half));
		memcpy(ck + o, body + half, bl - half);
		o += bl - half;
		o += (size_t)sprintf(ck + o, "\r\n0\r\n\r\n");
		send_all(c, ck, o);
		free(ck);
	} else {
		n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
			     status, reason, (unsigned long)bl);
		send_all(c, h, (size_t)n);
		send_all(c, body, bl);
	}
}

static void h_ok(sock_t c) { read_http_request(c); reply(c, 200, "OK", PPCENTER_OK, 0); }
static void h_ok_chunked(sock_t c) { read_http_request(c); reply(c, 200, "OK", PPCENTER_OK, 1); }
static void h_401(sock_t c) { read_http_request(c); reply(c, 401, "Unauthorized", "{\"code\":\"invalid_credentials\",\"message\":\"invalid credentials\"}", 0); }
static void h_402(sock_t c) { read_http_request(c); reply(c, 402, "Payment Required", "{\"code\":\"account_in_arrears\",\"message\":\"account is inactive or in arrears\"}", 0); }
static void h_503(sock_t c) { read_http_request(c); reply(c, 503, "Service Unavailable", "{\"code\":\"no_healthy_origin\",\"message\":\"no healthy SRT origin is available\"}", 0); }
static void h_redirect(sock_t c)
{
	const char *r = "HTTP/1.1 302 Found\r\nLocation: https://elsewhere/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	read_http_request(c);
	send_all(c, r, strlen(r));
}
static void h_garbage_endpoint(sock_t c) { read_http_request(c); reply(c, 200, "OK", "{\"ok\":true}", 0); }
static void h_close_early(sock_t c) { read_http_request(c); }
static void h_silent(sock_t c)
{
	uint8_t b[1];
	read_http_request(c);
	recv(c, (char *)b, 1, 0); /* hold the connection open until the client gives up */
}
static void h_close_mid_body(sock_t c)
{
	const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 500\r\nConnection: close\r\n\r\n{\"short\":";
	read_http_request(c);
	send_all(c, r, strlen(r));
}

/* ------------------------------------------------------------ WebSocket server side */

static void ws_send(sock_t c, int fin, int op, const char *p, size_t n)
{
	uint8_t h[4];
	size_t hl = 0;
	h[hl++] = (uint8_t)((fin ? 0x80 : 0) | op);
	if (n < 126) {
		h[hl++] = (uint8_t)n;
	} else {
		h[hl++] = 126;
		h[hl++] = (uint8_t)(n >> 8);
		h[hl++] = (uint8_t)n;
	}
	send_all(c, h, hl);
	if (n)
		send_all(c, p, n);
}

/* read one client frame; unmask; returns opcode or -1 */
static int ws_read(sock_t c, char *out, size_t cap, size_t *len)
{
	uint8_t h[2], ext[2], mask[4];
	size_t n;
	size_t i;
	if (recv_n(c, h, 2) != 0)
		return -1;
	n = h[1] & 0x7F;
	if (!(h[1] & 0x80))
		return -1; /* client frames MUST be masked */
	if (n == 126) {
		if (recv_n(c, ext, 2) != 0)
			return -1;
		n = ((size_t)ext[0] << 8) | ext[1];
	}
	if (recv_n(c, mask, 4) != 0 || n >= cap)
		return -1;
	if (n && recv_n(c, (uint8_t *)out, n) != 0)
		return -1;
	for (i = 0; i < n; i++)
		out[i] ^= (char)mask[i & 3];
	out[n] = 0;
	*len = n;
	return h[0] & 0x0F;
}

static int ws_handshake(sock_t c, int send_bad_accept)
{
	char key[64] = "", accept[64], resp[512];
	const char *p, *e;
	read_http_request(c);
	p = strstr(g_req, "Sec-WebSocket-Key: ");
	if (!p)
		return -1;
	p += 19;
	e = strstr(p, "\r\n");
	memcpy(key, p, (size_t)(e - p));
	key[e - p] = 0;
	g_ws_bearer[0] = 0;
	p = strstr(g_req, "Authorization: Bearer ");
	if (p) {
		p += 22;
		e = strstr(p, "\r\n");
		memcpy(g_ws_bearer, p, (size_t)(e - p));
		g_ws_bearer[e - p] = 0;
	}
	pp_ws_accept_key(key, accept, sizeof accept);
	if (send_bad_accept)
		strcpy(accept, "AAAAAAAAAAAAAAAAAAAAAAAAAAA=");
	snprintf(resp, sizeof resp,
		 "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nsec-websocket-accept: %s\r\n\r\n", accept);
	return send_all(c, resp, strlen(resp));
}

static void h_ws_script(sock_t c)
{
	char buf[512];
	size_t n = 0;
	int op;
	if (ws_handshake(c, 0) != 0)
		return;
	/* the real server pushes the current state right after the upgrade */
	{
		const char *ts = "{\"type\":\"TARGET_STATE\",\"path\":\"app1/B01/h264\",\"codec\":\"h264\",\"layers\":1,\"bitrate_percent\":80,\"latency_ms\":300}";
		ws_send(c, 1, 1, ts, strlen(ts));
	}
	/* fragmented text message: "{\"type\":\"ALERT\"," + "\"reason\":\"x\"}" */
	ws_send(c, 0, 1, "{\"type\":\"ALERT\",", 16);
	ws_send(c, 0, 0, "\"reason\":", 9);
	ws_send(c, 1, 0, "\"x\"}", 4);
	/* ping with payload: the client must answer with a pong echoing it */
	ws_send(c, 1, 9, "hello", 5);
	/* client -> server: CLIENT_INFO text, then the pong */
	op = ws_read(c, buf, sizeof buf, &n);
	if (op == 1)
		snprintf(g_ws_got[0], sizeof g_ws_got[0], "%.255s", buf);
	op = ws_read(c, buf, sizeof buf, &n);
	if (op == 0xA && n == 5 && memcmp(buf, "hello", 5) == 0)
		g_ws_pong_ok = 1;
	{
		const char *ls = "{\"type\":\"LOSS_SAMPLE\",\"unrecoverable_delta\":12,\"expected_delta\":580}";
		ws_send(c, 1, 1, ls, strlen(ls));
	}
	{
		const char *rc = "{\"type\":\"TARGET_STATE\",\"layers\":1,\"bitrate_percent\":100,\"reconnect\":true,\"reason\":\"degrade-latency-raise\"}";
		ws_send(c, 1, 1, rc, strlen(rc));
	}
	/* a 300 byte message needs the 16-bit length form */
	{
		char big[300];
		memset(big, 'a', sizeof big);
		memcpy(big, "{\"type\":\"ALERT\",\"reason\":\"", 25);
		memcpy(big + 290, "\"}", 2);
		memset(big + 25, 'z', 265);
		ws_send(c, 1, 1, big, 292);
	}
	/* close: the client should echo a close frame */
	{
		uint8_t code[2] = {0x03, 0xE8};
		ws_send(c, 1, 8, (const char *)code, 2);
	}
	op = ws_read(c, buf, sizeof buf, &n);
	g_ws_closed_seen = op == 8;
}

static void h_ws_badaccept(sock_t c) { ws_handshake(c, 1); }
static void h_ws_refuse(sock_t c)
{
	const char *r = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	read_http_request(c);
	send_all(c, r, strlen(r));
}

/* ------------------------------------------------------------ tests */

static void test_sha1_base64_accept(void)
{
	uint8_t d[20];
	char b[64];
	char acc[64];
	static const uint8_t abc_sha[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
					    0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
	char big[200];

	pp_sha1((const uint8_t *)"abc", 3, d);
	CHECK(memcmp(d, abc_sha, 20) == 0);
	/* the 56-byte FIPS 180 vector needs a second padding block */
	memset(big, 'a', sizeof big);
	pp_sha1((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
	CHECK_INT(d[0], 0x84);
	CHECK_INT(d[1], 0x98);
	CHECK_INT(d[19], 0xf1);
	pp_sha1((const uint8_t *)big, 0, d);
	CHECK_INT(d[0], 0xda);
	CHECK_INT(d[1], 0x39);

	CHECK_INT(pp_base64((const uint8_t *)"", 0, b, sizeof b), 0);
	CHECK_INT(pp_base64((const uint8_t *)"f", 1, b, sizeof b), 4);
	CHECK_STR(b, "Zg==");
	pp_base64((const uint8_t *)"fo", 2, b, sizeof b);
	CHECK_STR(b, "Zm8=");
	pp_base64((const uint8_t *)"foo", 3, b, sizeof b);
	CHECK_STR(b, "Zm9v");
	pp_base64((const uint8_t *)"foobar", 6, b, sizeof b);
	CHECK_STR(b, "Zm9vYmFy");
	CHECK_INT(pp_base64((const uint8_t *)"foobar", 6, b, 4), -1);

	/* the worked example in RFC 6455 section 1.3 */
	CHECK_INT(pp_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc, sizeof acc), 0);
	CHECK_STR(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

static void test_url_parse(void)
{
	pp_url u;
	CHECK_INT(pp_url_parse("https://api.pp-cdn.org", &u), 0);
	CHECK_INT(u.tls, 1);
	CHECK_STR(u.host, "api.pp-cdn.org");
	CHECK_INT(u.port, 443);
	CHECK_STR(u.path, "/");
	CHECK_INT(pp_url_parse("http://h:8080/a/b?c=d", &u), 0);
	CHECK_INT(u.tls, 0);
	CHECK_INT(u.port, 8080);
	CHECK_STR(u.path, "/a/b?c=d");
	CHECK_INT(pp_url_parse("wss://o.example/app/s/h264/ws/whip", &u), 0);
	CHECK_INT(u.tls, 1);
	CHECK_INT(u.port, 443);
	CHECK_STR(u.path, "/app/s/h264/ws/whip");
	CHECK_INT(pp_url_parse("ws://[::1]:9/x", &u), 0);
	CHECK_STR(u.host, "::1");
	CHECK_INT(u.port, 9);
	CHECK_INT(pp_url_parse("ftp://h/", &u), -1);
	CHECK_INT(pp_url_parse("http://", &u), -1);
	CHECK_INT(pp_url_parse("http://h:0/", &u), -1);
	CHECK_INT(pp_url_parse("http://h:99999/", &u), -1);
	CHECK_INT(pp_url_parse("host/path", &u), -1);
}

static void test_dechunk(void)
{
	char out[64];
	CHECK_INT(pp_http_dechunk("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", 26, out, sizeof out), 9);
	CHECK(memcmp(out, "Wikipedia", 9) == 0);
	CHECK_INT(pp_http_dechunk("4;ext=1\r\nWiki\r\n0\r\n\r\n", 20, out, sizeof out), 4);
	CHECK_INT(pp_http_dechunk("4\r\nWi", 5, out, sizeof out), -2);       /* incomplete */
	CHECK_INT(pp_http_dechunk("4\r\nWikiXX0\r\n\r\n", 15, out, sizeof out), -1); /* missing CRLF after data */
	CHECK_INT(pp_http_dechunk("zz\r\n", 4, out, sizeof out), -1);
	CHECK_INT(pp_http_dechunk("FFFFFFFFF\r\n", 11, out, sizeof out), -1);  /* absurd size */
	CHECK_INT(pp_http_dechunk("40\r\n", 4, out, 8), -2);
}

static void test_request_and_response_parsing(void)
{
	char body[1536];
	pp_publish_decision d;
	int n;

	n = pp_ppcenter_build_request(body, sizeof body, "app1", "s3cr\"et", "B01-front", "ap-southeast-1", 1);
	CHECK(n > 0);
	CHECK(strstr(body, "\"appId\":\"app1\"") != NULL);
	CHECK(strstr(body, "\"appSecret\":\"s3cr\\\"et\"") != NULL); /* escaped */
	CHECK(strstr(body, "\"streamName\":\"B01-front\"") != NULL);
	CHECK(strstr(body, "\"protocol\":\"srt\"") != NULL);
	CHECK(strstr(body, "\"capabilities\":[\"whip-hevc-h264\"]") != NULL);
	CHECK(strstr(body, "\"requestRegion\":\"ap-southeast-1\"") != NULL);
	n = pp_ppcenter_build_request(body, sizeof body, "app1", "x", "B01", NULL, 0);
	CHECK(strstr(body, "capabilities") == NULL);
	CHECK(strstr(body, "requestRegion") == NULL);
	CHECK(body[n - 1] == '}');
	CHECK_INT(pp_ppcenter_build_request(body, 20, "app1", "x", "B01", NULL, 0), -1);

	CHECK_INT(pp_ppcenter_parse_response(PPCENTER_OK, strlen(PPCENTER_OK), &d), 0);
	CHECK_STR(d.endpoint, "origin.example.org:7890");
	CHECK_INT(d.origin_node_id, 7);
	CHECK_STR(d.token, "TOKplain");
	CHECK_STR(d.token_codec[PP_CODEC_H264], "TOKh264");
	CHECK_STR(d.token_codec[PP_CODEC_HEVC], "TOKhevc");
	CHECK_STR(d.ws_url[PP_CODEC_H264], "wss://origin.example.org/app1/B01/h264/ws/whip");
	CHECK_STR(d.ws_url[PP_CODEC_HEVC], "wss://origin.example.org/app1/B01/hevc/ws/whip");
	CHECK_STR(d.ws_token[PP_CODEC_HEVC], "BEARhevc");

	/* the streamid token must be the one bound to the exact path */
	CHECK_STR(pp_decision_token(&d, PP_CODEC_HEVC), "TOKhevc");
	CHECK_STR(pp_decision_token(&d, PP_CODEC_H264), "TOKh264");
	CHECK_STR(pp_decision_token(&d, PP_CODEC_NONE), "TOKplain");

	/* origin without authKey / WebRTC base URL: endpoint only */
	{
		const char *minimal = "{\"originNodeId\":1,\"ppcdnPublishEndpoint\":\"10.0.0.1\"}";
		CHECK_INT(pp_ppcenter_parse_response(minimal, strlen(minimal), &d), 0);
		CHECK_STR(d.endpoint, "10.0.0.1");
		CHECK_STR(pp_decision_token(&d, PP_CODEC_H264), "");
		CHECK_STR(d.ws_url[PP_CODEC_H264], "");
	}
	/* a whip URL we cannot turn into a control URL disables degrade for that codec only */
	{
		const char *odd = "{\"ppcdnPublishEndpoint\":\"h:1\",\"whipTracks\":{\"h264\":{\"url\":\"rtsp://x/whip\",\"bearerToken\":\"t\"},"
				  "\"hevc\":{\"url\":\"http://h/a/s/hevc/whip\",\"bearerToken\":\"u\"}}}";
		CHECK_INT(pp_ppcenter_parse_response(odd, strlen(odd), &d), 0);
		CHECK_STR(d.ws_url[PP_CODEC_H264], "");
		CHECK_STR(d.ws_url[PP_CODEC_HEVC], "ws://h/a/s/hevc/ws/whip");
	}
	CHECK_INT(pp_ppcenter_parse_response("{}", 2, &d), -1);
	CHECK_INT(pp_ppcenter_parse_response("garbage", 7, &d), -1);
	CHECK_INT(pp_ppcenter_parse_response("{\"ppcdnPublishEndpoint\":\"\"}", 27, &d), -1);

	CHECK_INT(pp_fetch_class(200), PP_FETCH_OK);
	CHECK_INT(pp_fetch_class(402), PP_FETCH_SLOW);
	CHECK_INT(pp_fetch_class(400), PP_FETCH_FATAL);
	CHECK_INT(pp_fetch_class(401), PP_FETCH_FATAL);
	CHECK_INT(pp_fetch_class(403), PP_FETCH_FATAL);
	CHECK_INT(pp_fetch_class(503), PP_FETCH_RETRY);
	CHECK_INT(pp_fetch_class(500), PP_FETCH_RETRY);
	CHECK_INT(pp_fetch_class(429), PP_FETCH_RETRY);
	CHECK_INT(pp_fetch_class(404), PP_FETCH_RETRY);
	CHECK_INT(pp_fetch_class(0), PP_FETCH_RETRY);
}

static int fetch_with(handler_fn fn, pp_publish_decision *d, char *err, size_t errsz)
{
	mock_t m;
	pp_ppcenter_cfg c;
	char url[64];
	int rc;
	CHECK_INT(mock_start(&m, fn), 0);
	snprintf(url, sizeof url, "http://127.0.0.1:%d", m.port);
	memset(&c, 0, sizeof c);
	c.url = url;
	c.app_id = "app1";
	c.app_secret = "sekret";
	c.stream_name = "B01";
	c.want_degrade = 1;
	c.timeout_ms = 1500;
	rc = pp_ppcenter_fetch(&c, d, err, errsz);
	mock_stop(&m);
	return rc;
}

static void test_fetch_ok(void)
{
	pp_publish_decision d;
	char err[256] = "";

	CHECK_INT(fetch_with(h_ok, &d, err, sizeof err), PP_FETCH_OK);
	CHECK_STR(d.endpoint, "origin.example.org:7890");
	CHECK_STR(d.token_codec[PP_CODEC_HEVC], "TOKhevc");
	CHECK(strstr(g_req, "POST /v1/publish/requests HTTP/1.1\r\n") == g_req);
	CHECK(strstr(g_req, "Content-Type: application/json") != NULL);
	CHECK(strstr(g_req, "Host: 127.0.0.1:") != NULL);
	CHECK(strstr(g_req, "\"protocol\":\"srt\"") != NULL);
	CHECK(strstr(g_req, "\"appSecret\":\"sekret\"") != NULL);

	CHECK_INT(fetch_with(h_ok_chunked, &d, err, sizeof err), PP_FETCH_OK);
	CHECK_STR(d.endpoint, "origin.example.org:7890");
	CHECK_STR(d.ws_token[PP_CODEC_H264], "BEARh264");
}

static void test_fetch_errors(void)
{
	pp_publish_decision d;
	char err[256] = "";

	CHECK_INT(fetch_with(h_401, &d, err, sizeof err), PP_FETCH_FATAL);
	CHECK(strstr(err, "401") && strstr(err, "invalid_credentials"));
	CHECK_INT(fetch_with(h_402, &d, err, sizeof err), PP_FETCH_SLOW);
	CHECK(strstr(err, "account_in_arrears") != NULL);
	CHECK_INT(fetch_with(h_503, &d, err, sizeof err), PP_FETCH_RETRY);
	CHECK(strstr(err, "no_healthy_origin") != NULL);
	CHECK_INT(fetch_with(h_redirect, &d, err, sizeof err), PP_FETCH_RETRY); /* 302 is not followed, not fatal */
	CHECK_INT(fetch_with(h_garbage_endpoint, &d, err, sizeof err), PP_FETCH_RETRY);
	CHECK(strstr(err, "invalid publish decision") != NULL);
	CHECK_INT(fetch_with(h_close_early, &d, err, sizeof err), PP_FETCH_RETRY);
	CHECK_INT(fetch_with(h_close_mid_body, &d, err, sizeof err), PP_FETCH_RETRY);
	{
		int64_t t0 = pp_now_ms();
		pp_ppcenter_cfg c;
		mock_t m;
		char url[64];
		mock_start(&m, h_silent);
		snprintf(url, sizeof url, "http://127.0.0.1:%d", m.port);
		memset(&c, 0, sizeof c);
		c.url = url;
		c.app_id = "a";
		c.app_secret = "s";
		c.stream_name = "b";
		c.timeout_ms = 400;
		CHECK_INT(pp_ppcenter_fetch(&c, &d, err, sizeof err), PP_FETCH_RETRY);
		CHECK(strstr(err, "timed out") != NULL);
		CHECK(pp_now_ms() - t0 < 3000);
		mock_stop(&m);
	}
	/* nothing listening */
	{
		pp_ppcenter_cfg c;
		memset(&c, 0, sizeof c);
		c.url = "http://127.0.0.1:1";
		c.app_id = "a";
		c.app_secret = "s";
		c.stream_name = "b";
		c.timeout_ms = 800;
		CHECK_INT(pp_ppcenter_fetch(&c, &d, err, sizeof err), PP_FETCH_RETRY);
	}
	/* a bad URL is a config bug, not an outage */
	{
		pp_ppcenter_cfg c;
		memset(&c, 0, sizeof c);
		c.url = "api.pp-cdn.org";
		c.app_id = "a";
		c.app_secret = "s";
		c.stream_name = "b";
		CHECK_INT(pp_ppcenter_fetch(&c, &d, err, sizeof err), PP_FETCH_FATAL);
	}
	/* https without TLS support must say so instead of silently using plaintext */
	if (!pp_tls_available()) {
		pp_ppcenter_cfg c;
		memset(&c, 0, sizeof c);
		c.url = "https://127.0.0.1:9";
		c.app_id = "a";
		c.app_secret = "s";
		c.stream_name = "b";
		c.timeout_ms = 500;
		CHECK_INT(pp_ppcenter_fetch(&c, &d, err, sizeof err), PP_FETCH_RETRY);
		CHECK(strstr(err, "TLS") != NULL);
	}
}

static void test_websocket(void)
{
	mock_t m;
	char url[96], err[160] = "", buf[1024];
	pp_ws *w;
	pp_net_opts o;
	pp_degrade_msg msg;
	int n;

	memset(&o, 0, sizeof o);
	o.timeout_ms = 2000;
	memset(g_ws_got, 0, sizeof g_ws_got);
	g_ws_pong_ok = g_ws_closed_seen = 0;
	CHECK_INT(mock_start(&m, h_ws_script), 0);
	snprintf(url, sizeof url, "ws://127.0.0.1:%d/app1/B01/h264/ws/whip", m.port);
	w = pp_ws_connect(url, "BEARER.tok-123", &o, err, sizeof err);
	CHECK(w != NULL);
	if (!w) {
		printf("  connect error: %s\n", err);
		mock_stop(&m);
		return;
	}

	/* 1. initial TARGET_STATE */
	n = pp_ws_recv(w, buf, sizeof buf, 2000);
	CHECK(n > 0);
	CHECK_INT(pp_degrade_parse(buf, (size_t)n, &msg), 0);
	CHECK_INT(msg.type, PP_MSG_TARGET_STATE);
	CHECK_INT(msg.bitrate_percent, 80);
	CHECK_INT(msg.latency_ms, 300);
	CHECK_INT(pp_ws_send_text(w, "{\"type\":\"CLIENT_INFO\",\"client\":\"ppusher\",\"version\":\"0.1.0\"}", 56), 0);

	/* 2. fragmented message reassembled into one */
	n = pp_ws_recv(w, buf, sizeof buf, 2000);
	CHECK_STR(buf, "{\"type\":\"ALERT\",\"reason\":\"x\"}");
	pp_degrade_parse(buf, (size_t)n, &msg);
	CHECK_INT(msg.type, PP_MSG_ALERT);
	CHECK_STR(msg.reason, "x");

	/* 3. ping answered transparently, next message is LOSS_SAMPLE */
	n = pp_ws_recv(w, buf, sizeof buf, 2000);
	pp_degrade_parse(buf, (size_t)n, &msg);
	CHECK_INT(msg.type, PP_MSG_LOSS_SAMPLE);
	CHECK_INT(msg.unrecoverable_delta, 12);

	/* 4. reconnect flag */
	n = pp_ws_recv(w, buf, sizeof buf, 2000);
	pp_degrade_parse(buf, (size_t)n, &msg);
	CHECK_INT(msg.type, PP_MSG_TARGET_STATE);
	CHECK_INT(msg.reconnect, 1);
	CHECK_STR(msg.reason, "degrade-latency-raise");

	/* 5. extended (16-bit) payload length */
	n = pp_ws_recv(w, buf, sizeof buf, 2000);
	CHECK_INT(n, 292);

	/* 6. server close -> error, and no more data */
	CHECK_INT(pp_ws_recv(w, buf, sizeof buf, 2000), PP_IO_ERROR);
	CHECK_INT(pp_ws_recv(w, buf, sizeof buf, 100), PP_IO_ERROR);
	pp_ws_close(w);
	mock_stop(&m);

	CHECK_STR(g_ws_bearer, "BEARER.tok-123");
	CHECK(strstr(g_req, "GET /app1/B01/h264/ws/whip HTTP/1.1\r\n") == g_req);
	CHECK(strstr(g_req, "Upgrade: websocket") != NULL);
	CHECK(strstr(g_req, "Sec-WebSocket-Version: 13") != NULL);
	CHECK(strstr(g_ws_got[0], "\"type\":\"CLIENT_INFO\"") != NULL); /* arrived masked, unmasked correctly */
	CHECK_INT(g_ws_pong_ok, 1);
	CHECK_INT(g_ws_closed_seen, 1);
}

static void test_websocket_failures(void)
{
	mock_t m;
	char url[96], err[160] = "";
	pp_ws *w;
	pp_net_opts o;

	memset(&o, 0, sizeof o);
	o.timeout_ms = 1500;

	mock_start(&m, h_ws_badaccept);
	snprintf(url, sizeof url, "ws://127.0.0.1:%d/x/ws/whip", m.port);
	w = pp_ws_connect(url, "t", &o, err, sizeof err);
	CHECK(w == NULL);
	CHECK(strstr(err, "Sec-WebSocket-Accept") != NULL);
	mock_stop(&m);

	mock_start(&m, h_ws_refuse);
	snprintf(url, sizeof url, "ws://127.0.0.1:%d/x/ws/whip", m.port);
	w = pp_ws_connect(url, "badtoken", &o, err, sizeof err);
	CHECK(w == NULL);
	CHECK(strstr(err, "401") != NULL); /* ppmmx answers 401 for a bad bearer */
	mock_stop(&m);

	CHECK(pp_ws_connect("http://127.0.0.1:1/x", "t", &o, err, sizeof err) == NULL);
	CHECK(pp_ws_connect("ws://127.0.0.1:1/x", "t", &o, err, sizeof err) == NULL);
}

int main(void)
{
	RUN(test_sha1_base64_accept);
	RUN(test_url_parse);
	RUN(test_dechunk);
	RUN(test_request_and_response_parsing);
	RUN(test_fetch_ok);
	RUN(test_fetch_errors);
	RUN(test_websocket);
	RUN(test_websocket_failures);
	TEST_DONE();
}
