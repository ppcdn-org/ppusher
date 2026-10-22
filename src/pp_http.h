#ifndef PP_HTTP_H
#define PP_HTTP_H

#include "pp_net.h"

/*
 * One HTTP/1.1 POST (Connection: close). Handles Content-Length, chunked and
 * read-until-close bodies. No redirects, no proxies: ppcenter's endpoint is a
 * fixed URL, so a 3xx is reported as an error instead of followed.
 *
 * Returns 0 when an HTTP response was received (status in *status, body in
 * resp, NUL-terminated), -1 on transport / protocol failure (message in err).
 */
int pp_http_post(const char *url, const char *content_type, const char *body, size_t body_len,
		 const char *extra_headers, const pp_net_opts *opts, int total_timeout_ms, int *status, char *resp,
		 size_t resp_cap, size_t *resp_len, char *err, size_t errsz);

/* exposed for tests: decode a chunked body. Returns decoded length, -1 on malformed, -2 if incomplete. */
int pp_http_dechunk(const char *in, size_t n, char *out, size_t outcap);

#endif
