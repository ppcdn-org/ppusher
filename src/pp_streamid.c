#include "pp_streamid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *pp_codec_name(int codec)
{
	switch (codec) {
	case PP_CODEC_H264: return "h264";
	case PP_CODEC_HEVC: return "hevc";
	default: return "";
	}
}

int pp_stream_name_valid(const char *s)
{
	size_t i, n;
	if (!s || !*s)
		return 0;
	n = strlen(s);
	if (n > 128)
		return 0;
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		int alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
		if (i == 0 ? !alnum : !(alnum || c == '.' || c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

int pp_strip_codec_suffix(char *stream)
{
	size_t n = strlen(stream);
	if (n >= 5 && (strcmp(stream + n - 5, "/h264") == 0 || strcmp(stream + n - 5, "/hevc") == 0)) {
		stream[n - 5] = 0;
		return 1;
	}
	return 0;
}

static int token_ok(const char *t)
{
	for (; *t; t++) {
		unsigned char c = (unsigned char)*t;
		if (c <= 0x20 || c >= 0x7F || c == ':' || c == ',')
			return 0;
	}
	return 1;
}

int pp_streamid_build(char *out, size_t outsz, const char *app_id, const char *stream, int codec, const char *token)
{
	int n;
	const char *cn = pp_codec_name(codec);

	if (!pp_stream_name_valid(app_id) || !pp_stream_name_valid(stream))
		return PP_SID_INVALID;
	if (token && !token_ok(token))
		return PP_SID_INVALID;
	if (token && *token)
		n = snprintf(out, outsz, "publish:%s/%s%s%s:%s", app_id, stream, *cn ? "/" : "", cn, token);
	else
		n = snprintf(out, outsz, "publish:%s/%s%s%s", app_id, stream, *cn ? "/" : "", cn);
	if (n < 0 || (size_t)n >= outsz || n >= PP_STREAMID_MAX)
		return PP_SID_TOO_LONG;
	return PP_SID_OK;
}

void pp_mask_secret(const char *s, char *out, size_t outsz)
{
	size_t n = s ? strlen(s) : 0;
	char head[7];
	if (n == 0) {
		snprintf(out, outsz, "(empty)");
		return;
	}
	memcpy(head, s, n < 6 ? n : 6);
	head[n < 6 ? n : 6] = 0;
	snprintf(out, outsz, "%s...(len=%u)", head, (unsigned)n);
}

int pp_split_endpoint(const char *ep, char *host, size_t hostsz, int *port, int default_port)
{
	const char *p = ep, *h, *he;
	size_t n;
	long pv;

	if (!ep || !*ep)
		return -1;
	if (strncmp(p, "srt://", 6) == 0)
		p += 6;
	h = p;
	if (*p == '[') {
		h = p + 1;
		he = strchr(h, ']');
		if (!he)
			return -1;
		p = he + 1;
	} else {
		he = h;
		while (*he && *he != ':' && *he != '/' && *he != '?' && *he != '#')
			he++;
		p = he;
	}
	n = (size_t)(he - h);
	if (n == 0 || n >= hostsz)
		return -1;
	memcpy(host, h, n);
	host[n] = 0;
	*port = default_port;
	if (*p == ':') {
		char *end;
		pv = strtol(p + 1, &end, 10);
		if (end == p + 1 || pv < 1 || pv > 65535)
			return -1;
		*port = (int)pv;
	}
	return 0;
}
