/*
 * Writes synthetic MPEG-TS files with the muxer (no encoder needed) so the Go
 * interop check (tests/interop) can run in CI without ffmpeg.
 *   gen_ts <out-dir>   -> h264_opus.ts, hevc_aac.ts (with SEI timestamps)
 */
#include "pp_streamid.h"
#include "pp_ts.h"

#include <stdio.h>
#include <string.h>

static FILE *g_f;

static void sink(const uint8_t *m, size_t n, void *ud)
{
	(void)ud;
	fwrite(m, 1, n, g_f);
}

static size_t nal(uint8_t *o, const uint8_t *p, size_t n)
{
	o[0] = 0;
	o[1] = 0;
	o[2] = 0;
	o[3] = 1;
	memcpy(o + 4, p, n);
	return 4 + n;
}

static int gen(const char *path, int codec, int audio, int sei)
{
	pp_ts_config c;
	pp_ts *ts;
	uint8_t au[4096], body[600], pk[80];
	static const uint8_t sps264[] = {0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x40, 0x16, 0xE8, 0x40};
	static const uint8_t pps264[] = {0x68, 0xCE, 0x3C, 0x80};
	static const uint8_t vps265[] = {0x40, 0x01, 0x0C, 0x01};
	static const uint8_t sps265[] = {0x42, 0x01, 0x01, 0x01, 0x60};
	static const uint8_t pps265[] = {0x44, 0x01, 0xC1, 0x72};
	int f, i;

	g_f = fopen(path, "wb");
	if (!g_f)
		return 1;
	memset(&c, 0, sizeof c);
	c.video_codec = codec;
	c.audio_codec = audio;
	c.audio_channels = 2;
	c.audio_sample_rate = 48000;
	c.sei_timestamp = sei;
	c.sink = sink;
	ts = pp_ts_create(&c);
	for (i = 0; i < (int)sizeof body; i++)
		body[i] = (uint8_t)(i * 7 + 3) | 1;
	memset(pk, 0x55, sizeof pk);
	pk[0] = 0xFC; /* Opus TOC: CELT 20 ms */
	for (f = 0; f < 90; f++) {
		int64_t t = (int64_t)f * 3000; /* 30 fps */
		int key = f % 30 == 0;
		size_t o = 0;
		if (key) {
			if (codec == PP_CODEC_HEVC) {
				o += nal(au + o, vps265, sizeof vps265);
				o += nal(au + o, sps265, sizeof sps265);
				o += nal(au + o, pps265, sizeof pps265);
			} else {
				o += nal(au + o, sps264, sizeof sps264);
				o += nal(au + o, pps264, sizeof pps264);
			}
		}
		body[0] = codec == PP_CODEC_HEVC ? (key ? 0x26 : 0x02) : (key ? 0x65 : 0x41);
		if (codec == PP_CODEC_HEVC)
			body[1] = 0x01;
		o += nal(au + o, body, sizeof body);
		pp_ts_write_video(ts, au, o, t, t, key, 1760000000000LL + f * 33);
		for (i = 0; i < 2; i++) {
			if (audio == PP_AUDIO_OPUS || audio == PP_AUDIO_AAC)
				pp_ts_write_audio(ts, pk, sizeof pk, t + i * 1440);
		}
	}
	pp_ts_flush(ts);
	pp_ts_destroy(ts);
	fclose(g_f);
	return 0;
}

int main(int argc, char **argv)
{
	char p[512];
	if (argc < 2)
		return 2;
	snprintf(p, sizeof p, "%s/h264_opus.ts", argv[1]);
	if (gen(p, PP_CODEC_H264, PP_AUDIO_OPUS, 0))
		return 1;
	snprintf(p, sizeof p, "%s/hevc_aac_sei.ts", argv[1]);
	return gen(p, PP_CODEC_HEVC, PP_AUDIO_AAC, 1);
}
