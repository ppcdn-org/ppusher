/*
 * Writes synthetic MPEG-TS files with the muxer (no encoder needed) so the Go
 * interop check (tests/interop) can run in CI without ffmpeg.
 *   gen_ts <out-dir>
 *     h264_opus.ts        1 layer
 *     hevc_aac_sei.ts    1 layer, with SEI timestamps
 *     h264_x4_opus.ts    4-layer simulcast ladder (ppmmx maximum for H.264)
 *     hevc_x3_aac_sei.ts 3-layer simulcast ladder (ppmmx maximum for HEVC)
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

static int gen(const char *path, int codec, int audio, int layers, int sei)
{
	pp_ts_config c;
	pp_ts *ts;
	uint8_t au[4096], body[600], pk[80];
	uint8_t sps264[] = {0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x40, 0x16, 0xE8, 0x40};
	static const uint8_t pps264[] = {0x68, 0xCE, 0x3C, 0x80};
	static const uint8_t vps265[] = {0x40, 0x01, 0x0C, 0x01};
	uint8_t sps265[] = {0x42, 0x01, 0x01, 0x01, 0x60};
	static const uint8_t pps265[] = {0x44, 0x01, 0xC1, 0x72};
	int f, i, l;

	g_f = fopen(path, "wb");
	if (!g_f)
		return 1;
	memset(&c, 0, sizeof c);
	c.video_codec = codec;
	c.video_layers = layers;
	c.audio_codec = audio;
	c.audio_channels = 2;
	c.audio_sample_rate = 48000;
	c.sei_timestamp = sei;
	c.sink = sink;
	ts = pp_ts_create(&c);
	if (!ts)
		return 1;
	memset(pk, 0x55, sizeof pk);
	pk[0] = 0xFC; /* Opus TOC: CELT 20 ms */
	for (f = 0; f < 90; f++) {
		int64_t t = (int64_t)f * 3000; /* 30 fps */
		int key = f % 30 == 0;
		for (l = 0; l < layers; l++) { /* layer 0 = highest quality, bigger frames */
			size_t o = 0, blen = sizeof body - (size_t)l * 120;
			for (i = 0; i < (int)blen; i++)
				body[i] = (uint8_t)(i * 7 + 3 + l) | 1;
			if (key) {
				if (codec == PP_CODEC_HEVC) {
					sps265[4] = (uint8_t)(0x60 + l); /* every layer has its own parameter sets */
					o += nal(au + o, vps265, sizeof vps265);
					o += nal(au + o, sps265, sizeof sps265);
					o += nal(au + o, pps265, sizeof pps265);
				} else {
					sps264[9] = (uint8_t)(0x40 + l);
					o += nal(au + o, sps264, sizeof sps264);
					o += nal(au + o, pps264, sizeof pps264);
				}
			}
			body[0] = codec == PP_CODEC_HEVC ? (key ? 0x26 : 0x02) : (key ? 0x65 : 0x41);
			if (codec == PP_CODEC_HEVC)
				body[1] = 0x01;
			o += nal(au + o, body, blen);
			pp_ts_write_video_layer(ts, l, au, o, t, t, key, 1760000000000LL + f * 33, l == layers - 1);
		}
		for (i = 0; i < 2; i++)
			pp_ts_write_audio(ts, pk, sizeof pk, t + i * 1440);
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
	if (gen(p, PP_CODEC_H264, PP_AUDIO_OPUS, 1, 0))
		return 1;
	snprintf(p, sizeof p, "%s/hevc_aac_sei.ts", argv[1]);
	if (gen(p, PP_CODEC_HEVC, PP_AUDIO_AAC, 1, 1))
		return 1;
	snprintf(p, sizeof p, "%s/h264_x4_opus.ts", argv[1]);
	if (gen(p, PP_CODEC_H264, PP_AUDIO_OPUS, 4, 0))
		return 1;
	snprintf(p, sizeof p, "%s/hevc_x3_aac_sei.ts", argv[1]);
	return gen(p, PP_CODEC_HEVC, PP_AUDIO_AAC, 3, 1);
}
