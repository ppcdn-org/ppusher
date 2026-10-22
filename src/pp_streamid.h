#ifndef PP_STREAMID_H
#define PP_STREAMID_H

#include <stddef.h>

#define PP_STREAMID_MAX 512 /* libsrt SRTO_STREAMID limit */

#define PP_CODEC_NONE 0
#define PP_CODEC_H264 1
#define PP_CODEC_HEVC 2

#define PP_SID_OK 0
#define PP_SID_INVALID (-1)
#define PP_SID_TOO_LONG (-2)

/* "h264" / "hevc" / "" */
const char *pp_codec_name(int codec);

/* ppcenter's streamNameV1Pattern: ^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$ */
int pp_stream_name_valid(const char *s);

/* Remove a trailing "/h264" or "/hevc" in place; returns 1 if something was removed. */
int pp_strip_codec_suffix(char *stream);

/*
 * publish:{appId}/{stream}[/{codec}][:{token}]
 * token may be NULL/"" (no third segment). A token containing ':' ',' or
 * whitespace is rejected: ppmmx splits the streamid on ':' and SRT field
 * lists use ','.
 */
int pp_streamid_build(char *out, size_t outsz, const char *app_id, const char *stream, int codec, const char *token);

/* Log-safe rendering of a secret: first 6 chars + "...(len=N)". */
void pp_mask_secret(const char *s, char *out, size_t outsz);

/* "host", "host:port", "[v6]:port", optional "srt://" prefix and trailing path. */
int pp_split_endpoint(const char *ep, char *host, size_t hostsz, int *port, int default_port);

#endif
