#ifndef PP_WS_H
#define PP_WS_H

#include "pp_net.h"

/*
 * RFC 6455 client, text messages only (the degrade channel is JSON text):
 * handshake with Authorization: Bearer, masked client frames, ping/pong,
 * close, fragmented messages, keep-alive pings.
 */

typedef struct pp_ws pp_ws;

pp_ws *pp_ws_connect(const char *url, const char *bearer_token, const pp_net_opts *opts, char *err, size_t errsz);
int pp_ws_send_text(pp_ws *w, const char *msg, size_t n);
/*
 * Wait up to timeout_ms for one complete message. Returns its length (>=0,
 * NUL-terminated in buf), PP_IO_TIMEOUT if none arrived, PP_IO_ERROR if the
 * connection is closed / dead / violated the protocol. Sends keep-alive
 * pings (every 20 s) and answers server pings itself.
 */
int pp_ws_recv(pp_ws *w, char *buf, size_t cap, int timeout_ms);
void pp_ws_close(pp_ws *w);

/* exposed for tests */
void pp_sha1(const uint8_t *data, size_t n, uint8_t out[20]);
int pp_base64(const uint8_t *in, size_t n, char *out, size_t outsz);
int pp_ws_accept_key(const char *client_key, char *out, size_t outsz);

#endif
