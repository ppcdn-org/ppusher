#ifndef PP_ANNEXB_H
#define PP_ANNEXB_H

#include <stddef.h>
#include <stdint.h>

/*
 * Iterate Annex-B NAL units. *pos starts at 0. On success returns 1 and sets
 * `nal`/`nlen` to the NAL (start code and trailing zero bytes excluded); returns
 * 0 when no further NAL exists.
 */
int pp_annexb_next(const uint8_t *buf, size_t len, size_t *pos, const uint8_t **nal, size_t *nlen);

/* codec: PP_CODEC_H264 / PP_CODEC_HEVC (pp_streamid.h) */
int pp_nal_type(int codec, uint8_t first_byte);
int pp_nal_is_vcl(int codec, int type);
int pp_nal_is_idr(int codec, int type);

#define PP_NAL_VPS 1
#define PP_NAL_SPS 2
#define PP_NAL_PPS 3
#define PP_NAL_AUD 4
#define PP_NAL_SEI 5
/* Maps a raw NAL type to one of PP_NAL_* (0 = other). */
int pp_nal_kind(int codec, int type);

/* Insert emulation-prevention bytes; returns output length or -1 if outsz is too small. */
int pp_epb_escape(const uint8_t *in, size_t n, uint8_t *out, size_t outsz);

#endif
