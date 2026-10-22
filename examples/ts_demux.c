#include "ts_demux.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
	int pid;
	uint8_t *buf;
	size_t n, cap;
	size_t want; /* bounded PES: total bytes expected incl. header, 0 = unbounded */
	int active;
} pes_acc;

struct tsd {
	tsd_callbacks cb;
	uint8_t carry[188 * 2];
	size_t carry_n;
	int pmt_pid;
	int have_pmt;
	int vpid[4], nv, vtype, apid, atype, achan, arate;
	int audio_ready; /* AAC: ADTS header seen */
	pes_acc v[4], a;
};

static const int aac_rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

tsd *tsd_create(const tsd_callbacks *cb)
{
	tsd *d = (tsd *)calloc(1, sizeof *d);
	if (d)
		d->cb = *cb;
	return d;
}

void tsd_destroy(tsd *d)
{
	if (!d)
		return;
	int i;
	for (i = 0; i < 4; i++)
		free(d->v[i].buf);
	free(d->a.buf);
	free(d);
}

int tsd_ready(const tsd *d)
{
	return d->have_pmt && (d->atype == TSD_AUDIO_NONE || d->atype == TSD_AUDIO_OPUS || d->audio_ready);
}
int tsd_video_codec(const tsd *d) { return d->vtype; }
int tsd_video_layers(const tsd *d) { return d->nv; }
int tsd_audio_codec(const tsd *d) { return d->atype; }
int tsd_audio_channels(const tsd *d) { return d->achan; }
int tsd_audio_rate(const tsd *d) { return d->arate; }

static int64_t rd_ts(const uint8_t *p)
{
	return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) | ((int64_t)(p[2] >> 1) << 15) |
	       ((int64_t)p[3] << 7) | (p[4] >> 1);
}

/* ---- Opus: TOC -> duration of the packet in 1/10 ms ---- */
static int opus_packet_duration_100us(const uint8_t *p, size_t n)
{
	int toc, cfg, frames, dur;
	if (n < 1)
		return 200;
	toc = p[0];
	cfg = toc >> 3;
	if (cfg < 12)
		dur = (cfg & 3) == 0 ? 100 : (cfg & 3) == 1 ? 200 : (cfg & 3) == 2 ? 400 : 600;
	else if (cfg < 16)
		dur = (cfg & 1) ? 200 : 100;
	else
		dur = (cfg & 3) == 0 ? 25 : (cfg & 3) == 1 ? 50 : (cfg & 3) == 2 ? 100 : 200;
	switch (toc & 3) {
	case 0: frames = 1; break;
	case 1:
	case 2: frames = 2; break;
	default: frames = n >= 2 ? (p[1] & 0x3F) : 1; break;
	}
	return dur * frames;
}

static void deliver_audio(tsd *d, const uint8_t *pes, size_t n)
{
	size_t hl, pos;
	int64_t pts;
	if (n < 14 || pes[0] || pes[1] || pes[2] != 1)
		return;
	if (!(pes[7] & 0x80))
		return;
	hl = 9 + pes[8];
	if (n < hl)
		return;
	pts = rd_ts(pes + 9);
	pos = hl;

	if (d->atype == TSD_AUDIO_OPUS) {
		int64_t t_us = pts * 100 / 9;
		while (pos + 3 <= n) {
			size_t sz = 0, k = pos + 2;
			if (pes[pos] != 0x7F || (pes[pos + 1] & 0xE0) != 0xE0)
				break;
			while (k < n && pes[k] == 0xFF) {
				sz += 255;
				k++;
			}
			if (k >= n)
				break;
			sz += pes[k++];
			if (pes[pos + 1] & 0x10)
				k += 2; /* start trim */
			if (pes[pos + 1] & 0x08)
				k += 2; /* end trim */
			if (pes[pos + 1] & 0x04) {
				if (k >= n)
					break;
				k += 1 + pes[k];
			}
			if (k + sz > n)
				break;
			d->cb.on_audio(pes + k, sz, t_us, d->cb.ud);
			t_us += (int64_t)opus_packet_duration_100us(pes + k, sz) * 100;
			pos = k + sz;
		}
	} else {
		int64_t t_us = pts * 100 / 9;
		while (pos + 7 <= n) {
			size_t fl, hdr;
			int rate;
			if (pes[pos] != 0xFF || (pes[pos + 1] & 0xF0) != 0xF0)
				break;
			hdr = (pes[pos + 1] & 1) ? 7 : 9;
			fl = ((size_t)(pes[pos + 3] & 3) << 11) | ((size_t)pes[pos + 4] << 3) | (pes[pos + 5] >> 5);
			if (fl < hdr || pos + fl > n)
				break;
			if (!d->audio_ready) {
				int sfi = (pes[pos + 2] >> 2) & 15;
				d->achan = ((pes[pos + 2] & 1) << 2) | (pes[pos + 3] >> 6);
				d->arate = sfi < 13 ? aac_rates[sfi] : 0;
				d->audio_ready = 1;
			}
			rate = d->arate ? d->arate : 48000;
			d->cb.on_audio(pes + pos + hdr, fl - hdr, t_us, d->cb.ud);
			t_us += 1024LL * 1000000LL / rate;
			pos += fl;
		}
	}
}

static void deliver_video(tsd *d, int layer, const uint8_t *pes, size_t n)
{
	size_t hl;
	int64_t pts, dts;
	int flags;
	if (n < 14 || pes[0] || pes[1] || pes[2] != 1)
		return;
	flags = pes[7] >> 6;
	if (!(flags & 2))
		return;
	hl = 9 + pes[8];
	if (n <= hl)
		return;
	pts = rd_ts(pes + 9);
	dts = (flags == 3 && n >= 19) ? rd_ts(pes + 14) : pts;
	d->cb.on_video(layer, pes + hl, n - hl, pts, dts, d->cb.ud);
}

static void acc_deliver(tsd *d, pes_acc *a, int video_layer)
{
	if (a->n) {
		if (video_layer >= 0)
			deliver_video(d, video_layer, a->buf, a->n);
		else
			deliver_audio(d, a->buf, a->n);
	}
	a->n = 0;
	a->want = 0;
	a->active = 0;
}

static void acc_add(pes_acc *a, const uint8_t *p, size_t n)
{
	if (a->n + n > a->cap) {
		size_t cap = a->cap ? a->cap : 65536;
		uint8_t *b;
		while (cap < a->n + n)
			cap *= 2;
		b = (uint8_t *)realloc(a->buf, cap);
		if (!b)
			return;
		a->buf = b;
		a->cap = cap;
	}
	memcpy(a->buf + a->n, p, n);
	a->n += n;
}

static void parse_pmt(tsd *d, const uint8_t *s, size_t n)
{
	size_t sl, pos, end, pil;
	if (n < 12 || s[0] != 0x02)
		return;
	sl = 3 + (((s[1] & 0x0F) << 8) | s[2]);
	if (sl > n)
		return;
	pil = ((s[10] & 0x0F) << 8) | s[11];
	pos = 12 + pil;
	end = sl - 4;
	memset(d->vpid, 0, sizeof d->vpid);
	d->nv = 0;
	d->apid = 0;
	d->vtype = d->atype = 0;
	while (pos + 5 <= end) {
		int st = s[pos], pid = ((s[pos + 1] & 0x1F) << 8) | s[pos + 2];
		size_t il = ((s[pos + 3] & 0x0F) << 8) | s[pos + 4], q = pos + 5, qe = pos + 5 + il;
		if (qe > end)
			break;
		if ((st == 0x1B || st == 0x24) && d->nv < 4 &&
		    (d->vtype == 0 || d->vtype == (st == 0x1B ? TSD_VIDEO_H264 : TSD_VIDEO_HEVC))) {
			d->vpid[d->nv++] = pid; /* simulcast layers: every video stream of the first codec, in PMT order */
			d->vtype = st == 0x1B ? TSD_VIDEO_H264 : TSD_VIDEO_HEVC;
		} else if (st == 0x0F && !d->apid) {
			d->apid = pid;
			d->atype = TSD_AUDIO_AAC;
		} else if (st == 0x06 && !d->apid) {
			int opus = 0, ch = 2;
			while (q + 2 <= qe) {
				int tag = s[q];
				size_t len = s[q + 1];
				if (q + 2 + len > qe)
					break;
				if (tag == 0x05 && len >= 4 && memcmp(s + q + 2, "Opus", 4) == 0)
					opus = 1;
				if (tag == 0x7F && len >= 2 && s[q + 2] == 0x80) {
					int code = s[q + 3];
					ch = code == 1 ? 1 : 2;
				}
				q += 2 + len;
			}
			if (opus) {
				d->apid = pid;
				d->atype = TSD_AUDIO_OPUS;
				d->achan = ch;
				d->arate = 48000;
			}
		}
		pos = qe;
	}
	d->have_pmt = d->nv > 0;
}

static void on_packet(tsd *d, const uint8_t *p)
{
	int pid = ((p[1] & 0x1F) << 8) | p[2], pusi = (p[1] & 0x40) != 0, afc = (p[3] >> 4) & 3;
	size_t off = 4;
	const uint8_t *pl;
	size_t pn;

	if (p[1] & 0x80)
		return;
	if (afc & 2)
		off += 1 + p[4];
	if (!(afc & 1) || off >= 188)
		return;
	pl = p + off;
	pn = 188 - off;

	if (pid == 0 && pusi) {
		size_t po = 1 + pl[0];
		if (po + 12 <= pn && pl[po] == 0x00) {
			/* first program entry that is not the NIT (program 0) */
			size_t sl = 3 + (((pl[po + 1] & 0x0F) << 8) | pl[po + 2]), e = po + sl - 4, q = po + 8;
			while (q + 4 <= e && q + 4 <= pn) {
				int prog = (pl[q] << 8) | pl[q + 1];
				if (prog != 0) {
					d->pmt_pid = ((pl[q + 2] & 0x1F) << 8) | pl[q + 3];
					break;
				}
				q += 4;
			}
		}
		return;
	}
	if (d->pmt_pid && pid == d->pmt_pid && pusi && !d->have_pmt) {
		size_t po = 1 + pl[0];
		if (po < pn)
			parse_pmt(d, pl + po, pn - po);
		return;
	}
	if (!d->have_pmt)
		return;
	{
		pes_acc *a = NULL;
		int layer = -1, i;
		for (i = 0; i < d->nv; i++)
			if (pid == d->vpid[i]) {
				a = &d->v[i];
				layer = i;
			}
		if (!a && pid == d->apid)
			a = &d->a;
		if (!a)
			return;
		if (pusi) {
			acc_deliver(d, a, layer);
			a->active = 1;
			if (pn >= 6 && pl[0] == 0 && pl[1] == 0 && pl[2] == 1) {
				size_t plen = ((size_t)pl[4] << 8) | pl[5];
				a->want = plen ? plen + 6 : 0;
			}
		}
		if (!a->active)
			return;
		acc_add(a, pl, pn);
		if (a->want && a->n >= a->want) {
			a->n = a->want;
			acc_deliver(d, a, layer);
		}
	}
}

void tsd_feed(tsd *d, const uint8_t *data, size_t n)
{
	while (n) {
		if (d->carry_n) {
			size_t take = 188 - d->carry_n;
			if (take > n)
				take = n;
			memcpy(d->carry + d->carry_n, data, take);
			d->carry_n += take;
			data += take;
			n -= take;
			if (d->carry_n == 188) {
				if (d->carry[0] == 0x47)
					on_packet(d, d->carry);
				d->carry_n = 0;
			}
			continue;
		}
		if (data[0] != 0x47) { /* resync */
			data++;
			n--;
			continue;
		}
		if (n >= 188) {
			if (n >= 189 && data[188] != 0x47) {
				data++;
				n--;
				continue;
			}
			on_packet(d, data);
			data += 188;
			n -= 188;
		} else {
			memcpy(d->carry, data, n);
			d->carry_n = n;
			n = 0;
		}
	}
}

void tsd_flush(tsd *d)
{
	int i;
	for (i = 0; i < d->nv; i++)
		acc_deliver(d, &d->v[i], i);
	acc_deliver(d, &d->a, -1);
}
