#include "pp_ts.h"
#include "pp_annexb.h"
#include "pp_streamid.h"

#include <stdlib.h>
#include <string.h>

#define PS_MAX 512
#define TS_BASE_OFFSET 90000LL /* start timestamps 1 s in, so PCR = DTS - 0.1 s never goes negative */
#define PCR_LEAD 9000LL        /* same as mediacommon's dtsPCRDiff */
#define PSI_INTERVAL 45000LL   /* 500 ms at 90 kHz */

typedef struct {
	uint8_t d[PS_MAX];
	size_t n;
} paramset;

struct pp_ts {
	pp_ts_config cfg;
	uint8_t msg[PP_TS_MSG_SIZE];
	size_t msg_n; /* bytes used in msg */
	uint8_t cc_pat, cc_pmt, cc_video, cc_audio;
	int64_t last_psi;
	int have_psi;
	paramset vps, sps, pps;
	uint8_t *au;
	size_t au_cap;
};

typedef struct {
	const uint8_t *a;
	size_t an;
	const uint8_t *b;
	size_t bn;
	size_t off;
} src_t;

static const int aac_rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

uint32_t pp_crc32_mpeg(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xFFFFFFFFu;
	size_t i;
	int k;
	for (i = 0; i < n; i++) {
		crc ^= (uint32_t)p[i] << 24;
		for (k = 0; k < 8; k++)
			crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
	}
	return crc;
}

pp_ts *pp_ts_create(const pp_ts_config *cfg)
{
	pp_ts *ts;
	if (!cfg || !cfg->sink || (cfg->video_codec != PP_CODEC_H264 && cfg->video_codec != PP_CODEC_HEVC))
		return NULL;
	if (cfg->audio_codec == PP_AUDIO_OPUS && cfg->audio_channels != 1 && cfg->audio_channels != 2)
		return NULL;
	ts = (pp_ts *)calloc(1, sizeof *ts);
	if (!ts)
		return NULL;
	ts->cfg = *cfg;
	return ts;
}

void pp_ts_destroy(pp_ts *ts)
{
	if (!ts)
		return;
	free(ts->au);
	free(ts);
}

static void emit_packet(pp_ts *ts, const uint8_t *pkt)
{
	memcpy(ts->msg + ts->msg_n, pkt, PP_TS_PACKET);
	ts->msg_n += PP_TS_PACKET;
	if (ts->msg_n == PP_TS_MSG_SIZE) {
		ts->cfg.sink(ts->msg, PP_TS_MSG_SIZE, ts->cfg.ud);
		ts->msg_n = 0;
	}
}

size_t pp_ts_pending(const pp_ts *ts)
{
	return ts->msg_n;
}

void pp_ts_flush(pp_ts *ts)
{
	uint8_t pkt[PP_TS_PACKET];
	if (ts->msg_n == 0)
		return;
	memset(pkt, 0xFF, sizeof pkt);
	pkt[0] = 0x47;
	pkt[1] = 0x1F;
	pkt[2] = 0xFF;
	pkt[3] = 0x10; /* null packet, payload only */
	while (ts->msg_n != 0)
		emit_packet(ts, pkt);
}

static void src_read(src_t *s, uint8_t *dst, size_t n)
{
	while (n) {
		size_t take;
		if (s->off < s->an) {
			take = s->an - s->off;
			if (take > n)
				take = n;
			memcpy(dst, s->a + s->off, take);
		} else {
			size_t boff = s->off - s->an;
			take = s->bn - boff;
			if (take > n)
				take = n;
			memcpy(dst, s->b + boff, take);
		}
		dst += take;
		s->off += take;
		n -= take;
	}
}

/* Split `src` into TS packets on `pid`. pcr_base < 0: no PCR. */
static void packetize(pp_ts *ts, int pid, uint8_t *cc, src_t *src, int64_t pcr_base, int rai)
{
	size_t remaining = src->an + src->bn;
	int first = 1;

	while (remaining > 0) {
		uint8_t pkt[PP_TS_PACKET];
		size_t af_total = 0, cap, pay, stuff;
		int has_pcr = first && pcr_base >= 0;
		int has_rai = first && rai;

		if (has_pcr || has_rai)
			af_total = 2 + (has_pcr ? 6 : 0); /* length byte + flags (+ PCR) */
		cap = 184 - af_total;
		if (remaining >= cap) {
			pay = cap;
			stuff = 0;
		} else {
			pay = remaining;
			stuff = cap - pay;
		}
		if (stuff) {
			if (af_total == 0) {
				af_total = stuff; /* length byte + flags + 0xFF fill (or a lone zero length) */
			} else {
				af_total += stuff;
			}
		}

		pkt[0] = 0x47;
		pkt[1] = (uint8_t)((first ? 0x40 : 0) | ((pid >> 8) & 0x1F));
		pkt[2] = (uint8_t)(pid & 0xFF);
		pkt[3] = (uint8_t)((af_total ? 0x30 : 0x10) | (*cc & 0x0F));
		*cc = (uint8_t)((*cc + 1) & 0x0F);

		if (af_total) {
			size_t p = 5;
			pkt[4] = (uint8_t)(af_total - 1);
			if (af_total > 1) {
				pkt[p++] = (uint8_t)((has_rai ? 0x40 : 0) | (has_pcr ? 0x10 : 0));
				if (has_pcr) {
					uint64_t b = (uint64_t)pcr_base & 0x1FFFFFFFFULL;
					pkt[p++] = (uint8_t)(b >> 25);
					pkt[p++] = (uint8_t)(b >> 17);
					pkt[p++] = (uint8_t)(b >> 9);
					pkt[p++] = (uint8_t)(b >> 1);
					pkt[p++] = (uint8_t)(((b & 1) << 7) | 0x7E);
					pkt[p++] = 0;
				}
				while (p < 4 + af_total)
					pkt[p++] = 0xFF;
			}
		}
		src_read(src, pkt + 4 + af_total, pay);
		remaining -= pay;
		first = 0;
		emit_packet(ts, pkt);
	}
}

static void write_psi(pp_ts *ts, int pid, uint8_t *cc, const uint8_t *section, size_t n)
{
	uint8_t pkt[PP_TS_PACKET];
	pkt[0] = 0x47;
	pkt[1] = (uint8_t)(0x40 | ((pid >> 8) & 0x1F));
	pkt[2] = (uint8_t)(pid & 0xFF);
	pkt[3] = (uint8_t)(0x10 | (*cc & 0x0F));
	*cc = (uint8_t)((*cc + 1) & 0x0F);
	pkt[4] = 0; /* pointer_field */
	memset(pkt + 5, 0xFF, 183);
	memcpy(pkt + 5, section, n);
	emit_packet(ts, pkt);
}

int pp_ts_write_tables(pp_ts *ts)
{
	uint8_t sec[64];
	size_t n;
	uint32_t crc;

	/* PAT: one program (1) -> PMT PID */
	n = 0;
	sec[n++] = 0x00;
	sec[n++] = 0xB0;
	sec[n++] = 13;
	sec[n++] = 0x00;
	sec[n++] = 0x01; /* transport_stream_id */
	sec[n++] = 0xC1; /* version 0, current */
	sec[n++] = 0x00;
	sec[n++] = 0x00;
	sec[n++] = 0x00;
	sec[n++] = 0x01; /* program_number */
	sec[n++] = (uint8_t)(0xE0 | (PP_PID_PMT >> 8));
	sec[n++] = (uint8_t)(PP_PID_PMT & 0xFF);
	crc = pp_crc32_mpeg(sec, n);
	sec[n++] = (uint8_t)(crc >> 24);
	sec[n++] = (uint8_t)(crc >> 16);
	sec[n++] = (uint8_t)(crc >> 8);
	sec[n++] = (uint8_t)crc;
	write_psi(ts, 0, &ts->cc_pat, sec, n);

	/* PMT */
	{
		uint8_t pmt[128];
		size_t es_len_pos, len_pos;
		n = 0;
		pmt[n++] = 0x02;
		len_pos = n;
		pmt[n++] = 0xB0;
		pmt[n++] = 0; /* section_length, patched below */
		pmt[n++] = 0x00;
		pmt[n++] = 0x01; /* program_number */
		pmt[n++] = 0xC1;
		pmt[n++] = 0x00;
		pmt[n++] = 0x00;
		pmt[n++] = (uint8_t)(0xE0 | (PP_PID_VIDEO >> 8)); /* PCR PID = video */
		pmt[n++] = (uint8_t)(PP_PID_VIDEO & 0xFF);
		pmt[n++] = 0xF0;
		pmt[n++] = 0x00; /* program_info_length */

		pmt[n++] = ts->cfg.video_codec == PP_CODEC_HEVC ? 0x24 : 0x1B;
		pmt[n++] = (uint8_t)(0xE0 | (PP_PID_VIDEO >> 8));
		pmt[n++] = (uint8_t)(PP_PID_VIDEO & 0xFF);
		pmt[n++] = 0xF0;
		pmt[n++] = 0x00;

		if (ts->cfg.audio_codec == PP_AUDIO_OPUS) {
			pmt[n++] = 0x06; /* private data */
			pmt[n++] = (uint8_t)(0xE0 | (PP_PID_AUDIO >> 8));
			pmt[n++] = (uint8_t)(PP_PID_AUDIO & 0xFF);
			pmt[n++] = 0xF0;
			es_len_pos = n;
			pmt[n++] = 0;
			/* registration_descriptor: "Opus" */
			pmt[n++] = 0x05;
			pmt[n++] = 4;
			pmt[n++] = 'O';
			pmt[n++] = 'p';
			pmt[n++] = 'u';
			pmt[n++] = 's';
			/* extension_descriptor: opus_audio_descriptor (ext tag 0x80, channel_config_code) */
			pmt[n++] = 0x7F;
			pmt[n++] = 2;
			pmt[n++] = 0x80;
			pmt[n++] = (uint8_t)ts->cfg.audio_channels; /* 1 mono, 2 stereo */
			pmt[es_len_pos] = (uint8_t)(n - es_len_pos - 1);
		} else if (ts->cfg.audio_codec == PP_AUDIO_AAC) {
			pmt[n++] = 0x0F; /* AAC ADTS */
			pmt[n++] = (uint8_t)(0xE0 | (PP_PID_AUDIO >> 8));
			pmt[n++] = (uint8_t)(PP_PID_AUDIO & 0xFF);
			pmt[n++] = 0xF0;
			pmt[n++] = 0x00;
		}
		{
			size_t sl = n - 3 + 4; /* bytes after the length field, incl. CRC */
			pmt[len_pos + 1] = (uint8_t)(sl);
			pmt[len_pos] = (uint8_t)(0xB0 | ((sl >> 8) & 0x0F));
		}
		crc = pp_crc32_mpeg(pmt, n);
		pmt[n++] = (uint8_t)(crc >> 24);
		pmt[n++] = (uint8_t)(crc >> 16);
		pmt[n++] = (uint8_t)(crc >> 8);
		pmt[n++] = (uint8_t)crc;
		write_psi(ts, PP_PID_PMT, &ts->cc_pmt, pmt, n);
	}
	return 0;
}

static void maybe_psi(pp_ts *ts, int64_t t90, int force)
{
	if (force || !ts->have_psi || t90 - ts->last_psi >= PSI_INTERVAL || t90 < ts->last_psi) {
		pp_ts_write_tables(ts);
		ts->have_psi = 1;
		ts->last_psi = t90;
	}
}

static size_t put_ts(uint8_t *p, int prefix, int64_t t)
{
	uint64_t v = (uint64_t)t & 0x1FFFFFFFFULL;
	p[0] = (uint8_t)((prefix << 4) | (((v >> 30) & 7) << 1) | 1);
	p[1] = (uint8_t)(v >> 22);
	p[2] = (uint8_t)((((v >> 15) & 0x7F) << 1) | 1);
	p[3] = (uint8_t)(v >> 7);
	p[4] = (uint8_t)(((v & 0x7F) << 1) | 1);
	return 5;
}

static int au_reserve(pp_ts *ts, size_t need)
{
	if (need <= ts->au_cap)
		return 0;
	{
		size_t cap = ts->au_cap ? ts->au_cap : 64 * 1024;
		uint8_t *p;
		while (cap < need)
			cap *= 2;
		p = (uint8_t *)realloc(ts->au, cap);
		if (!p)
			return -1;
		ts->au = p;
		ts->au_cap = cap;
	}
	return 0;
}

static size_t au_nal(uint8_t *dst, const uint8_t *nal, size_t n)
{
	dst[0] = 0;
	dst[1] = 0;
	dst[2] = 0;
	dst[3] = 1;
	memcpy(dst + 4, nal, n);
	return 4 + n;
}

static void cache_ps(paramset *ps, const uint8_t *nal, size_t n)
{
	if (n <= PS_MAX) {
		memcpy(ps->d, nal, n);
		ps->n = n;
	}
}

/* user_data_unregistered SEI NAL carrying OBS-ABSTS-SEI-v1 + big-endian ms. Returns NAL length (no start code). */
static size_t build_ts_sei(int codec, int64_t wall_ms, uint8_t *out)
{
	static const uint8_t uuid[16] = {'O', 'B', 'S', '-', 'A', 'B', 'S', 'T', 'S', '-', 'S', 'E', 'I', '-', 'v', '1'};
	uint8_t rbsp[2 + 24 + 1];
	uint8_t esc[2 * sizeof rbsp];
	size_t n = 0, hl = 0;
	int en, i;
	uint64_t ms = (uint64_t)wall_ms;

	rbsp[n++] = 5;
	rbsp[n++] = 24;
	memcpy(rbsp + n, uuid, 16);
	n += 16;
	for (i = 0; i < 8; i++)
		rbsp[n++] = (uint8_t)(ms >> (8 * (7 - i)));
	rbsp[n++] = 0x80;
	en = pp_epb_escape(rbsp, n, esc, sizeof esc);
	if (codec == PP_CODEC_HEVC) {
		out[hl++] = 40 << 1; /* suffix SEI, layer 0 */
		out[hl++] = 1;       /* temporal id plus1 */
	} else {
		out[hl++] = 0x06;
	}
	memcpy(out + hl, esc, (size_t)en);
	return hl + (size_t)en;
}

int pp_ts_write_video(pp_ts *ts, const uint8_t *buf, size_t len, int64_t pts90, int64_t dts90, int is_key,
		      int64_t wall_ms)
{
	int codec = ts->cfg.video_codec;
	size_t pos = 0, nlen, out = 0, sei_len = 0, sei_at = (size_t)-1;
	const uint8_t *nal;
	int have_vps = 0, have_sps = 0, have_pps = 0, any = 0;
	uint8_t sei[64];
	uint8_t hdr[19];
	size_t hl = 0, need;
	src_t src;
	int64_t pts, dts;

	if (!buf || len == 0)
		return -1;

	/* pass 1: learn parameter sets, see what the AU already carries */
	while (pp_annexb_next(buf, len, &pos, &nal, &nlen)) {
		int type = pp_nal_type(codec, nal[0]);
		int kind = pp_nal_kind(codec, type);
		any = 1;
		if (kind == PP_NAL_VPS && codec == PP_CODEC_HEVC) {
			have_vps = 1;
			cache_ps(&ts->vps, nal, nlen);
		} else if (kind == PP_NAL_SPS) {
			have_sps = 1;
			cache_ps(&ts->sps, nal, nlen);
		} else if (kind == PP_NAL_PPS) {
			have_pps = 1;
			cache_ps(&ts->pps, nal, nlen);
		}
	}
	if (!any)
		return -1;

	if (ts->cfg.sei_timestamp)
		sei_len = build_ts_sei(codec, wall_ms, sei);

	need = len + 3 * 64 + 3 * PS_MAX + sei_len + 64 + (len / 2) + 64; /* generous: start codes grow 3 -> 4 */
	if (au_reserve(ts, need) != 0)
		return -2;

	/* pass 2: AUD, missing parameter sets (keyframes), original NALs, timestamp SEI */
	if (codec == PP_CODEC_HEVC) {
		static const uint8_t aud[] = {0x46, 0x01, 0x50};
		out += au_nal(ts->au + out, aud, sizeof aud);
	} else {
		static const uint8_t aud[] = {0x09, 0xF0};
		out += au_nal(ts->au + out, aud, sizeof aud);
	}
	if (is_key) {
		if (codec == PP_CODEC_HEVC && !have_vps && ts->vps.n)
			out += au_nal(ts->au + out, ts->vps.d, ts->vps.n);
		if (!have_sps && ts->sps.n)
			out += au_nal(ts->au + out, ts->sps.d, ts->sps.n);
		if (!have_pps && ts->pps.n)
			out += au_nal(ts->au + out, ts->pps.d, ts->pps.n);
	}
	pos = 0;
	while (pp_annexb_next(buf, len, &pos, &nal, &nlen)) {
		int type = pp_nal_type(codec, nal[0]);
		int kind = pp_nal_kind(codec, type);
		if (kind == PP_NAL_AUD)
			continue;
		if (sei_len && codec == PP_CODEC_H264 && sei_at == (size_t)-1 && pp_nal_is_vcl(codec, type)) {
			sei_at = out; /* AVC has no suffix SEI: must precede the first slice */
			out += au_nal(ts->au + out, sei, sei_len);
		}
		out += au_nal(ts->au + out, nal, nlen);
	}
	if (sei_len && sei_at == (size_t)-1)
		out += au_nal(ts->au + out, sei, sei_len);

	maybe_psi(ts, pts90, is_key);

	pts = pts90 + TS_BASE_OFFSET;
	dts = dts90 + TS_BASE_OFFSET;
	hdr[hl++] = 0;
	hdr[hl++] = 0;
	hdr[hl++] = 1;
	hdr[hl++] = 0xE0;
	hdr[hl++] = 0; /* PES_packet_length: unbounded */
	hdr[hl++] = 0;
	hdr[hl++] = 0x84; /* '10', data_alignment_indicator */
	if (dts90 == pts90) {
		hdr[hl++] = 0x80;
		hdr[hl++] = 5;
		hl += put_ts(hdr + hl, 2, pts);
	} else {
		hdr[hl++] = 0xC0;
		hdr[hl++] = 10;
		hl += put_ts(hdr + hl, 3, pts);
		hl += put_ts(hdr + hl, 1, dts);
	}
	src.a = hdr;
	src.an = hl;
	src.b = ts->au;
	src.bn = out;
	src.off = 0;
	packetize(ts, PP_PID_VIDEO, &ts->cc_video, &src, dts - PCR_LEAD, is_key);
	pp_ts_flush(ts);
	return 0;
}

int pp_ts_write_audio(pp_ts *ts, const uint8_t *frame, size_t len, int64_t pts90)
{
	uint8_t hdr[32];
	uint8_t pre[16];
	size_t hl = 0, pl = 0, total;
	src_t src;
	int64_t pts = pts90 + TS_BASE_OFFSET;

	if (!frame || len == 0 || ts->cfg.audio_codec == PP_AUDIO_NONE)
		return -1;

	if (ts->cfg.audio_codec == PP_AUDIO_OPUS) {
		size_t k = len / 255;
		if (k + 2 + 1 > sizeof pre)
			return -1; /* > ~3.5 KB opus packet: not valid */
		pre[pl++] = 0x7F;
		pre[pl++] = 0xE0;
		while (k--)
			pre[pl++] = 0xFF;
		pre[pl++] = (uint8_t)(len % 255);
	} else {
		int sfi = -1, i;
		size_t flen = len + 7;
		int ch = ts->cfg.audio_channels > 0 ? ts->cfg.audio_channels : 2;
		for (i = 0; i < (int)(sizeof aac_rates / sizeof aac_rates[0]); i++)
			if (aac_rates[i] == ts->cfg.audio_sample_rate)
				sfi = i;
		if (sfi < 0 || flen > 0x1FFF)
			return -1;
		pre[pl++] = 0xFF;
		pre[pl++] = 0xF1;
		pre[pl++] = (uint8_t)((1 << 6) | (sfi << 2) | ((ch >> 2) & 1)); /* AAC-LC */
		pre[pl++] = (uint8_t)(((ch & 3) << 6) | ((flen >> 11) & 3));
		pre[pl++] = (uint8_t)((flen >> 3) & 0xFF);
		pre[pl++] = (uint8_t)(((flen & 7) << 5) | 0x1F);
		pre[pl++] = 0xFC;
	}
	total = pl + len;
	if (total + 8 > 0xFFFF)
		return -1;

	maybe_psi(ts, pts90, 0);

	hdr[hl++] = 0;
	hdr[hl++] = 0;
	hdr[hl++] = 1;
	hdr[hl++] = ts->cfg.audio_codec == PP_AUDIO_OPUS ? 0xBD : 0xC0;
	hdr[hl++] = (uint8_t)((total + 8) >> 8); /* flags(2) + hdr_len(1) + PTS(5) + payload */
	hdr[hl++] = (uint8_t)((total + 8) & 0xFF);
	hdr[hl++] = 0x84;
	hdr[hl++] = 0x80; /* PTS only: Opus requires PTS == DTS */
	hdr[hl++] = 5;
	hl += put_ts(hdr + hl, 2, pts);

	/* header + control/ADTS prefix are contiguous in one small buffer */
	memcpy(hdr + hl, pre, pl);
	hl += pl;
	src.a = hdr;
	src.an = hl;
	src.b = frame;
	src.bn = len;
	src.off = 0;
	packetize(ts, PP_PID_AUDIO, &ts->cc_audio, &src, -1, 1);
	return 0;
}
