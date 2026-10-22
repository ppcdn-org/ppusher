#ifndef PP_NET_H
#define PP_NET_H

#include <stddef.h>
#include <stdint.h>

/*
 * Blocking TCP (+ optional TLS) with per-call timeouts. Just enough for one
 * HTTPS POST to ppcenter and one WebSocket to ppmmx.
 */

typedef struct pp_conn pp_conn;

typedef struct {
	int tls;            /* 1 = wrap in TLS */
	int verify;         /* verify the server certificate (default: yes) */
	const char *ca_file; /* PEM bundle; NULL = the TLS library's default paths */
	int timeout_ms;     /* connect / handshake timeout */
} pp_net_opts;

#define PP_IO_TIMEOUT (-2)
#define PP_IO_ERROR (-1)
#define PP_IO_EOF 0

/* 1 if this build can speak TLS (PPUSHER_TLS=openssl). */
int pp_tls_available(void);

pp_conn *pp_conn_open(const char *host, int port, const pp_net_opts *o, char *err, size_t errsz);
/* >0: bytes read; 0: orderly EOF; PP_IO_TIMEOUT; PP_IO_ERROR */
int pp_conn_read(pp_conn *c, void *buf, size_t n, int timeout_ms);
/* 0 on success, -1 on error/timeout */
int pp_conn_write_all(pp_conn *c, const void *buf, size_t n, int timeout_ms);
void pp_conn_close(pp_conn *c);

/* "http(s)://host[:port]/path" and "ws(s)://..." */
typedef struct {
	int tls;
	char host[256];
	int port;
	char path[512]; /* always starts with '/' */
} pp_url;

int pp_url_parse(const char *url, pp_url *u);

#endif
