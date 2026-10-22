#include "pp_test.h"
#include "pp_streamid.h"
#include "pp_ts.h"

#include <stdlib.h>

/* ---------- a small independent TS reader used to check the muxer's output ---------- */

#define CAP (4 * 1024 * 1024)
static uint8_t g_ts[CAP];
static size_t g_n;
static int g_msgs, g_bad_msg;

static void sink(const uint8_t *m, size_t len, void *ud)
{
	(void)ud;
	if (len != PP_TS_MSG_SIZE)
		g_bad_msg++;
	if (g_n + len <= CAP)
		memcpy(g_ts + g_n, m, len);
	g_n += len;
	g_msgs++;
}

static void reset(void)
{
	g_n = 0;
	g_msgs = 0;
	g_bad_msg = 0;
}

static const uint8_t *pkt(size_t i)
{
	return g_ts + i * 188;
}

static int pkt_pid(const uint8_t *p)
{
	return ((p[1] & 0x1F) << 8) | p[2];
}

typedef struct {
	size_t len;
	uint8_t data[200000];
	int af_present, af_rai, af_pcr;
	int64_t pcr_base;
	int first_pkt_index;
} pes_t;

static pes_t g_pes;

/* reassemble the idx-th PES of `pid` (starting at a PUSI packet) */
static int get_pes(int pid, int idx)
{
	size_t i, count = g_n / 188;
	int seen = -1;
	memset(&g_pes, 0, sizeof g_pes);
	for (i = 0; i < count; i++) {
		const uint8_t *p = pkt(i);
		int afc, off = 4;
		if (pkt_pid(p) != pid)
			continue;
		afc = (p[3] >> 4) & 3;
		if (p[1] & 0x40)
			seen++;
		if (seen != idx)
			continue;
		if (afc & 2) {
			int afl = p[4];
			if ((p[1] & 0x40) && afl > 0) {
				g_pes.af_present = 1;
				g_pes.af_rai = (p[5] & 0x40) != 0;
				g_pes.af_pcr = (p[5] & 0x10) != 0;
				if (g_pes.af_pcr)
					g_pes.pcr_base = ((int64_t)p[6] << 25) | ((int64_t)p[7] << 17) | ((int64_t)p[8] << 9) |
							 ((int64_t)p[9] << 1) | (p[10] >> 7);
			}
			off += 1 + afl;
		}
		if (p[1] & 0x40)
			g_pes.first_pkt_index = (int)i;
		if (afc & 1) {
			memcpy(g_pes.data + g_pes.len, p + off, 188 - (size_t)off);
			g_pes.len += 188 - (size_t)off;
		}
	}
	return seen >= idx;
}

static int64_t rd_ts(const uint8_t *p)
{
	return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) | ((int64_t)(p[2] >> 1) << 15) |
	       ((int64_t)p[3] << 7) | (p[4] >> 1);
}

/* every packet starts with 0x47 and per-PID continuity counters count 0..15 without gaps */
static void check_stream_integrity(void)
{
	size_t i, count = g_n / 188;
	int cc[8192];
	int seen[8192];
	memset(seen, 0, sizeof seen);
	CHECK_INT(g_n % 1316, 0);
	CHECK_INT(g_bad_msg, 0);
	for (i = 0; i < count; i++) {
		const uint8_t *p = pkt(i);
		int pid = pkt_pid(p);
		CHECK_INT(p[0], 0x47);
		if (pid == 0x1FFF)
			continue;
		if (p[3] & 0x10) {
			int c = p[3] & 0x0F;
			if (seen[pid])
				CHECK_INT(c, (cc[pid] + 1) & 15);
			cc[pid] = c;
			seen[pid] = 1;
		}
	}
}

static const uint8_t SPS264[] = {0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x40, 0x16, 0xE8, 0x40};
static const uint8_t PPS264[] = {0x68, 0xCE, 0x3C, 0x80};

static size_t put_nal(uint8_t *dst, int four, const uint8_t *nal, size_t n)
{
	size_t o = 0;
	if (four)
		dst[o++] = 0;
	dst[o++] = 0;
	dst[o++] = 0;
	dst[o++] = 1;
	memcpy(dst + o, nal, n);
	return o + n;
}

static size_t build_idr264(uint8_t *au, int with_params)
{
	uint8_t idr[200];
	size_t o = 0, i;
	idr[0] = 0x65;
	for (i = 1; i < sizeof idr; i++)
		idr[i] = (uint8_t)(i * 7 + 1);
	if (with_params) {
		o += put_nal(au + o, 1, SPS264, sizeof SPS264);
		o += put_nal(au + o, 1, PPS264, sizeof PPS264);
	}
	o += put_nal(au + o, 1, idr, sizeof idr);
	return o;
}

static size_t build_p264(uint8_t *au, size_t n)
{
	uint8_t p[2000];
	size_t i;
	p[0] = 0x41;
	for (i = 1; i < n && i < sizeof p; i++)
		p[i] = (uint8_t)(i * 13 + 5) | 1;
	return put_nal(au, 0, p, n < sizeof p ? n : sizeof p);
}

static pp_ts *mk(int codec, int audio, int ch, int rate, int sei)
{
	pp_ts_config c;
	memset(&c, 0, sizeof c);
	c.video_codec = codec;
	c.audio_codec = audio;
	c.audio_channels = ch;
	c.audio_sample_rate = rate;
	c.sei_timestamp = sei;
	c.sink = sink;
	reset();
	return pp_ts_create(&c);
}

/* ---------- tests ---------- */

static void test_crc(void)
{
	/* CRC-32/MPEG-2 check value */
	CHECK(pp_crc32_mpeg((const uint8_t *)"123456789", 9) == 0x0376E6E7u);
}

static void test_tables(void)
{
	pp_ts *ts = mk(PP_CODEC_H264, PP_AUDIO_OPUS, 2, 0, 0);
	const uint8_t *p;
	const uint8_t *s;
	size_t sl, pos;
	static const uint8_t opus_desc[] = {0x05, 0x04, 'O', 'p', 'u', 's', 0x7F, 0x02, 0x80, 0x02};

	CHECK(ts != NULL);
	pp_ts_write_tables(ts);
	pp_ts_flush(ts);
	CHECK_INT(g_msgs, 1);
	check_stream_integrity();

	/* PAT */
	p = pkt(0);
	CHECK_INT(pkt_pid(p), 0);
	CHECK(p[1] & 0x40);
	CHECK_INT(p[4], 0); /* pointer_field */
	s = p + 5;
	CHECK_INT(s[0], 0x00);
	sl = 3 + (((s[1] & 0x0F) << 8) | s[2]);
	CHECK_INT(pp_crc32_mpeg(s, sl), 0); /* CRC over a section including its CRC is zero */
	CHECK_INT(((s[10] & 0x1F) << 8) | s[11], PP_PID_PMT);

	/* PMT */
	p = pkt(1);
	CHECK_INT(pkt_pid(p), PP_PID_PMT);
	s = p + 5;
	CHECK_INT(s[0], 0x02);
	sl = 3 + (((s[1] & 0x0F) << 8) | s[2]);
	CHECK_INT(pp_crc32_mpeg(s, sl), 0);
	CHECK_INT(((s[8] & 0x1F) << 8) | s[9], PP_PID_VIDEO); /* PCR PID */
	pos = 12;
	CHECK_INT(s[pos], 0x1B);
	CHECK_INT(((s[pos + 1] & 0x1F) << 8) | s[pos + 2], PP_PID_VIDEO);
	CHECK_INT(((s[pos + 3] & 0x0F) << 8) | s[pos + 4], 0);
	pos += 5;
	CHECK_INT(s[pos], 0x06);
	CHECK_INT(((s[pos + 1] & 0x1F) << 8) | s[pos + 2], PP_PID_AUDIO);
	CHECK_INT(((s[pos + 3] & 0x0F) << 8) | s[pos + 4], (int)sizeof opus_desc);
	CHECK(memcmp(s + pos + 5, opus_desc, sizeof opus_desc) == 0);
	CHECK_INT(pos + 5 + sizeof opus_desc + 4, sl);
	pp_ts_destroy(ts);

	/* mono: channel_config_code 1; HEVC: stream_type 0x24; AAC: 0x0F with no descriptors */
	ts = mk(PP_CODEC_HEVC, PP_AUDIO_OPUS, 1, 0, 0);
	pp_ts_write_tables(ts);
	pp_ts_flush(ts);
	s = pkt(1) + 5;
	CHECK_INT(s[12], 0x24);
	CHECK_INT(s[12 + 5 + 5 + 9], 0x01);
	pp_ts_destroy(ts);

	ts = mk(PP_CODEC_H264, PP_AUDIO_AAC, 2, 48000, 0);
	pp_ts_write_tables(ts);
	pp_ts_flush(ts);
	s = pkt(1) + 5;
	CHECK_INT(s[17], 0x0F);
	CHECK_INT(((s[17 + 3] & 0x0F) << 8) | s[17 + 4], 0);
	pp_ts_destroy(ts);

	/* video only */
	ts = mk(PP_CODEC_H264, PP_AUDIO_NONE, 0, 0, 0);
	pp_ts_write_tables(ts);
	pp_ts_flush(ts);
	s = pkt(1) + 5;
	CHECK_INT(3 + (((s[1] & 0x0F) << 8) | s[2]), 12 + 5 + 4);
	pp_ts_destroy(ts);
}

static void test_video_keyframe(void)
{
	pp_ts *ts = mk(PP_CODEC_H264, PP_AUDIO_OPUS, 2, 0, 0);
	uint8_t au[1024];
	size_t n = build_idr264(au, 1);
	static const uint8_t aud[] = {0, 0, 0, 1, 0x09, 0xF0};
	uint8_t *d;

	CHECK_INT(pp_ts_write_video(ts, au, n, 90000, 90000, 1, 0), 0);
	CHECK_INT(g_msgs >= 1, 1);
	check_stream_integrity();

	/* PAT + PMT precede the first video packet */
	CHECK_INT(pkt_pid(pkt(0)), 0);
	CHECK_INT(pkt_pid(pkt(1)), PP_PID_PMT);
	CHECK_INT(pkt_pid(pkt(2)), PP_PID_VIDEO);

	CHECK(get_pes(PP_PID_VIDEO, 0));
	CHECK_INT(g_pes.af_present, 1);
	CHECK_INT(g_pes.af_rai, 1);
	CHECK_INT(g_pes.af_pcr, 1);
	d = g_pes.data;
	CHECK_INT(d[0], 0);
	CHECK_INT(d[1], 0);
	CHECK_INT(d[2], 1);
	CHECK_INT(d[3], 0xE0);
	CHECK_INT(d[4] | d[5], 0); /* unbounded PES */
	CHECK_INT(d[6] & 0xC0, 0x80);
	CHECK_INT(d[7], 0x80); /* PTS only (dts == pts) */
	CHECK_INT(d[8], 5);
	CHECK(rd_ts(d + 9) == 90000 + 90000); /* 1 s timestamp base offset */
	CHECK((d[9] >> 4) == 2);
	/* PCR leads DTS by 0.1 s */
	CHECK(g_pes.pcr_base == 90000 + 90000 - 9000);
	/* payload: AUD, SPS, PPS, IDR - all with 4-byte start codes */
	CHECK(memcmp(d + 14, aud, sizeof aud) == 0);
	CHECK(memcmp(d + 14 + 6, "\x00\x00\x00\x01\x67\x42\xC0", 7) == 0);
	CHECK(memcmp(d + 14 + 6 + 4 + sizeof SPS264, "\x00\x00\x00\x01\x68\xCE", 6) == 0);
	CHECK_INT(g_pes.len >= 14 + 6 + (4 + sizeof SPS264) + (4 + sizeof PPS264) + 4 + 200, 1);
	pp_ts_destroy(ts);
}

static void test_param_set_cache(void)
{
	pp_ts *ts = mk(PP_CODEC_H264, PP_AUDIO_NONE, 0, 0, 0);
	uint8_t au[1024];
	size_t n;
	uint8_t *d;

	n = build_idr264(au, 1);
	pp_ts_write_video(ts, au, n, 0, 0, 1, 0);
	/* encoder emits a later IDR without in-band SPS/PPS: the muxer re-inserts the cached ones */
	n = build_idr264(au, 0);
	pp_ts_write_video(ts, au, n, 90000, 90000, 1, 0);
	CHECK(get_pes(PP_PID_VIDEO, 1));
	d = g_pes.data + 14;
	CHECK(memcmp(d, "\x00\x00\x00\x01\x09\xF0", 6) == 0);
	CHECK(memcmp(d + 6, "\x00\x00\x00\x01", 4) == 0);
	CHECK(memcmp(d + 10, SPS264, sizeof SPS264) == 0);
	CHECK(memcmp(d + 10 + sizeof SPS264, "\x00\x00\x00\x01", 4) == 0);
	CHECK(memcmp(d + 14 + sizeof SPS264, PPS264, sizeof PPS264) == 0);

	/* a non-key AU is passed through without parameter sets */
	n = build_p264(au, 300);
	pp_ts_write_video(ts, au, n, 93000, 93000, 0, 0);
	CHECK(get_pes(PP_PID_VIDEO, 2));
	CHECK_INT(g_pes.af_rai, 0);
	CHECK_INT(g_pes.af_pcr, 1); /* PCR rides on every video AU */
	d = g_pes.data + 14;
	CHECK(memcmp(d, "\x00\x00\x00\x01\x09\xF0\x00\x00\x00\x01\x41", 11) == 0);
	check_stream_integrity();
	pp_ts_destroy(ts);
}

static void test_video_pts_dts_and_timing(void)
{
	pp_ts *ts = mk(PP_CODEC_H264, PP_AUDIO_NONE, 0, 0, 0);
	uint8_t au[1024];
	size_t n = build_idr264(au, 1);
	uint8_t *d;

	pp_ts_write_video(ts, au, n, 183000, 180000, 1, 0);
	CHECK(get_pes(PP_PID_VIDEO, 0));
	d = g_pes.data;
	CHECK_INT(d[7], 0xC0);
	CHECK_INT(d[8], 10);
	CHECK((d[9] >> 4) == 3);
	CHECK((d[14] >> 4) == 1);
	CHECK(rd_ts(d + 9) == 183000 + 90000);
	CHECK(rd_ts(d + 14) == 180000 + 90000);
	CHECK(g_pes.pcr_base == 180000 + 90000 - 9000);
	pp_ts_destroy(ts);

	/* 33-bit wrap */
	ts = mk(PP_CODEC_H264, PP_AUDIO_NONE, 0, 0, 0);
	pp_ts_write_video(ts, au, n, 0x1FFFFFFFFLL, 0x1FFFFFFFFLL, 1, 0);
	CHECK(get_pes(PP_PID_VIDEO, 0));
	CHECK(rd_ts(g_pes.data + 9) == ((0x1FFFFFFFFLL + 90000) & 0x1FFFFFFFFLL));
	pp_ts_destroy(ts);
}

static void test_hevc(void)
{
	static const uint8_t vps[] = {0x40, 0x01, 0x0C, 0x01};
	static const uint8_t sps[] = {0x42, 0x01, 0x01, 0x01, 0x60};
	static const uint8_t pps[] = {0x44, 0x01, 0xC1, 0x72};
	uint8_t idr[100], au[512];
	size_t o = 0, i;
	pp_ts *ts = mk(PP_CODEC_HEVC, PP_AUDIO_NONE, 0, 0, 0);
	uint8_t *d;

	idr[0] = 0x26;
	idr[1] = 0x01;
	for (i = 2; i < sizeof idr; i++)
		idr[i] = (uint8_t)(i + 3);
	/* first IDR carries VPS/SPS/PPS; second carries nothing */
	o += put_nal(au + o, 1, vps, sizeof vps);
	o += put_nal(au + o, 1, sps, sizeof sps);
	o += put_nal(au + o, 1, pps, sizeof pps);
	o += put_nal(au + o, 1, idr, sizeof idr);
	pp_ts_write_video(ts, au, o, 0, 0, 1, 0);
	o = put_nal(au, 1, idr, sizeof idr);
	pp_ts_write_video(ts, au, o, 3000, 3000, 1, 0);

	CHECK(get_pes(PP_PID_VIDEO, 1));
	d = g_pes.data + 14;
	CHECK(memcmp(d, "\x00\x00\x00\x01\x46\x01\x50", 7) == 0); /* HEVC AUD */
	CHECK(memcmp(d + 7, "\x00\x00\x00\x01", 4) == 0);
	CHECK(memcmp(d + 11, vps, sizeof vps) == 0);
	CHECK(memcmp(d + 11 + sizeof vps + 4, sps, sizeof sps) == 0);
	CHECK(memcmp(d + 11 + sizeof vps + 4 + sizeof sps + 4, pps, sizeof pps) == 0);
	check_stream_integrity();
	pp_ts_destroy(ts);
}

static void test_opus(void)
{
	pp_ts *ts = mk(PP_CODEC_H264, PP_AUDIO_OPUS, 2, 0, 0);
	uint8_t f[400];
	uint8_t *d;
	size_t i;

	for (i = 0; i < sizeof f; i++)
		f[i] = (uint8_t)(i * 3 + 1);
	CHECK_INT(pp_ts_write_audio(ts, f, 20, 90000), 0);
	pp_ts_flush(ts);
	CHECK(get_pes(PP_PID_AUDIO, 0));
	d = g_pes.data;
	CHECK_INT(d[3], 0xBD); /* private_stream_1 */
	/* PES_packet_length = optional header (3) + PTS (5) + control header (3) + packet (20) */
	CHECK_INT((d[4] << 8) | d[5], 3 + 5 + 3 + 20);
	CHECK_INT(d[7], 0x80); /* PTS only: mmx rejects Opus whose PTS != DTS */
	CHECK(rd_ts(d + 9) == 180000);
	CHECK_INT(d[14], 0x7F);
	CHECK_INT(d[15], 0xE0);
	CHECK_INT(d[16], 20);
	CHECK(memcmp(d + 17, f, 20) == 0);
	CHECK_INT(g_pes.len, 6 + 3 + 5 + 3 + 20);
	CHECK_INT(g_pes.af_rai, 1);
	CHECK_INT(g_pes.af_pcr, 0); /* PCR is on the video PID */

	/* a 300-byte packet: size is 255 + 45 -> FF 2D */
	CHECK_INT(pp_ts_write_audio(ts, f, 300, 90000 + 1800), 0);
	pp_ts_flush(ts);
	CHECK(get_pes(PP_PID_AUDIO, 1));
	d = g_pes.data;
	CHECK_INT(d[14], 0x7F);
	CHECK_INT(d[15], 0xE0);
	CHECK_INT(d[16], 0xFF);
	CHECK_INT(d[17], 45);
	CHECK(memcmp(d + 18, f, 300) == 0);
	CHECK_INT((d[4] << 8) | d[5], 3 + 5 + 4 + 300);
	/* exactly 255 -> FF 00 */
	CHECK_INT(pp_ts_write_audio(ts, f, 255, 90000 + 3600), 0);
	pp_ts_flush(ts);
	CHECK(get_pes(PP_PID_AUDIO, 2));
	CHECK_INT(g_pes.data[16], 0xFF);
	CHECK_INT(g_pes.data[17], 0);

	pp_ts_flush(ts);
	check_stream_integrity();

	CHECK_INT(pp_ts_write_audio(ts, f, 0, 0), -1);
	CHECK_INT(pp_ts_write_audio(ts, NULL, 5, 0), -1);
	pp_ts_destroy(ts);
}

static void test_aac(void)
{
	pp_ts *ts = mk(PP_CODEC_H264, PP_AUDIO_AAC, 2, 48000, 0);
	uint8_t f[100];
	uint8_t *d;
	int flen;

	memset(f, 0x55, sizeof f);
	CHECK_INT(pp_ts_write_audio(ts, f, sizeof f, 0), 0);
	pp_ts_flush(ts);
	CHECK(get_pes(PP_PID_AUDIO, 0));
	d = g_pes.data;
	CHECK_INT(d[3], 0xC0);
	d += 14;
	CHECK_INT(d[0], 0xFF);
	CHECK_INT(d[1], 0xF1);
	CHECK_INT(d[2], 0x4C);               /* LC, 48 kHz, ch bit2 = 0 */
	CHECK_INT(d[3] >> 6, 2);             /* channel_config low bits = 2 */
	flen = ((d[3] & 3) << 11) | (d[4] << 3) | (d[5] >> 5);
	CHECK_INT(flen, 107);
	CHECK_INT(d[6], 0xFC);
	CHECK(memcmp(d + 7, f, sizeof f) == 0);
	pp_ts_destroy(ts);

	/* sample rate the ADTS table cannot express */
	ts = mk(PP_CODEC_H264, PP_AUDIO_AAC, 2, 12345, 0);
	CHECK_INT(pp_ts_write_audio(ts, f, sizeof f, 0), -1);
	pp_ts_destroy(ts);
}

static void test_message_framing_and_cc(void)
{
	pp_ts *ts = mk(PP_CODEC_H264, PP_AUDIO_OPUS, 2, 0, 0);
	uint8_t au[4096], f[80];
	size_t n, i;
	int k;

	memset(f, 9, sizeof f);
	n = build_idr264(au, 1);
	for (k = 0; k < 60; k++) {
		int64_t t = (int64_t)k * 3000;
		if (k % 30 == 0) {
			pp_ts_write_video(ts, au, n, t, t, 1, 0);
		} else {
			size_t m = build_p264(au + 2000, 100 + (size_t)k * 20);
			pp_ts_write_video(ts, au + 2000, m, t, t, 0, 0);
		}
		for (i = 0; i < 2; i++)
			pp_ts_write_audio(ts, f, sizeof f, t + (int64_t)i * 1500);
	}
	pp_ts_flush(ts);
	CHECK(g_msgs > 5);
	check_stream_integrity(); /* sync bytes, 1316-byte messages, continuity counters per PID */

	/* PAT/PMT re-sent at every keyframe */
	{
		size_t count = g_n / 188, pats = 0;
		for (i = 0; i < count; i++)
			if (pkt_pid(pkt(i)) == 0)
				pats++;
		CHECK(pats >= 2);
	}
	pp_ts_destroy(ts);
}

static void test_flush_pads_with_null_packets(void)
{
	pp_ts *ts = mk(PP_CODEC_H264, PP_AUDIO_NONE, 0, 0, 0);
	uint8_t au[1024];
	size_t n = build_idr264(au, 1), i, count;

	pp_ts_write_video(ts, au, n, 0, 0, 1, 0);
	/* write_video flushes at the AU boundary: the tail of the last message is null packets */
	CHECK_INT(g_n % 1316, 0);
	count = g_n / 188;
	CHECK_INT(pkt_pid(pkt(count - 1)), 0x1FFF);
	CHECK_INT(pkt(count - 1)[3] & 0x30, 0x10);
	for (i = 4; i < 188; i++)
		if (pkt(count - 1)[i] != 0xFF) {
			CHECK(0);
			break;
		}
	/* flushing an already aligned stream adds nothing */
	n = g_n;
	pp_ts_flush(ts);
	CHECK_INT(g_n, n);
	pp_ts_destroy(ts);
}

static size_t unescape(const uint8_t *in, size_t n, uint8_t *out)
{
	size_t i, o = 0;
	int zeros = 0;
	for (i = 0; i < n; i++) {
		if (zeros >= 2 && in[i] == 3) {
			zeros = 0;
			continue;
		}
		out[o++] = in[i];
		zeros = in[i] == 0 ? zeros + 1 : 0;
	}
	return o;
}

static const uint8_t *find_seq(const uint8_t *h, size_t hn, const uint8_t *nd, size_t nn)
{
	size_t i;
	for (i = 0; i + nn <= hn; i++)
		if (memcmp(h + i, nd, nn) == 0)
			return h + i;
	return NULL;
}

static void check_sei_payload(const uint8_t *nal, size_t hdr, int64_t ms)
{
	uint8_t rb[64];
	size_t n = unescape(nal + hdr, 40, rb);
	int i;
	CHECK_INT(rb[0], 5);   /* user_data_unregistered */
	CHECK_INT(rb[1], 24);  /* 16 uuid + 8 timestamp */
	CHECK(memcmp(rb + 2, "OBS-ABSTS-SEI-v1", 16) == 0);
	for (i = 0; i < 8; i++)
		CHECK_INT(rb[18 + i], (int)((uint64_t)ms >> (8 * (7 - i)) & 0xFF));
	CHECK_INT(rb[26], 0x80); /* rbsp trailing bits */
	CHECK(n >= 27);
}

static void test_sei_timestamp(void)
{
	uint8_t au[1024];
	size_t n = build_idr264(au, 1);
	pp_ts *ts;
	const uint8_t *p;
	const uint8_t *idr;
	/* a timestamp whose bytes contain 00 00 01 and therefore need emulation prevention */
	int64_t tricky = 0x0000018F12345678LL;
	int64_t normal = 1760000000123LL;

	ts = mk(PP_CODEC_H264, PP_AUDIO_NONE, 0, 0, 1);
	pp_ts_write_video(ts, au, n, 0, 0, 1, normal);
	CHECK(get_pes(PP_PID_VIDEO, 0));
	p = find_seq(g_pes.data, g_pes.len, (const uint8_t *)"\x00\x00\x00\x01\x06\x05\x18", 7);
	CHECK(p != NULL);
	if (p) {
		check_sei_payload(p + 5, 0, normal);
		/* AVC: the SEI precedes the first slice (no suffix SEI in H.264) */
		idr = find_seq(g_pes.data, g_pes.len, (const uint8_t *)"\x00\x00\x00\x01\x65", 5);
		CHECK(idr != NULL && idr > p);
	}
	pp_ts_destroy(ts);

	ts = mk(PP_CODEC_H264, PP_AUDIO_NONE, 0, 0, 1);
	pp_ts_write_video(ts, au, n, 0, 0, 1, tricky);
	CHECK(get_pes(PP_PID_VIDEO, 0));
	p = find_seq(g_pes.data, g_pes.len, (const uint8_t *)"\x00\x00\x00\x01\x06\x05\x18", 7);
	CHECK(p != NULL);
	if (p) {
		check_sei_payload(p + 5, 0, tricky);
		CHECK(find_seq(p, 60, (const uint8_t *)"\x00\x00\x03\x01", 4) != NULL);
	}
	pp_ts_destroy(ts);

	/* disabled by default */
	ts = mk(PP_CODEC_H264, PP_AUDIO_NONE, 0, 0, 0);
	pp_ts_write_video(ts, au, n, 0, 0, 1, normal);
	CHECK(get_pes(PP_PID_VIDEO, 0));
	CHECK(find_seq(g_pes.data, g_pes.len, (const uint8_t *)"OBS-ABSTS", 9) == NULL);
	pp_ts_destroy(ts);

	/* HEVC: suffix SEI (type 40, header 50 01) after the VCL NAL */
	{
		uint8_t hidr[60], hau[200];
		size_t o, i;
		hidr[0] = 0x26;
		hidr[1] = 0x01;
		for (i = 2; i < sizeof hidr; i++)
			hidr[i] = (uint8_t)i;
		o = put_nal(hau, 1, hidr, sizeof hidr);
		ts = mk(PP_CODEC_HEVC, PP_AUDIO_NONE, 0, 0, 1);
		pp_ts_write_video(ts, hau, o, 0, 0, 1, normal);
		CHECK(get_pes(PP_PID_VIDEO, 0));
		p = find_seq(g_pes.data, g_pes.len, (const uint8_t *)"\x00\x00\x00\x01\x50\x01\x05\x18", 8);
		CHECK(p != NULL);
		if (p) {
			check_sei_payload(p + 6, 0, normal);
			CHECK(find_seq(g_pes.data, g_pes.len, (const uint8_t *)"\x00\x00\x00\x01\x26\x01", 6) < p);
		}
		pp_ts_destroy(ts);
	}
}

static void test_invalid_input(void)
{
	pp_ts_config c;
	pp_ts *ts;
	uint8_t junk[64] = {1, 2, 3, 4, 5};
	uint8_t f[10] = {0};

	memset(&c, 0, sizeof c);
	c.sink = sink;
	c.video_codec = 99;
	CHECK(pp_ts_create(&c) == NULL);
	c.video_codec = PP_CODEC_H264;
	c.audio_codec = PP_AUDIO_OPUS;
	c.audio_channels = 6; /* the Opus descriptor only supports mono/stereo here */
	CHECK(pp_ts_create(&c) == NULL);
	c.audio_channels = 2;
	c.sink = NULL;
	CHECK(pp_ts_create(&c) == NULL);

	ts = mk(PP_CODEC_H264, PP_AUDIO_NONE, 0, 0, 0);
	CHECK_INT(pp_ts_write_video(ts, junk, sizeof junk, 0, 0, 1, 0), -1); /* no start code */
	CHECK_INT(pp_ts_write_video(ts, NULL, 0, 0, 0, 1, 0), -1);
	CHECK_INT(pp_ts_write_audio(ts, f, sizeof f, 0), -1); /* no audio configured */
	CHECK_INT(g_n, 0);
	pp_ts_destroy(ts);
}

int main(void)
{
	RUN(test_crc);
	RUN(test_tables);
	RUN(test_video_keyframe);
	RUN(test_param_set_cache);
	RUN(test_video_pts_dts_and_timing);
	RUN(test_hevc);
	RUN(test_opus);
	RUN(test_aac);
	RUN(test_message_framing_and_cc);
	RUN(test_flush_pads_with_null_packets);
	RUN(test_sei_timestamp);
	RUN(test_invalid_input);
	TEST_DONE();
}
