#include "pp_ws.h"
#include "pp_platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_MAX_FRAME 65536
#define WS_PING_INTERVAL_MS 20000
#define WS_DEAD_MS 60000

/* ------------------------- SHA-1 and base64 (handshake only) ------------------------- */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

void pp_sha1(const uint8_t *data, size_t n, uint8_t out[20])
{
	uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
	uint8_t blk[64];
	size_t i, full = n / 64, rem = n % 64;
	uint64_t bits = (uint64_t)n * 8;
	size_t b;
	int pass;

	for (pass = 0; pass < 2; pass++) {
		size_t nblocks = pass == 0 ? full : (rem < 56 ? 1 : 2);
		for (b = 0; b < nblocks; b++) {
			uint32_t w[80], a, bb, c, d, e, t;
			int k;
			if (pass == 0) {
				memcpy(blk, data + b * 64, 64);
			} else {
				memset(blk, 0, 64);
				if (b == 0) {
					memcpy(blk, data + full * 64, rem);
					blk[rem] = 0x80;
				}
				if (b == nblocks - 1) {
					for (k = 0; k < 8; k++)
						blk[56 + k] = (uint8_t)(bits >> (56 - 8 * k));
				}
			}
			for (k = 0; k < 16; k++)
				w[k] = ((uint32_t)blk[4 * k] << 24) | ((uint32_t)blk[4 * k + 1] << 16) |
				       ((uint32_t)blk[4 * k + 2] << 8) | blk[4 * k + 3];
			for (k = 16; k < 80; k++)
				w[k] = ROL(w[k - 3] ^ w[k - 8] ^ w[k - 14] ^ w[k - 16], 1);
			a = h[0];
			bb = h[1];
			c = h[2];
			d = h[3];
			e = h[4];
			for (k = 0; k < 80; k++) {
				uint32_t f, kk;
				if (k < 20) {
					f = (bb & c) | (~bb & d);
					kk = 0x5A827999u;
				} else if (k < 40) {
					f = bb ^ c ^ d;
					kk = 0x6ED9EBA1u;
				} else if (k < 60) {
					f = (bb & c) | (bb & d) | (c & d);
					kk = 0x8F1BBCDCu;
				} else {
					f = bb ^ c ^ d;
					kk = 0xCA62C1D6u;
				}
				t = ROL(a, 5) + f + e + kk + w[k];
				e = d;
				d = c;
				c = ROL(bb, 30);
				bb = a;
				a = t;
			}
			h[0] += a;
			h[1] += bb;
			h[2] += c;
			h[3] += d;
			h[4] += e;
		}
	}
	for (i = 0; i < 5; i++) {
		out[4 * i] = (uint8_t)(h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(h[i] >> 8);
		out[4 * i + 3] = (uint8_t)h[i];
	}
}

int pp_base64(const uint8_t *in, size_t n, char *out, size_t outsz)
{
	static const char tb[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i, o = 0;
	if (outsz < ((n + 2) / 3) * 4 + 1)
		return -1;
	for (i = 0; i + 2 < n; i += 3) {
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
		out[o++] = tb[v >> 18];
		out[o++] = tb[(v >> 12) & 63];
		out[o++] = tb[(v >> 6) & 63];
		out[o++] = tb[v & 63];
	}
	if (n - i == 1) {
		uint32_t v = (uint32_t)in[i] << 16;
		out[o++] = tb[v >> 18];
		out[o++] = tb[(v >> 12) & 63];
		out[o++] = '=';
		out[o++] = '=';
	} else if (n - i == 2) {
		uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
		out[o++] = tb[v >> 18];
		out[o++] = tb[(v >> 12) & 63];
		out[o++] = tb[(v >> 6) & 63];
		out[o++] = '=';
	}
	out[o] = 0;
	return (int)o;
}

int pp_ws_accept_key(const char *client_key, char *out, size_t outsz)
{
	char tmp[128];
	uint8_t d[20];
	int n = snprintf(tmp, sizeof tmp, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", client_key);
	if (n < 0 || (size_t)n >= sizeof tmp)
		return -1;
	pp_sha1((const uint8_t *)tmp, (size_t)n, d);
	return pp_base64(d, 20, out, outsz) < 0 ? -1 : 0;
}

/* ------------------------- client ------------------------- */

struct pp_ws {
	pp_conn *c;
	uint8_t rbuf[WS_MAX_FRAME + 16];
	size_t rlen;
	int64_t last_rx_ms, last_ping_ms;
	int closed;
	/* partially assembled message */
	uint8_t *msg;
	size_t msg_n, msg_cap;
};

static void werr(char *err, size_t n, const char *m)
{
	if (err && n)
		snprintf(err, n, "%s", m);
}

static int send_frame(pp_ws *w, int opcode, const uint8_t *p, size_t n)
{
	uint8_t hdr[14], mask[4];
	size_t hl = 0, i;
	uint8_t *buf;
	int rc;

	hdr[hl++] = (uint8_t)(0x80 | opcode);
	if (n < 126) {
		hdr[hl++] = (uint8_t)(0x80 | n);
	} else if (n < 65536) {
		hdr[hl++] = 0x80 | 126;
		hdr[hl++] = (uint8_t)(n >> 8);
		hdr[hl++] = (uint8_t)n;
	} else {
		return -1;
	}
	pp_rand_bytes(mask, 4);
	memcpy(hdr + hl, mask, 4);
	hl += 4;
	buf = (uint8_t *)malloc(hl + n);
	if (!buf)
		return -1;
	memcpy(buf, hdr, hl);
	for (i = 0; i < n; i++)
		buf[hl + i] = (uint8_t)(p[i] ^ mask[i & 3]);
	rc = pp_conn_write_all(w->c, buf, hl + n, 5000);
	free(buf);
	return rc;
}

pp_ws *pp_ws_connect(const char *url, const char *bearer, const pp_net_opts *opts, char *err, size_t errsz)
{
	pp_url u;
	pp_net_opts o;
	pp_ws *w;
	uint8_t kraw[16];
	char key[32], accept[40], req[1536], hostport[300];
	int n;
	size_t got = 0, hdr_end = 0;
	char hdr[4096];
	int64_t deadline;

	if (pp_url_parse(url, &u) != 0 || strncmp(url, "ws", 2) != 0) {
		werr(err, errsz, "invalid WebSocket URL");
		return NULL;
	}
	o = opts ? *opts : (pp_net_opts){0};
	o.tls = u.tls;
	if (o.timeout_ms <= 0)
		o.timeout_ms = 8000;

	w = (pp_ws *)calloc(1, sizeof *w);
	if (!w)
		return NULL;
	w->c = pp_conn_open(u.host, u.port, &o, err, errsz);
	if (!w->c) {
		free(w);
		return NULL;
	}

	pp_rand_bytes(kraw, sizeof kraw);
	pp_base64(kraw, sizeof kraw, key, sizeof key);
	if ((u.tls && u.port == 443) || (!u.tls && u.port == 80))
		snprintf(hostport, sizeof hostport, "%s", u.host);
	else
		snprintf(hostport, sizeof hostport, "%s:%d", u.host, u.port);
	n = snprintf(req, sizeof req,
		     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: ppusher/0.1\r\n%s%s%s",
		     u.path, hostport, key, bearer && *bearer ? "Authorization: Bearer " : "", bearer ? bearer : "",
		     bearer && *bearer ? "\r\n\r\n" : "\r\n");
	if (n < 0 || (size_t)n >= sizeof req) {
		werr(err, errsz, "WebSocket request too large");
		goto fail;
	}
	if (pp_conn_write_all(w->c, req, (size_t)n, 5000) != 0) {
		werr(err, errsz, "WebSocket handshake send failed");
		goto fail;
	}

	deadline = pp_now_ms() + o.timeout_ms;
	while (!hdr_end) {
		int64_t left = deadline - pp_now_ms();
		int r;
		size_t i;
		if (left <= 0 || got >= sizeof hdr - 1) {
			werr(err, errsz, "WebSocket handshake timed out");
			goto fail;
		}
		r = pp_conn_read(w->c, hdr + got, sizeof hdr - 1 - got, (int)left);
		if (r <= 0) {
			werr(err, errsz, r == PP_IO_TIMEOUT ? "WebSocket handshake timed out" : "WebSocket handshake failed");
			goto fail;
		}
		got += (size_t)r;
		for (i = 3; i < got; i++)
			if (hdr[i - 3] == '\r' && hdr[i - 2] == '\n' && hdr[i - 1] == '\r' && hdr[i] == '\n') {
				hdr_end = i + 1;
				break;
			}
	}
	hdr[got] = 0;
	if (strncmp(hdr, "HTTP/1.1 101", 12) != 0) {
		char m[96];
		int st = (got > 12 && strncmp(hdr, "HTTP/1.", 7) == 0) ? atoi(hdr + 9) : 0;
		snprintf(m, sizeof m, "WebSocket upgrade refused (HTTP %d)", st);
		werr(err, errsz, m);
		goto fail;
	}
	pp_ws_accept_key(key, accept, sizeof accept);
	{
		/* header names are case-insensitive; the value must be our key's accept hash */
		const char *p = hdr, *end = hdr + hdr_end;
		int ok = 0;
		while (p < end) {
			const char *eol = memchr(p, 0x0A, (size_t)(end - p));
			static const char name[] = "sec-websocket-accept:";
			size_t k;
			int match = 1;
			if (!eol)
				break;
			for (k = 0; k < sizeof name - 1; k++)
				if (tolower((unsigned char)p[k]) != name[k]) {
					match = 0;
					break;
				}
			if (match) {
				const char *v = p + sizeof name - 1;
				size_t al = strlen(accept);
				while (v < eol && (*v == ' ' || *v == 0x09))
					v++;
				if ((size_t)(eol - v) >= al && memcmp(v, accept, al) == 0)
					ok = 1;
			}
			p = eol + 1;
		}
		if (!ok) {
			werr(err, errsz, "WebSocket handshake: bad Sec-WebSocket-Accept");
			goto fail;
		}
	}
	/* bytes after the headers already belong to the first frames */
	if (got > hdr_end) {
		w->rlen = got - hdr_end;
		memcpy(w->rbuf, hdr + hdr_end, w->rlen);
	}
	w->last_rx_ms = w->last_ping_ms = pp_now_ms();
	return w;
fail:
	pp_conn_close(w->c);
	free(w);
	return NULL;
}

int pp_ws_send_text(pp_ws *w, const char *msg, size_t n)
{
	if (w->closed)
		return -1;
	return send_frame(w, 0x1, (const uint8_t *)msg, n);
}

static int msg_append(pp_ws *w, const uint8_t *p, size_t n)
{
	if (w->msg_n + n > WS_MAX_FRAME)
		return -1;
	if (w->msg_n + n + 1 > w->msg_cap) {
		size_t cap = w->msg_cap ? w->msg_cap : 1024;
		uint8_t *m;
		while (cap < w->msg_n + n + 1)
			cap *= 2;
		m = (uint8_t *)realloc(w->msg, cap);
		if (!m)
			return -1;
		w->msg = m;
		w->msg_cap = cap;
	}
	memcpy(w->msg + w->msg_n, p, n);
	w->msg_n += n;
	return 0;
}

/* Consume one frame from rbuf if complete. Returns 1 = a full text message is ready, 0 = need more
 * data / handled a control frame, -1 = connection unusable. */
static int parse_frame(pp_ws *w)
{
	const uint8_t *p = w->rbuf;
	size_t need, hl = 2, plen;
	int fin, op, masked;
	uint8_t mask[4] = {0, 0, 0, 0};
	size_t i;

	if (w->rlen < 2)
		return 0;
	fin = (p[0] & 0x80) != 0;
	op = p[0] & 0x0F;
	masked = (p[1] & 0x80) != 0;
	if (p[0] & 0x70)
		return -1; /* reserved bits: no extension was negotiated */
	plen = p[1] & 0x7F;
	if (plen == 126) {
		if (w->rlen < 4)
			return 0;
		plen = ((size_t)p[2] << 8) | p[3];
		hl = 4;
	} else if (plen == 127) {
		if (w->rlen < 10)
			return 0;
		if (p[2] || p[3] || p[4] || p[5])
			return -1; /* > 4 GB; anything over WS_MAX_FRAME is refused below */
		plen = ((size_t)p[6] << 24) | ((size_t)p[7] << 16) | ((size_t)p[8] << 8) | p[9];
		hl = 10;
	}
	if (plen > WS_MAX_FRAME)
		return -1;
	if (masked) {
		if (w->rlen < hl + 4)
			return 0;
		memcpy(mask, p + hl, 4);
		hl += 4;
	}
	need = hl + plen;
	if (w->rlen < need)
		return 0;

	if (masked)
		for (i = 0; i < plen; i++)
			w->rbuf[hl + i] ^= mask[i & 3];

	if (op >= 0x8) { /* control frames: unfragmented, <= 125 bytes */
		if (!fin || plen > 125)
			return -1;
		if (op == 0x8) {
			send_frame(w, 0x8, w->rbuf + hl, plen >= 2 ? 2 : 0);
			w->closed = 1;
			return -1;
		}
		if (op == 0x9)
			send_frame(w, 0xA, w->rbuf + hl, plen);
	} else if (op == 0x1 || op == 0x2 || op == 0x0) {
		if (op != 0x0)
			w->msg_n = 0;
		else if (w->msg_n == 0)
			return -1; /* continuation with nothing to continue */
		if (msg_append(w, w->rbuf + hl, plen) != 0)
			return -1;
		if (fin) {
			memmove(w->rbuf, w->rbuf + need, w->rlen - need);
			w->rlen -= need;
			return 1;
		}
	} else {
		return -1;
	}
	memmove(w->rbuf, w->rbuf + need, w->rlen - need);
	w->rlen -= need;
	return 0;
}

int pp_ws_recv(pp_ws *w, char *buf, size_t cap, int timeout_ms)
{
	int64_t deadline = pp_now_ms() + timeout_ms;

	if (w->closed)
		return PP_IO_ERROR;
	for (;;) {
		int64_t now = pp_now_ms(), left;
		int r;

		/* drain whatever is already buffered before touching the socket */
		for (;;) {
			size_t before = w->rlen;
			r = parse_frame(w);
			if (r < 0) {
				w->closed = 1;
				return PP_IO_ERROR;
			}
			if (r == 1) {
				size_t n = w->msg_n < cap - 1 ? w->msg_n : cap - 1;
				memcpy(buf, w->msg, n);
				buf[n] = 0;
				w->msg_n = 0;
				return (int)n;
			}
			if (w->rlen == before)
				break; /* partial frame: need more bytes */
		}

		if (now - w->last_rx_ms > WS_DEAD_MS) {
			w->closed = 1;
			return PP_IO_ERROR;
		}
		if (now - w->last_ping_ms >= WS_PING_INTERVAL_MS) {
			if (send_frame(w, 0x9, (const uint8_t *)"pp", 2) != 0) {
				w->closed = 1;
				return PP_IO_ERROR;
			}
			w->last_ping_ms = now;
		}
		left = deadline - now;
		if (left <= 0)
			return PP_IO_TIMEOUT;
		if (w->rlen >= sizeof w->rbuf)
			return PP_IO_ERROR;
		r = pp_conn_read(w->c, w->rbuf + w->rlen, sizeof w->rbuf - w->rlen, (int)(left > 1000 ? 1000 : left));
		if (r == PP_IO_TIMEOUT)
			continue;
		if (r <= 0) {
			w->closed = 1;
			return PP_IO_ERROR;
		}
		w->rlen += (size_t)r;
		w->last_rx_ms = pp_now_ms();
	}
}

void pp_ws_close(pp_ws *w)
{
	if (!w)
		return;
	if (!w->closed && w->c) {
		uint8_t code[2] = {0x03, 0xE8}; /* 1000 normal closure */
		send_frame(w, 0x8, code, 2);
	}
	pp_conn_close(w->c);
	free(w->msg);
	free(w);
}
