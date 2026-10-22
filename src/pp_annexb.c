#include "pp_annexb.h"
#include "pp_streamid.h"

int pp_annexb_next(const uint8_t *buf, size_t len, size_t *pos, const uint8_t **nal, size_t *nlen)
{
	size_t i = *pos, s, e, j;

	for (;;) {
		while (i + 3 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1))
			i++;
		if (i + 3 > len) {
			*pos = len;
			return 0;
		}
		s = i + 3;
		j = s;
		while (j + 3 <= len && !(buf[j] == 0 && buf[j + 1] == 0 && buf[j + 2] == 1))
			j++;
		e = (j + 3 <= len) ? j : len;
		*pos = e;
		i = e;
		while (e > s && buf[e - 1] == 0)
			e--;
		if (e == s)
			continue; /* empty NAL, keep scanning */
		*nal = buf + s;
		*nlen = e - s;
		return 1;
	}
}

int pp_nal_type(int codec, uint8_t b)
{
	return codec == PP_CODEC_HEVC ? ((b >> 1) & 0x3F) : (b & 0x1F);
}

int pp_nal_is_vcl(int codec, int type)
{
	return codec == PP_CODEC_HEVC ? type <= 31 : (type >= 1 && type <= 5);
}

int pp_nal_is_idr(int codec, int type)
{
	return codec == PP_CODEC_HEVC ? (type >= 16 && type <= 21) : type == 5;
}

int pp_nal_kind(int codec, int type)
{
	if (codec == PP_CODEC_HEVC) {
		switch (type) {
		case 32: return PP_NAL_VPS;
		case 33: return PP_NAL_SPS;
		case 34: return PP_NAL_PPS;
		case 35: return PP_NAL_AUD;
		case 39:
		case 40: return PP_NAL_SEI;
		default: return 0;
		}
	}
	switch (type) {
	case 6: return PP_NAL_SEI;
	case 7: return PP_NAL_SPS;
	case 8: return PP_NAL_PPS;
	case 9: return PP_NAL_AUD;
	default: return 0;
	}
}

int pp_epb_escape(const uint8_t *in, size_t n, uint8_t *out, size_t outsz)
{
	size_t i, o = 0;
	int zeros = 0;
	for (i = 0; i < n; i++) {
		if (zeros >= 2 && in[i] <= 3) {
			if (o >= outsz)
				return -1;
			out[o++] = 3;
			zeros = 0;
		}
		if (o >= outsz)
			return -1;
		out[o++] = in[i];
		zeros = in[i] == 0 ? zeros + 1 : 0;
	}
	return (int)o;
}
