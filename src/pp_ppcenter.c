#include "pp_ppcenter.h"
#include "pp_degrade.h"
#include "pp_http.h"
#include "pp_json.h"
#include "pp_streamid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESP_MAX (64 * 1024)

int pp_ppcenter_build_request(char *out, size_t cap, const char *app_id, const char *app_secret,
			      const char *stream_name, const char *region, int want_degrade)
{
	char a[256], s[512], n[256], r[256];
	int len;

	if (pp_json_escape(app_id, a, sizeof a) < 0 || pp_json_escape(app_secret, s, sizeof s) < 0 ||
	    pp_json_escape(stream_name, n, sizeof n) < 0 || pp_json_escape(region ? region : "", r, sizeof r) < 0)
		return -1;
	len = snprintf(out, cap, "{\"appId\":\"%s\",\"appSecret\":\"%s\",\"streamName\":\"%s\",\"protocol\":\"srt\"%s%s%s%s}",
		       a, s, n, want_degrade ? ",\"capabilities\":[\"whip-hevc-h264\"]" : "",
		       region && *region ? ",\"requestRegion\":\"" : "", region && *region ? r : "",
		       region && *region ? "\"" : "");
	return (len < 0 || (size_t)len >= cap) ? -1 : len;
}

int pp_ppcenter_parse_response(const char *json, size_t n, pp_publish_decision *d)
{
	const char *sub, *t;
	size_t sn, tn;
	int64_t iv;
	static const char *const names[3] = {"", "h264", "hevc"};
	int c;

	memset(d, 0, sizeof *d);
	if (!pp_json_str(json, n, "ppcdnPublishEndpoint", d->endpoint, sizeof d->endpoint) || !d->endpoint[0])
		return -1;
	if (pp_json_int(json, n, "originNodeId", &iv))
		d->origin_node_id = iv;
	pp_json_str(json, n, "publishToken", d->token, sizeof d->token);
	if (pp_json_obj(json, n, "publishTokens", &sub, &sn))
		for (c = 1; c <= 2; c++)
			pp_json_str(sub, sn, names[c], d->token_codec[c], sizeof d->token_codec[c]);
	if (pp_json_obj(json, n, "whipTracks", &sub, &sn)) {
		for (c = 1; c <= 2; c++) {
			char url[PP_WSURL_MAX];
			if (!pp_json_obj(sub, sn, names[c], &t, &tn))
				continue;
			if (pp_json_str(t, tn, "url", url, sizeof url) &&
			    pp_degrade_ws_url(url, d->ws_url[c], sizeof d->ws_url[c]) == 0)
				pp_json_str(t, tn, "bearerToken", d->ws_token[c], sizeof d->ws_token[c]);
			else
				d->ws_url[c][0] = 0;
		}
	}
	return 0;
}

const char *pp_decision_token(const pp_publish_decision *d, int codec)
{
	if (codec == PP_CODEC_H264 || codec == PP_CODEC_HEVC)
		return d->token_codec[codec];
	return d->token;
}

int pp_fetch_class(int st)
{
	if (st == 200)
		return PP_FETCH_OK;
	if (st == 402)
		return PP_FETCH_SLOW;
	if (st == 400 || st == 401 || st == 403)
		return PP_FETCH_FATAL;
	return PP_FETCH_RETRY; /* 0 (transport), 404 from a gateway mid-deploy, 429, 5xx, 3xx ... never give up on those */
}

static int publish_url(const char *in, char *out, size_t cap)
{
	pp_url u;
	int n;
	if (pp_url_parse(in, &u) != 0 || strncmp(in, "http", 4) != 0)
		return -1;
	/* like ppobs: only scheme + authority matter, the path is always /v1/publish/requests */
	n = snprintf(out, cap, "%s://%s:%d/v1/publish/requests", u.tls ? "https" : "http", u.host, u.port);
	return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int pp_ppcenter_fetch(const pp_ppcenter_cfg *cfg, pp_publish_decision *d, char *err, size_t errsz)
{
	char url[512], body[1536];
	char *resp;
	size_t rn = 0;
	int status = 0, bl, rc;
	pp_net_opts o;
	char neterr[160] = "";

	if (publish_url(cfg->url, url, sizeof url) != 0) {
		snprintf(err, errsz, "invalid ppcenter URL");
		return PP_FETCH_FATAL;
	}
	bl = pp_ppcenter_build_request(body, sizeof body, cfg->app_id, cfg->app_secret, cfg->stream_name, cfg->region,
				       cfg->want_degrade);
	if (bl < 0) {
		snprintf(err, errsz, "request too large");
		return PP_FETCH_FATAL;
	}
	resp = (char *)malloc(RESP_MAX);
	if (!resp) {
		snprintf(err, errsz, "out of memory");
		return PP_FETCH_RETRY;
	}
	memset(&o, 0, sizeof o);
	o.verify = cfg->tls_verify;
	o.ca_file = cfg->ca_file;
	o.timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 8000;

	if (pp_http_post(url, "application/json", body, (size_t)bl, NULL, &o, o.timeout_ms, &status, resp, RESP_MAX,
			 &rn, neterr, sizeof neterr) != 0) {
		snprintf(err, errsz, "ppcenter request failed: %s", neterr);
		free(resp);
		return PP_FETCH_RETRY;
	}
	rc = pp_fetch_class(status);
	if (rc == PP_FETCH_OK) {
		if (pp_ppcenter_parse_response(resp, rn, d) != 0) {
			snprintf(err, errsz, "ppcenter returned an invalid publish decision");
			rc = PP_FETCH_RETRY;
		}
	} else {
		char code[64] = "", msg[128] = "";
		pp_json_str(resp, rn, "code", code, sizeof code);
		pp_json_str(resp, rn, "message", msg, sizeof msg);
		snprintf(err, errsz, "ppcenter answered HTTP %d%s%s%s%s", status, code[0] ? " " : "", code,
			 msg[0] ? ": " : "", msg);
	}
	memset(resp, 0, RESP_MAX); /* tokens */
	free(resp);
	return rc;
}
