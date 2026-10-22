/*
 * ppusher-cli: feed libppusher from an MPEG-TS source (stdin, UDP or a file).
 * It demuxes the TS into H.264/HEVC access units and Opus/AAC frames and
 * pushes them through the public API, i.e. the same path a hardware encoder
 * would use. For bring-up and interop testing.
 *
 *   ffmpeg -re -i in.mp4 -c:v libx264 -bf 0 -g 60 -c:a libopus -f mpegts - | ppusher-cli --app-id X --stream Y
 */
#include "ppusher/ppusher.h"
#include "ts_demux.h"

#include "pp_platform.h"
#include "pp_ts.h"
#include "pp_streamid.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define closesocket close
#define INVALID_SOCKET (-1)
#endif

typedef struct {
	const char *ppcenter, *app_id, *stream, *region, *secret_env, *secret_file;
	const char *input, *dry_run, *ca_file, *passphrase;
	const char *direct, *direct_token, *direct_ws, *direct_ws_token;
	int codec; /* 0 = detect */
	const char *bitrate_list; /* "layer0,layer1,..." nominal kbps */
	int latency_ms, no_degrade, sei, no_codec_path, insecure, verbose, loop, realtime;
} opts_t;

typedef struct {
	opts_t o;
	ppusher_t *p;
	FILE *dry;
	pp_ts *dts_mux;
	int64_t first_pts_us, start_wall_ms, loop_offset_us, last_video_us;
	int have_first, warned_bframes, have_raw_first, nlayers;
	int64_t raw_first_us, raw_last_us;
	uint64_t video_n, audio_n;
} app_t;

static volatile int g_quit;

static void on_sigint(int sig)
{
	(void)sig;
	g_quit = 1;
}

static void noop_video(int layer, const uint8_t *au, size_t len, int64_t pts90, int64_t dts90, void *ud)
{
	(void)layer;
	(void)au; (void)len; (void)pts90; (void)dts90; (void)ud;
}

static void noop_audio(const uint8_t *f, size_t len, int64_t pts_us, void *ud)
{
	(void)f; (void)len; (void)pts_us; (void)ud;
}

static void on_log(int lvl, const char *msg, void *ud)
{
	static const char *const n[] = {"ERROR", "WARN", "INFO", "DEBUG"};
	opts_t *o = (opts_t *)ud;
	if (lvl <= PPUSHER_LOG_INFO || o->verbose)
		fprintf(stderr, "[%s] %s\n", n[lvl < 4 ? lvl : 3], msg);
}

static void on_state(int st, const char *info, void *ud)
{
	static const char *const n[] = {"idle", "fetching", "connecting", "streaming", "backoff", "FATAL", "stopped"};
	(void)ud;
	fprintf(stderr, "[state] %s %s\n", st >= 0 && st < 7 ? n[st] : "?", info);
}

static void on_bitrate(int layer, int kbps, void *ud)
{
	(void)ud;
	fprintf(stderr, "[degrade] a real encoder would now set layer %d to %d kbps (this CLI replays a fixed file/stream)\n",
		layer, kbps);
}

static void on_layers(int first, int count, void *ud)
{
	(void)ud;
	fprintf(stderr, "[layers] publishing layers %d..%d (%d active)\n", first, first + count - 1, count);
}

static void on_keyframe(void *ud)
{
	(void)ud;
	fprintf(stderr, "[keyframe] requested (waiting for the next IDR in the input)\n");
}

/* ---------------------------------------------------------------- pacing */

static void pace(app_t *a, int64_t pts_us)
{
	int64_t target, now;
	if (!a->o.realtime)
		return;
	if (!a->have_first) {
		a->have_first = 1;
		a->first_pts_us = pts_us;
		a->start_wall_ms = pp_now_ms();
	}
	target = a->start_wall_ms + (pts_us - a->first_pts_us) / 1000;
	for (;;) {
		now = pp_now_ms();
		if (now >= target || g_quit)
			break;
		pp_sleep_ms((int)(target - now > 20 ? 20 : target - now));
	}
}

/* ---------------------------------------------------------------- demux callbacks */

static int is_key_au(int codec, const uint8_t *au, size_t n)
{
	size_t i;
	for (i = 0; i + 4 < n; i++) {
		if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
			int t = codec == PPUSHER_CODEC_HEVC ? ((au[i + 3] >> 1) & 0x3F) : (au[i + 3] & 0x1F);
			if (codec == PPUSHER_CODEC_HEVC ? (t >= 16 && t <= 21) : t == 5)
				return 1;
			i += 2;
		}
	}
	return 0;
}

static void cb_video(int layer, const uint8_t *au, size_t len, int64_t pts90, int64_t dts90, void *ud)
{
	app_t *a = (app_t *)ud;
	int64_t raw = dts90 * 100 / 9;
	int64_t us = raw + a->loop_offset_us;
	int key = is_key_au(a->o.codec, au, len);

	if (layer >= a->nlayers)
		return; /* more streams than the codec's ladder limit */

	if (pts90 != dts90 && !a->warned_bframes) {
		a->warned_bframes = 1;
		fprintf(stderr, "[warn] input has B-frames (PTS != DTS); libppusher needs B-frames = 0 - using DTS. "
				"Re-encode with -bf 0.\n");
	}
	if (layer == 0) {
		if (!a->have_raw_first) {
			a->have_raw_first = 1;
			a->raw_first_us = raw;
		}
		a->raw_last_us = raw;
		a->video_n++;
		pace(a, us); /* one pacing point per frame: layers of a frame share its timestamp */
	}
	a->last_video_us = us;
	if (a->dts_mux) {
		pp_ts_write_video_layer(a->dts_mux, layer, au, len, us * 9 / 100, us * 9 / 100, key, 0, layer == a->nlayers - 1);
	} else {
		int rc = ppusher_push_video(a->p, layer, au, len, us, us, key);
		if (a->o.verbose && rc != PPUSHER_OK && rc != PPUSHER_E_NOT_CONNECTED)
			fprintf(stderr, "[push_video] rc=%d\n", rc);
	}
}

static void cb_audio(const uint8_t *fr, size_t len, int64_t pts_us, void *ud)
{
	app_t *a = (app_t *)ud;
	int64_t us = pts_us + a->loop_offset_us;
	a->audio_n++;
	if (a->dts_mux)
		pp_ts_write_audio(a->dts_mux, fr, len, us * 9 / 100);
	else
		ppusher_push_audio(a->p, fr, len, us);
}

static void dry_sink(const uint8_t *m, size_t n, void *ud)
{
	fwrite(m, 1, n, ((app_t *)ud)->dry);
}

/* ---------------------------------------------------------------- input */

typedef struct {
	int kind; /* 0 stdin, 1 file, 2 udp */
	FILE *f;
	sock_t s;
	const char *path;
} input_t;

static int input_open(input_t *in, const char *spec)
{
	memset(in, 0, sizeof *in);
	if (strncmp(spec, "ts:", 3) == 0)
		spec += 3;
	if (strcmp(spec, "-") == 0) {
		in->kind = 0;
		in->f = stdin;
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
#endif
		return 0;
	}
	if (strncmp(spec, "udp://", 6) == 0) {
		char host[128], *colon;
		struct sockaddr_in sa;
		int reuse = 1;
#ifdef _WIN32
		WSADATA w;
		WSAStartup(MAKEWORD(2, 2), &w);
#endif
		snprintf(host, sizeof host, "%s", spec + 6);
		colon = strrchr(host, ':');
		if (!colon)
			return -1;
		*colon = 0;
		memset(&sa, 0, sizeof sa);
		sa.sin_family = AF_INET;
		sa.sin_port = htons((unsigned short)atoi(colon + 1));
		sa.sin_addr.s_addr = (!host[0] || strcmp(host, "0.0.0.0") == 0) ? htonl(INADDR_ANY) : inet_addr(host);
		in->s = socket(AF_INET, SOCK_DGRAM, 0);
		if (in->s == INVALID_SOCKET)
			return -1;
		setsockopt(in->s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof reuse);
		if (bind(in->s, (struct sockaddr *)&sa, sizeof sa) != 0) {
			closesocket(in->s);
			return -1;
		}
		in->kind = 2;
		return 0;
	}
	in->kind = 1;
	in->path = spec;
	in->f = fopen(spec, "rb");
	return in->f ? 0 : -1;
}

/* >0 bytes, 0 = end of input */
static int input_read(input_t *in, uint8_t *buf, size_t cap)
{
	if (in->kind == 2) {
		int r = recv(in->s, (char *)buf, (int)cap, 0);
		return r > 0 ? r : 0;
	}
	return (int)fread(buf, 1, cap, in->f);
}

/* ---------------------------------------------------------------- main */

static void usage(void)
{
	fputs("ppusher-cli " PPUSHER_VERSION " - push an MPEG-TS source to PPCDN over SRT\n\n"
	      "  --app-id ID --stream NAME        PPCDN app id and stream name (no /h264|/hevc suffix)\n"
	      "  --ppcenter URL                   default https://api.pp-cdn.org\n"
	      "  --secret-env VAR                 env var holding appSecret (default PPUSHER_APP_SECRET)\n"
	      "  --secret-file FILE               read appSecret from a file (first line)\n"
	      "  --region R                       requestRegion\n"
	      "  --input SPEC                     ts:-  (stdin, default) | ts:udp://0.0.0.0:5000 | ts:FILE\n"
	      "  --loop                           repeat a file input forever\n"
	      "  --realtime / --no-realtime       pace by timestamps (default: on for files only)\n"
	      "  --codec h264|hevc                default: detect from the PMT\n"
	      "  --bitrate-kbps A[,B,..]          nominal per-layer encoder bitrate (layer 0 first), used to print degrade targets\n"
	      "  --no-degrade                     do not open the ppmmx degrade WebSocket\n"
	      "  --no-codec-path                  publish on the bare <app>/<stream> path (pplayer can't play it)\n"
	      "  --sei                            inject the OBS-ABSTS-SEI-v1 timestamp into every video AU\n"
	      "  --passphrase P  --latency MS     SRT encryption / latency\n"
	      "  --ca-file F  --insecure          TLS CA bundle / skip certificate checks (testing only)\n"
	      "  --direct HOST:PORT [--direct-token T] [--direct-ws URL --direct-ws-token T]\n"
	      "                                   skip ppcenter, publish straight to an ppmmx SRT listener\n"
	      "  --dry-run FILE                   no network: write the muxed TS to FILE\n"
	      "  -v                               verbose\n",
	      stderr);
}

static const char *arg(int argc, char **argv, int *i)
{
	if (*i + 1 >= argc) {
		fprintf(stderr, "missing value for %s\n", argv[*i]);
		exit(2);
	}
	return argv[++*i];
}

int main(int argc, char **argv)
{
	app_t app;
	opts_t *o = &app.o;
	int i, rc = 0;
	input_t in;
	tsd *d;
	tsd_callbacks cb;
	uint8_t *buf;
	char secret[256] = "";
	ppusher_config_t cfg;
	ppusher_callbacks_t pcb;
	int vcodec, acodec, ach = 2, arate = 48000, nlayers = 1, li;
	uint8_t *pre = NULL;
	size_t pre_n = 0, pre_cap = 0;

	memset(&app, 0, sizeof app);
	o->ppcenter = "https://api.pp-cdn.org";
	o->secret_env = "PPUSHER_APP_SECRET";
	o->input = "ts:-";
	o->realtime = -1;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--app-id")) o->app_id = arg(argc, argv, &i);
		else if (!strcmp(a, "--stream")) o->stream = arg(argc, argv, &i);
		else if (!strcmp(a, "--ppcenter")) o->ppcenter = arg(argc, argv, &i);
		else if (!strcmp(a, "--secret-env")) o->secret_env = arg(argc, argv, &i);
		else if (!strcmp(a, "--secret-file")) o->secret_file = arg(argc, argv, &i);
		else if (!strcmp(a, "--region")) o->region = arg(argc, argv, &i);
		else if (!strcmp(a, "--input")) o->input = arg(argc, argv, &i);
		else if (!strcmp(a, "--loop")) o->loop = 1;
		else if (!strcmp(a, "--realtime")) o->realtime = 1;
		else if (!strcmp(a, "--no-realtime")) o->realtime = 0;
		else if (!strcmp(a, "--codec")) {
			const char *v = arg(argc, argv, &i);
			o->codec = !strcmp(v, "hevc") ? PPUSHER_CODEC_HEVC : !strcmp(v, "h264") ? PPUSHER_CODEC_H264 : 0;
		} else if (!strcmp(a, "--bitrate-kbps")) o->bitrate_list = arg(argc, argv, &i);
		else if (!strcmp(a, "--no-degrade")) o->no_degrade = 1;
		else if (!strcmp(a, "--no-codec-path")) o->no_codec_path = 1;
		else if (!strcmp(a, "--sei")) o->sei = 1;
		else if (!strcmp(a, "--passphrase")) o->passphrase = arg(argc, argv, &i);
		else if (!strcmp(a, "--latency")) o->latency_ms = atoi(arg(argc, argv, &i));
		else if (!strcmp(a, "--ca-file")) o->ca_file = arg(argc, argv, &i);
		else if (!strcmp(a, "--insecure")) o->insecure = 1;
		else if (!strcmp(a, "--direct")) o->direct = arg(argc, argv, &i);
		else if (!strcmp(a, "--direct-token")) o->direct_token = arg(argc, argv, &i);
		else if (!strcmp(a, "--direct-ws")) o->direct_ws = arg(argc, argv, &i);
		else if (!strcmp(a, "--direct-ws-token")) o->direct_ws_token = arg(argc, argv, &i);
		else if (!strcmp(a, "--dry-run")) o->dry_run = arg(argc, argv, &i);
		else if (!strcmp(a, "-v")) o->verbose = 1;
		else {
			usage();
			return !strcmp(a, "-h") || !strcmp(a, "--help") ? 0 : 2;
		}
	}
	if (!o->app_id || !o->stream) {
		usage();
		return 2;
	}
	if (!o->dry_run && !o->direct) {
		if (o->secret_file) {
			FILE *f = fopen(o->secret_file, "r");
			if (!f || !fgets(secret, sizeof secret, f)) {
				fprintf(stderr, "cannot read %s\n", o->secret_file);
				return 2;
			}
			fclose(f);
			secret[strcspn(secret, "\r\n")] = 0;
		} else if (getenv(o->secret_env)) {
			snprintf(secret, sizeof secret, "%s", getenv(o->secret_env));
		}
		if (!secret[0]) {
			fprintf(stderr, "no appSecret: set $%s or use --secret-file (it is deliberately not a command-line argument)\n",
				o->secret_env);
			return 2;
		}
	}
	if (input_open(&in, o->input) != 0) {
		fprintf(stderr, "cannot open input %s\n", o->input);
		return 2;
	}
	if (o->realtime < 0)
		o->realtime = in.kind == 1;

	buf = (uint8_t *)malloc(64 * 1024);
	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	/* ---- probe: read until the PMT (and the AAC config) is known ---- */
	{
		tsd_callbacks pc;
		tsd *probe;
		memset(&pc, 0, sizeof pc);
		pc.on_video = noop_video;
		pc.on_audio = noop_audio;
		probe = tsd_create(&pc);
		while (!tsd_ready(probe) && pre_n < (8u << 20) && !g_quit) {
			int r = input_read(&in, buf, 64 * 1024);
			if (r <= 0)
				break;
			if (pre_n + (size_t)r > pre_cap) {
				pre_cap = (pre_n + (size_t)r) * 2;
				pre = (uint8_t *)realloc(pre, pre_cap);
			}
			memcpy(pre + pre_n, buf, (size_t)r);
			pre_n += (size_t)r;
			tsd_feed(probe, buf, (size_t)r);
		}
		if (!tsd_ready(probe)) {
			fprintf(stderr, "no H.264/HEVC stream found in the input (looked at %u bytes)\n", (unsigned)pre_n);
			return 1;
		}
		vcodec = tsd_video_codec(probe) == TSD_VIDEO_HEVC ? PPUSHER_CODEC_HEVC : PPUSHER_CODEC_H264;
		acodec = tsd_audio_codec(probe) == TSD_AUDIO_OPUS ? PPUSHER_AUDIO_OPUS :
			 tsd_audio_codec(probe) == TSD_AUDIO_AAC ? PPUSHER_AUDIO_AAC : PPUSHER_AUDIO_NONE;
		nlayers = tsd_video_layers(probe);
		ach = tsd_audio_channels(probe);
		arate = tsd_audio_rate(probe);
		tsd_destroy(probe);
	}
	if (o->codec && o->codec != vcodec)
		fprintf(stderr, "[warn] --codec says %s but the stream is %s; using the stream's codec (mmx checks path vs. PMT)\n",
			o->codec == PPUSHER_CODEC_HEVC ? "hevc" : "h264", vcodec == PPUSHER_CODEC_HEVC ? "hevc" : "h264");
	o->codec = vcodec;
	{
		int max = vcodec == PPUSHER_CODEC_HEVC ? PPUSHER_MAX_LAYERS_HEVC : PPUSHER_MAX_LAYERS_H264;
		if (nlayers > max) {
			fprintf(stderr, "[warn] input has %d video streams; ppmmx allows at most %d %s layers, ignoring the rest\n",
				nlayers, max, vcodec == PPUSHER_CODEC_HEVC ? "HEVC" : "H.264");
			nlayers = max;
		}
	}
	app.nlayers = nlayers;
	fprintf(stderr, "input: %d x %s video layer%s, %s audio\n", nlayers, vcodec == PPUSHER_CODEC_HEVC ? "HEVC" : "H.264",
		nlayers > 1 ? "s (simulcast, first = highest)" : "",
		acodec == PPUSHER_AUDIO_OPUS ? "Opus" : acodec == PPUSHER_AUDIO_AAC ? "AAC" : "no");

	if (o->dry_run) {
		pp_ts_config tc;
		memset(&tc, 0, sizeof tc);
		tc.video_codec = vcodec == PPUSHER_CODEC_HEVC ? PP_CODEC_HEVC : PP_CODEC_H264;
		tc.video_layers = nlayers;
		tc.audio_codec = acodec;
		tc.audio_channels = ach;
		tc.audio_sample_rate = arate;
		tc.sei_timestamp = o->sei;
		tc.sink = dry_sink;
		tc.ud = &app;
		app.dry = fopen(o->dry_run, "wb");
		if (!app.dry) {
			fprintf(stderr, "cannot write %s\n", o->dry_run);
			return 1;
		}
		app.dts_mux = pp_ts_create(&tc);
		if (!app.dts_mux) {
			fprintf(stderr, "cannot create the muxer for this input (Opus must be mono or stereo)\n");
			return 1;
		}
		pp_ts_write_tables(app.dts_mux);
	} else {
		ppusher_config_default(&cfg);
		cfg.ppcenter_url = o->ppcenter;
		cfg.app_id = o->app_id;
		cfg.app_secret = secret;
		cfg.stream_name = o->stream;
		cfg.region = o->region;
		cfg.video_codec = vcodec;
		cfg.audio_codec = acodec;
		cfg.audio_channels = ach > 0 ? ach : 2;
		cfg.audio_sample_rate = arate > 0 ? arate : 48000;
		cfg.video_layers = nlayers;
		if (o->bitrate_list) { /* "1500" or "3000,1500,600" */
			const char *q = o->bitrate_list;
			for (li = 0; li < nlayers && *q; li++) {
				cfg.video_bitrate_kbps[li] = atoi(q);
				while (*q && *q != ',')
					q++;
				if (*q == ',')
					q++;
			}
		}
		cfg.codec_path = !o->no_codec_path;
		cfg.enable_degrade = !o->no_degrade;
		cfg.enable_sei_timestamp = o->sei;
		cfg.passphrase = o->passphrase;
		cfg.srt_latency_ms = o->latency_ms;
		cfg.ca_file = o->ca_file;
		cfg.tls_verify = !o->insecure;
		cfg.direct_endpoint = o->direct;
		cfg.direct_token = o->direct_token;
		cfg.direct_ws_url = o->direct_ws;
		cfg.direct_ws_token = o->direct_ws_token;
		memset(&pcb, 0, sizeof pcb);
		pcb.set_bitrate = on_bitrate;
		pcb.on_layers = on_layers;
		pcb.request_keyframe = on_keyframe;
		pcb.on_state = on_state;
		pcb.on_log = on_log;
		pcb.ud = o;
		app.p = ppusher_create(&cfg, &pcb);
		memset(secret, 0, sizeof secret);
		if (!app.p) {
			fprintf(stderr, "invalid configuration (app id / stream name / codec)\n");
			return 2;
		}
		if (ppusher_start(app.p) != PPUSHER_OK) {
			fprintf(stderr, "cannot start\n");
			return 1;
		}
	}

	/* ---- main loop ---- */
	memset(&cb, 0, sizeof cb);
	cb.on_video = cb_video;
	cb.on_audio = cb_audio;
	cb.ud = &app;
	d = tsd_create(&cb);
	tsd_feed(d, pre, pre_n);
	free(pre);
	{
		int64_t last_report = pp_now_ms();
		while (!g_quit) {
			int r = input_read(&in, buf, 64 * 1024);
			if (r <= 0) {
				tsd_flush(d);
				if (o->loop && in.kind == 1) {
					app.loop_offset_us += (app.raw_last_us - app.raw_first_us) + 40000;
					fclose(in.f);
					in.f = fopen(in.path, "rb");
					tsd_destroy(d);
					d = tsd_create(&cb);
					if (in.f)
						continue;
				}
				break;
			}
			tsd_feed(d, buf, (size_t)r);
			if (app.p && pp_now_ms() - last_report >= 10000) {
				ppusher_stats_t st;
				last_report = pp_now_ms();
				ppusher_get_stats(app.p, &st);
				fprintf(stderr, "[stats] state=%d sent=%llu dropped=%llu frames_dropped=%llu reconnects=%u bitrate=%d%% "
						"degrade_ws=%d loss=%.2f%% rtt=%dms\n",
					st.state, (unsigned long long)st.msgs_sent, (unsigned long long)st.msgs_dropped,
					(unsigned long long)st.frames_dropped, st.reconnects, st.bitrate_percent,
					st.degrade_connected, st.last_loss_pct, st.rtt_ms);
			}
		}
	}
	tsd_destroy(d);
	fprintf(stderr, "input ended: %llu video AUs, %llu audio frames\n", (unsigned long long)app.video_n,
		(unsigned long long)app.audio_n);
	if (app.dts_mux) {
		pp_ts_flush(app.dts_mux);
		pp_ts_destroy(app.dts_mux);
		fclose(app.dry);
	}
	if (app.p) {
		if (!g_quit)
			pp_sleep_ms(1500); /* let the queue drain */
		ppusher_destroy(app.p);
	}
	free(buf);
	return rc;
}
