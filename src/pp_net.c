#include "pp_net.h"
#include "pp_platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define PP_INVALID_SOCK INVALID_SOCKET
#define sock_close closesocket
#define sock_errno() WSAGetLastError()
#define PP_EINTR WSAEINTR
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define PP_INVALID_SOCK (-1)
#define sock_close close
#define sock_errno() errno
#define PP_EINTR EINTR
#endif

#ifdef PPUSHER_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

struct pp_conn {
	sock_t fd;
#ifdef PPUSHER_HAVE_OPENSSL
	SSL_CTX *ctx;
	SSL *ssl;
#endif
};

static void seterr(char *err, size_t n, const char *fmt, const char *a)
{
	if (err && n)
		snprintf(err, n, fmt, a ? a : "");
}

int pp_tls_available(void)
{
#ifdef PPUSHER_HAVE_OPENSSL
	return 1;
#else
	return 0;
#endif
}

static void net_init(void)
{
#ifdef _WIN32
	static int done;
	if (!done) {
		WSADATA w;
		WSAStartup(MAKEWORD(2, 2), &w);
		done = 1;
	}
#endif
}

static void set_nonblock(sock_t s, int nb)
{
#ifdef _WIN32
	u_long v = nb ? 1 : 0;
	ioctlsocket(s, FIONBIO, &v);
#else
	int fl = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

/* 1 = ready, 0 = timeout, -1 = error */
static int wait_fd(sock_t s, int for_write, int timeout_ms)
{
	fd_set fds, exc;
	struct timeval tv;
	int r;
	FD_ZERO(&fds);
	FD_SET(s, &fds);
	FD_ZERO(&exc);
	FD_SET(s, &exc);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	/* Winsock reports a refused non-blocking connect() through the except set, not the write set */
	r = select((int)s + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, for_write ? &exc : NULL, &tv);
	if (r < 0)
		return sock_errno() == PP_EINTR ? 0 : -1;
	if (for_write && FD_ISSET(s, &exc))
		return -1;
	return r > 0 ? 1 : 0;
}

static sock_t tcp_connect(const char *host, int port, int timeout_ms, char *err, size_t errsz)
{
	struct addrinfo hints, *res = NULL, *ai;
	char portstr[16];
	sock_t s = PP_INVALID_SOCK;
	int rc;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(portstr, sizeof portstr, "%d", port);
	rc = getaddrinfo(host, portstr, &hints, &res);
	if (rc != 0 || !res) {
		seterr(err, errsz, "cannot resolve %s", host);
		return PP_INVALID_SOCK;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		int w;
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == PP_INVALID_SOCK)
			continue;
		set_nonblock(s, 1);
		rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
		if (rc == 0)
			break;
#ifdef _WIN32
		if (sock_errno() != WSAEWOULDBLOCK)
#else
		if (errno != EINPROGRESS)
#endif
		{
			sock_close(s);
			s = PP_INVALID_SOCK;
			continue;
		}
		w = wait_fd(s, 1, timeout_ms);
		if (w == 1) {
			int soerr = 0;
			socklen_t sl = sizeof soerr;
			getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
			if (soerr == 0)
				break;
		}
		sock_close(s);
		s = PP_INVALID_SOCK;
	}
	freeaddrinfo(res);
	if (s == PP_INVALID_SOCK) {
		seterr(err, errsz, "cannot connect to %s", host);
		return PP_INVALID_SOCK;
	}
	set_nonblock(s, 0);
	{
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
	}
	return s;
}

static void set_io_timeouts(sock_t s, int ms)
{
#ifdef _WIN32
	DWORD t = (DWORD)ms;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
#else
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

pp_conn *pp_conn_open(const char *host, int port, const pp_net_opts *o, char *err, size_t errsz)
{
	pp_conn *c;
	int timeout = (o && o->timeout_ms > 0) ? o->timeout_ms : 8000;

	net_init();
	c = (pp_conn *)calloc(1, sizeof *c);
	if (!c)
		return NULL;
	c->fd = tcp_connect(host, port, timeout, err, errsz);
	if (c->fd == PP_INVALID_SOCK) {
		free(c);
		return NULL;
	}
	if (o && o->tls) {
#ifdef PPUSHER_HAVE_OPENSSL
		unsigned char ip[16];
		int verify = o->verify;

		OPENSSL_init_ssl(0, NULL);
		c->ctx = SSL_CTX_new(TLS_client_method());
		if (!c->ctx)
			goto tls_fail;
		SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
		if (verify) {
			SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
			if (o->ca_file && *o->ca_file) {
				if (SSL_CTX_load_verify_locations(c->ctx, o->ca_file, NULL) != 1) {
					seterr(err, errsz, "cannot load CA file %s", o->ca_file);
					goto fail;
				}
			} else if (SSL_CTX_set_default_verify_paths(c->ctx) != 1) {
				seterr(err, errsz, "no default CA paths (%s); set ca_file or tls_verify=0", "");
				goto fail;
			}
		} else {
			SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, NULL);
		}
		c->ssl = SSL_new(c->ctx);
		if (!c->ssl)
			goto tls_fail;
		SSL_set_fd(c->ssl, (int)c->fd);
		if (inet_pton(AF_INET, host, ip) == 1 || inet_pton(AF_INET6, host, ip) == 1) {
			if (verify)
				X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(c->ssl), host);
		} else {
			SSL_set_tlsext_host_name(c->ssl, host);
			if (verify)
				SSL_set1_host(c->ssl, host);
		}
		set_io_timeouts(c->fd, timeout);
		if (SSL_connect(c->ssl) != 1) {
			char buf[160];
			unsigned long e = ERR_peek_last_error();
			long vr = SSL_get_verify_result(c->ssl);
			if (vr != X509_V_OK)
				snprintf(buf, sizeof buf, "TLS certificate check failed: %s", X509_verify_cert_error_string(vr));
			else if (e)
				ERR_error_string_n(e, buf, sizeof buf);
			else
				snprintf(buf, sizeof buf, "TLS handshake failed");
			seterr(err, errsz, "%s", buf);
			goto fail;
		}
		set_io_timeouts(c->fd, 0);
		return c;
	tls_fail:
		seterr(err, errsz, "TLS setup failed%s", "");
	fail:
		pp_conn_close(c);
		return NULL;
#else
		seterr(err, errsz, "this build has no TLS support (rebuild with PPUSHER_TLS=openssl)%s", "");
		sock_close(c->fd);
		free(c);
		return NULL;
#endif
	}
	return c;
}

int pp_conn_read(pp_conn *c, void *buf, size_t n, int timeout_ms)
{
	int w;
#ifdef PPUSHER_HAVE_OPENSSL
	if (c->ssl) {
		for (;;) {
			int r, e;
			if (SSL_pending(c->ssl) == 0) {
				w = wait_fd(c->fd, 0, timeout_ms);
				if (w == 0)
					return PP_IO_TIMEOUT;
				if (w < 0)
					return PP_IO_ERROR;
			}
			r = SSL_read(c->ssl, buf, (int)n);
			if (r > 0)
				return r;
			e = SSL_get_error(c->ssl, r);
			if (e == SSL_ERROR_ZERO_RETURN)
				return PP_IO_EOF;
			if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
				continue; /* a TLS record boundary: loop back through select() */
			return PP_IO_ERROR;
		}
	}
#endif
	w = wait_fd(c->fd, 0, timeout_ms);
	if (w == 0)
		return PP_IO_TIMEOUT;
	if (w < 0)
		return PP_IO_ERROR;
	{
		int r = recv(c->fd, (char *)buf, (int)n, 0);
		if (r > 0)
			return r;
		if (r == 0)
			return PP_IO_EOF;
		return PP_IO_ERROR;
	}
}

int pp_conn_write_all(pp_conn *c, const void *buf, size_t n, int timeout_ms)
{
	const char *p = (const char *)buf;
	int64_t deadline = pp_now_ms() + timeout_ms;

	while (n > 0) {
		int r;
		int64_t left = deadline - pp_now_ms();
		if (left <= 0)
			return -1;
		if (wait_fd(c->fd, 1, (int)left) != 1)
			return -1;
#ifdef PPUSHER_HAVE_OPENSSL
		if (c->ssl) {
			r = SSL_write(c->ssl, p, (int)(n > 16384 ? 16384 : n));
			if (r <= 0) {
				int e = SSL_get_error(c->ssl, r);
				if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE)
					continue;
				return -1;
			}
		} else
#endif
		{
			r = send(c->fd, p, (int)n, 0);
			if (r <= 0)
				return -1;
		}
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

void pp_conn_close(pp_conn *c)
{
	if (!c)
		return;
#ifdef PPUSHER_HAVE_OPENSSL
	if (c->ssl) {
		SSL_shutdown(c->ssl);
		SSL_free(c->ssl);
	}
	if (c->ctx)
		SSL_CTX_free(c->ctx);
#endif
	if (c->fd != PP_INVALID_SOCK)
		sock_close(c->fd);
	free(c);
}

int pp_url_parse(const char *url, pp_url *u)
{
	const char *p = url, *hs, *he, *path;
	size_t n;

	memset(u, 0, sizeof *u);
	if (!url)
		return -1;
	if (strncmp(p, "https://", 8) == 0 || strncmp(p, "wss://", 6) == 0) {
		u->tls = 1;
		u->port = 443;
		p += (p[0] == 'h') ? 8 : 6;
	} else if (strncmp(p, "http://", 7) == 0 || strncmp(p, "ws://", 5) == 0) {
		u->port = 80;
		p += (p[0] == 'h') ? 7 : 5;
	} else {
		return -1;
	}
	hs = p;
	if (*hs == '[') {
		hs++;
		he = strchr(hs, ']');
		if (!he)
			return -1;
		path = he + 1;
	} else {
		he = hs;
		while (*he && *he != ':' && *he != '/' && *he != '?' && *he != '#')
			he++;
		path = he;
	}
	n = (size_t)(he - hs);
	if (n == 0 || n >= sizeof u->host)
		return -1;
	memcpy(u->host, hs, n);
	if (*path == ':') {
		char *end;
		long v = strtol(path + 1, &end, 10);
		if (end == path + 1 || v < 1 || v > 65535)
			return -1;
		u->port = (int)v;
		path = end;
	}
	if (*path == 0) {
		strcpy(u->path, "/");
	} else if (*path == '/' || *path == '?') {
		size_t pl = strlen(path);
		if (pl + (*path == '?') >= sizeof u->path)
			return -1;
		if (*path == '?') {
			u->path[0] = '/';
			memcpy(u->path + 1, path, pl + 1);
		} else {
			memcpy(u->path, path, pl + 1);
		}
	} else {
		return -1;
	}
	return 0;
}
