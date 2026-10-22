#include "pp_json.h"

#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
		p++;
	return p;
}

/* p points at the opening quote. Returns pointer just past the closing quote, or NULL. */
static const char *skip_string(const char *p, const char *e)
{
	p++;
	while (p < e) {
		if (*p == '\\') {
			p += 2;
			continue;
		}
		if (*p == '"')
			return p + 1;
		p++;
	}
	return NULL;
}

static const char *skip_value(const char *p, const char *e)
{
	int depth = 0;

	p = skip_ws(p, e);
	if (p >= e)
		return NULL;
	if (*p == '"')
		return skip_string(p, e);
	if (*p == '{' || *p == '[') {
		while (p < e) {
			if (*p == '"') {
				p = skip_string(p, e);
				if (!p)
					return NULL;
				continue;
			}
			if (*p == '{' || *p == '[')
				depth++;
			else if (*p == '}' || *p == ']') {
				depth--;
				if (depth == 0)
					return p + 1;
			}
			p++;
		}
		return NULL;
	}
	while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
		p++;
	return p;
}

int pp_json_find(const char *json, size_t n, const char *key, const char **v, size_t *vn)
{
	const char *p = json, *e = json + n;
	size_t klen = strlen(key);

	if (!json)
		return 0;
	p = skip_ws(p, e);
	if (p >= e || *p != '{')
		return 0;
	p++;
	for (;;) {
		const char *ks, *ke, *vs, *ve;
		p = skip_ws(p, e);
		if (p >= e || *p == '}')
			return 0;
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p != '"')
			return 0;
		ks = p + 1;
		p = skip_string(p, e);
		if (!p)
			return 0;
		ke = p - 1;
		p = skip_ws(p, e);
		if (p >= e || *p != ':')
			return 0;
		p++;
		p = skip_ws(p, e);
		vs = p;
		ve = skip_value(p, e);
		if (!ve)
			return 0;
		if ((size_t)(ke - ks) == klen && memcmp(ks, key, klen) == 0) {
			*v = vs;
			*vn = (size_t)(ve - vs);
			return 1;
		}
		p = ve;
	}
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_hex4(const char *p, const char *e, unsigned *out)
{
	unsigned v = 0;
	int i;
	if (e - p < 4)
		return 0;
	for (i = 0; i < 4; i++) {
		int h = hexval(p[i]);
		if (h < 0)
			return 0;
		v = v * 16 + (unsigned)h;
	}
	*out = v;
	return 1;
}

static size_t put_utf8(unsigned cp, char *out, size_t pos, size_t cap)
{
	char tmp[4];
	size_t n, i;
	if (cp < 0x80) {
		tmp[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		tmp[0] = (char)(0xC0 | (cp >> 6));
		tmp[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp < 0x10000) {
		tmp[0] = (char)(0xE0 | (cp >> 12));
		tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else {
		tmp[0] = (char)(0xF0 | (cp >> 18));
		tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		tmp[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	}
	for (i = 0; i < n; i++)
		if (pos + i < cap)
			out[pos + i] = tmp[i];
	return n;
}

int pp_json_str(const char *json, size_t n, const char *key, char *out, size_t outsz)
{
	const char *v, *p, *e;
	size_t vn, pos = 0;

	if (outsz == 0)
		return 0;
	out[0] = 0;
	if (!pp_json_find(json, n, key, &v, &vn) || vn < 2 || v[0] != '"')
		return 0;
	p = v + 1;
	e = v + vn - 1;
	while (p < e) {
		char c = *p++;
		if (c == '\\' && p < e) {
			char x = *p++;
			switch (x) {
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'u': {
				unsigned cp;
				if (!parse_hex4(p, e, &cp))
					return 0;
				p += 4;
				if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
					unsigned lo;
					if (parse_hex4(p + 2, e, &lo) && lo >= 0xDC00 && lo < 0xE000) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						p += 6;
					}
				}
				pos += put_utf8(cp, out, pos, outsz - 1);
				continue;
			}
			default:
				c = x;
				break;
			}
		}
		if (pos < outsz - 1)
			out[pos] = c;
		pos++;
	}
	out[pos < outsz - 1 ? pos : outsz - 1] = 0;
	return 1;
}

int pp_json_int(const char *json, size_t n, const char *key, int64_t *out)
{
	const char *v;
	size_t vn;
	char buf[40];
	char *end;
	double d;

	if (!pp_json_find(json, n, key, &v, &vn) || vn == 0 || vn >= sizeof buf)
		return 0;
	if (!((v[0] >= '0' && v[0] <= '9') || v[0] == '-'))
		return 0;
	memcpy(buf, v, vn);
	buf[vn] = 0;
	if (!memchr(buf, '.', vn) && !memchr(buf, 'e', vn) && !memchr(buf, 'E', vn)) {
		*out = strtoll(buf, &end, 10);
		return end != buf;
	}
	d = strtod(buf, &end);
	if (end == buf)
		return 0;
	*out = (int64_t)d;
	return 1;
}

int pp_json_uint(const char *json, size_t n, const char *key, uint64_t *out)
{
	const char *v;
	size_t vn;
	char buf[40];
	char *end;

	if (!pp_json_find(json, n, key, &v, &vn) || vn == 0 || vn >= sizeof buf)
		return 0;
	if (v[0] < '0' || v[0] > '9')
		return 0;
	memcpy(buf, v, vn);
	buf[vn] = 0;
	*out = strtoull(buf, &end, 10);
	return end != buf;
}

int pp_json_bool(const char *json, size_t n, const char *key, int *out)
{
	const char *v;
	size_t vn;
	if (!pp_json_find(json, n, key, &v, &vn))
		return 0;
	if (vn == 4 && memcmp(v, "true", 4) == 0) {
		*out = 1;
		return 1;
	}
	if (vn == 5 && memcmp(v, "false", 5) == 0) {
		*out = 0;
		return 1;
	}
	return 0;
}

int pp_json_obj(const char *json, size_t n, const char *key, const char **sub, size_t *subn)
{
	if (!pp_json_find(json, n, key, sub, subn) || *subn < 2 || (*sub)[0] != '{')
		return 0;
	return 1;
}

int pp_json_escape(const char *s, char *out, size_t outsz)
{
	size_t pos = 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		char tmp[8];
		size_t k, tl;
		switch (c) {
		case '"': memcpy(tmp, "\\\"", 2); tl = 2; break;
		case '\\': memcpy(tmp, "\\\\", 2); tl = 2; break;
		case '\n': memcpy(tmp, "\\n", 2); tl = 2; break;
		case '\r': memcpy(tmp, "\\r", 2); tl = 2; break;
		case '\t': memcpy(tmp, "\\t", 2); tl = 2; break;
		default:
			if (c < 0x20) {
				static const char hx[] = "0123456789abcdef";
				memcpy(tmp, "\\u00", 4);
				tmp[4] = hx[c >> 4];
				tmp[5] = hx[c & 15];
				tl = 6;
			} else {
				tmp[0] = (char)c;
				tl = 1;
			}
		}
		if (pos + tl + 1 > outsz)
			return -1;
		for (k = 0; k < tl; k++)
			out[pos++] = tmp[k];
	}
	if (pos + 1 > outsz)
		return -1;
	out[pos] = 0;
	return (int)pos;
}
