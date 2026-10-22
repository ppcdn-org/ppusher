#ifndef PP_PPCENTER_H
#define PP_PPCENTER_H

#include "pp_net.h"

#include <stddef.h>
#include <stdint.h>

#define PP_TOKEN_MAX 1024
#define PP_WSURL_MAX 512

/* What POST /v1/publish/requests (protocol "srt") hands back. Index 1 = h264, 2 = hevc (PP_CODEC_*). */
typedef struct {
	int64_t origin_node_id;
	char endpoint[256]; /* ppcdnPublishEndpoint: host[:port] */
	char token[PP_TOKEN_MAX];                 /* publishToken: codec-less path */
	char token_codec[3][PP_TOKEN_MAX];        /* publishTokens.{h264,hevc} */
	char ws_url[3][PP_WSURL_MAX];             /* degrade control URL derived from whipTracks[codec].url */
	char ws_token[3][PP_TOKEN_MAX];           /* whipTracks[codec].bearerToken */
} pp_publish_decision;

typedef struct {
	const char *url; /* "https://api.pp-cdn.org" or the full /v1/publish/requests URL */
	const char *app_id, *app_secret, *stream_name, *region;
	int want_degrade; /* send capabilities:["whip-hevc-h264"] */
	int tls_verify;
	const char *ca_file;
	int timeout_ms;
} pp_ppcenter_cfg;

/* Request body (JSON). Returns length or -1 if `cap` is too small. */
int pp_ppcenter_build_request(char *out, size_t cap, const char *app_id, const char *app_secret,
			      const char *stream_name, const char *region, int want_degrade);

/* Fills *d from a 200 response body. 0 = ok, -1 = malformed or no ppcdnPublishEndpoint. */
int pp_ppcenter_parse_response(const char *json, size_t n, pp_publish_decision *d);

/* The token to put in the streamid for `codec` (PP_CODEC_*): codec-less token for PP_CODEC_NONE. May be "". */
const char *pp_decision_token(const pp_publish_decision *d, int codec);

/* What the session should do after a fetch attempt. */
#define PP_FETCH_OK 0
#define PP_FETCH_RETRY 1 /* transport error, 429, 5xx (incl. 503 no_healthy_origin) */
#define PP_FETCH_SLOW 2  /* 402 account_in_arrears */
#define PP_FETCH_FATAL 3 /* 400 / 401 / 403 / other 4xx: credentials or request are wrong */
int pp_fetch_class(int http_status);

/* Full round trip. Returns a PP_FETCH_* class; err is filled on anything but OK. */
int pp_ppcenter_fetch(const pp_ppcenter_cfg *cfg, pp_publish_decision *d, char *err, size_t errsz);

#endif
