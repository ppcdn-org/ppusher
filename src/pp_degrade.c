#include "pp_degrade.h"
#include "pp_json.h"

#include <stdio.h>
#include <string.h>

int pp_degrade_parse(const char *json, size_t n, pp_degrade_msg *m)
{
	char type[32];
	int64_t iv;
	uint64_t uv;
	const char *s;
	size_t sn;

	memset(m, 0, sizeof *m);
	m->bitrate_percent = 100;
	if (!pp_json_find(json, n, "type", &s, &sn))
		return -1;
	if (!pp_json_str(json, n, "type", type, sizeof type))
		return -1;

	pp_json_str(json, n, "path", m->path, sizeof m->path);
	pp_json_str(json, n, "codec", m->codec, sizeof m->codec);

	if (strcmp(type, "TARGET_STATE") == 0) {
		int b;
		m->type = PP_MSG_TARGET_STATE;
		if (pp_json_int(json, n, "layers", &iv))
			m->layers = iv < 0 ? 0 : (iv > 64 ? 64 : (int)iv);
		if (pp_json_int(json, n, "bitrate_percent", &iv))
			m->bitrate_percent = iv < 1 ? 1 : (iv > 100 ? 100 : (int)iv);
		if (pp_json_int(json, n, "latency_ms", &iv))
			m->latency_ms = iv < 0 ? 0 : (iv > 60000 ? 60000 : (int)iv);
		if (pp_json_bool(json, n, "reconnect", &b))
			m->reconnect = b;
		pp_json_str(json, n, "reason", m->reason, sizeof m->reason);
	} else if (strcmp(type, "LOSS_SAMPLE") == 0) {
		m->type = PP_MSG_LOSS_SAMPLE;
		if (pp_json_uint(json, n, "unrecoverable_delta", &uv))
			m->unrecoverable_delta = uv;
		if (pp_json_uint(json, n, "expected_delta", &uv))
			m->expected_delta = uv;
	} else if (strcmp(type, "ALERT") == 0) {
		m->type = PP_MSG_ALERT;
		pp_json_str(json, n, "reason", m->reason, sizeof m->reason);
	}
	return 0;
}

int pp_degrade_ws_url(const char *whip_url, char *out, size_t outsz)
{
	char tmp[512];
	size_t n;
	int r;

	if (!whip_url)
		return -1;
	n = strlen(whip_url);
	if (n == 0 || n >= sizeof tmp)
		return -1;
	memcpy(tmp, whip_url, n + 1);
	while (n > 0 && tmp[n - 1] == '/')
		tmp[--n] = 0;
	if (strchr(tmp, '?') || strchr(tmp, '#'))
		return -1;
	if (n < 5 || strcmp(tmp + n - 5, "/whip") != 0)
		return -1;
	tmp[n - 5] = 0;
	if (strncmp(tmp, "https://", 8) == 0)
		r = snprintf(out, outsz, "wss://%s/ws/whip", tmp + 8);
	else if (strncmp(tmp, "http://", 7) == 0)
		r = snprintf(out, outsz, "ws://%s/ws/whip", tmp + 7);
	else
		return -1;
	return (r < 0 || (size_t)r >= outsz) ? -1 : 0;
}

int pp_target_apply(pp_target_tracker *t, const pp_degrade_msg *m)
{
	int flags = 0;
	if (!t->have) {
		t->have = 1;
		t->layers = m->layers;
		t->bitrate_percent = m->bitrate_percent;
		/* The first state we ever see only matters if it is not the undegraded default. */
		if (m->bitrate_percent != 100)
			flags |= PP_TGT_BITRATE_CHANGED;
	} else {
		if (m->bitrate_percent != t->bitrate_percent)
			flags |= PP_TGT_BITRATE_CHANGED;
		if (m->layers != t->layers)
			flags |= PP_TGT_LAYERS_CHANGED;
		t->layers = m->layers;
		t->bitrate_percent = m->bitrate_percent;
	}
	if (m->reconnect)
		flags |= PP_TGT_RECONNECT;
	return flags;
}
