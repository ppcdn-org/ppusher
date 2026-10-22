#include "pp_http.h"
#include "pp_platform.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HEADERS 16384

static int ci_prefix(const char *s, const char *prefix)
{
	for (; *prefix; s++, prefix++)
		if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
			return 0;
	return 1;
}

int pp_http_dechunk(const char *in, size_t n, char *out, size_t outcap)
{
	size_t pos = 0, o = 0;

	for (;;) {
		size_t sz = 0, i = pos;
		int digits = 0;
		while (i < n && isxdigit((unsigned char)in[i])) {
			int c = in[i];
			sz = sz * 16 + (size_t)(c <= '9' ? c - '0' : (tolower(c) - 'a' + 10));
			if (sz > (size_t)1 << 28)
				return -1;
			i++;
			digits++;
		}
		if (i >= n)
			return -2;
		if (!digits)
			return -1;
		while (i < n && in[i] != '\n') /* skip chunk extensions up to CRLF */
			i++;
		if (i >= n)
			return -2;
		i++;
		if (sz == 0)
			return (int)o; /* trailers (if any) are ignored */
		if (i + sz + 2 > n)
			return -2;
		if (o + sz >= outcap)
			return -1;
		memcpy(out + o, in + i, sz);
		o += sz;
		i += sz;
		if (in[i] != '\r' || in[i + 1] != '\n')
			return -1;
		pos = i + 2;
	}
}

static void fail(char *err, size_t errsz, const char *m)
{
	if (err && errsz)
		snprintf(err, errsz, "%s", m);
}

int pp_http_post(const char *url, const char *content_type, const char *body, size_t body_len,
		 const char *extra_headers, const pp_net_opts *opts, int total_timeout_ms, int *status, char *resp,
		 size_t resp_cap, size_t *resp_len, char *err, size_t errsz)
{
	pp_url u;
	pp_net_opts o;
	pp_conn *c;
	char *req = NULL, *raw = NULL;
	size_t raw_cap = resp_cap + MAX_HEADERS + 64, raw_n = 0, hdr_end = 0, req_n;
	int64_t deadline = pp_now_ms() + total_timeout_ms;
	long content_length = -1;
	int chunked = 0, st = 0, rc = -1;
	char hostport[300];

	if (pp_url_parse(url, &u) != 0 || (u.path[0] != '/')) {
		fail(err, errsz, "invalid URL");
		return -1;
	}
	if (strncmp(url, "http", 4) != 0) {
		fail(err, errsz, "not an HTTP URL");
		return -1;
	}
	o = opts ? *opts : (pp_net_opts){0};
	o.tls = u.tls;
	if (o.timeout_ms <= 0)
		o.timeout_ms = total_timeout_ms;

	c = pp_conn_open(u.host, u.port, &o, err, errsz);
	if (!c)
		return -1;

	if ((u.tls && u.port == 443) || (!u.tls && u.port == 80))
		snprintf(hostport, sizeof hostport, "%s", u.host);
	else
		snprintf(hostport, sizeof hostport, "%s:%d", u.host, u.port);

	req = (char *)malloc(body_len + 1024 + (extra_headers ? strlen(extra_headers) : 0));
	raw = (char *)malloc(raw_cap);
	if (!req || !raw) {
		fail(err, errsz, "out of memory");
		goto done;
	}
	req_n = (size_t)sprintf(req,
				"POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ppusher/0.1\r\nAccept: application/json\r\n"
				"Content-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n%s\r\n",
				u.path, hostport, content_type ? content_type : "application/json", (unsigned long)body_len,
				extra_headers ? extra_headers : "");
	memcpy(req + req_n, body, body_len);
	if (pp_conn_write_all(c, req, req_n + body_len, total_timeout_ms) != 0) {
		fail(err, errsz, "send failed");
		goto done;
	}

	for (;;) {
		int64_t left = deadline - pp_now_ms();
		int r;

		if (hdr_end) {
			size_t have = raw_n - hdr_end;
			if (content_length >= 0 && have >= (size_t)content_length)
				break;
			if (chunked) {
				char *tmp = (char *)malloc(have + 1);
				int d;
				if (tmp) {
					d = pp_http_dechunk(raw + hdr_end, have, tmp, have + 1);
					free(tmp);
					if (d >= 0 || d == -1)
						break;
				}
			}
		}
		if (left <= 0) {
			fail(err, errsz, "timed out waiting for the response");
			goto done;
		}
		if (raw_n >= raw_cap) {
			fail(err, errsz, "response too large");
			goto done;
		}
		r = pp_conn_read(c, raw + raw_n, raw_cap - raw_n, (int)left);
		if (r == PP_IO_TIMEOUT) {
			fail(err, errsz, "timed out waiting for the response");
			goto done;
		}
		if (r == PP_IO_EOF)
			break;
		if (r < 0) {
			fail(err, errsz, "receive failed");
			goto done;
		}
		raw_n += (size_t)r;

		if (!hdr_end) {
			size_t i;
			for (i = 3; i < raw_n && i < MAX_HEADERS; i++) {
				if (raw[i - 3] == '\r' && raw[i - 2] == '\n' && raw[i - 1] == '\r' && raw[i] == '\n') {
					hdr_end = i + 1;
					break;
				}
			}
			if (!hdr_end && raw_n >= MAX_HEADERS) {
				fail(err, errsz, "response headers too large");
				goto done;
			}
			if (hdr_end) {
				const char *p = raw, *e = raw + hdr_end;
				if (hdr_end < 12 || strncmp(raw, "HTTP/1.", 7) != 0) {
					fail(err, errsz, "not an HTTP response");
					goto done;
				}
				st = atoi(raw + 9);
				while (p < e && *p != '\n')
					p++;
				p++;
				while (p < e) {
					if (ci_prefix(p, "content-length:"))
						content_length = atol(p + 15);
					else if (ci_prefix(p, "transfer-encoding:")) {
						const char *q = p + 18;
						const char *le = memchr(p, '\n', (size_t)(e - p));
						while (q < (le ? le : e)) {
							if (ci_prefix(q, "chunked")) {
								chunked = 1;
								break;
							}
							q++;
						}
					}
					while (p < e && *p != '\n')
						p++;
					p++;
				}
			}
		}
	}

	if (!hdr_end) {
		fail(err, errsz, "connection closed before a response was received");
		goto done;
	}
	if (st >= 300 && st < 400) {
		fail(err, errsz, "server answered with a redirect (not followed)");
		*status = st;
		if (resp_cap)
			resp[0] = 0;
		if (resp_len)
			*resp_len = 0;
		rc = 0;
		goto done;
	}
	{
		size_t have = raw_n - hdr_end;
		int blen;
		if (chunked) {
			blen = pp_http_dechunk(raw + hdr_end, have, resp, resp_cap);
			if (blen < 0) {
				fail(err, errsz, "malformed or oversized chunked body");
				goto done;
			}
		} else {
			size_t take = content_length >= 0 && (size_t)content_length < have ? (size_t)content_length : have;
			if (take >= resp_cap) {
				fail(err, errsz, "response body too large");
				goto done;
			}
			memcpy(resp, raw + hdr_end, take);
			blen = (int)take;
			if (content_length >= 0 && have < (size_t)content_length) {
				fail(err, errsz, "response truncated");
				goto done;
			}
		}
		resp[blen] = 0;
		if (resp_len)
			*resp_len = (size_t)blen;
		*status = st;
		rc = 0;
	}
done:
	free(req);
	free(raw);
	pp_conn_close(c);
	return rc;
}
