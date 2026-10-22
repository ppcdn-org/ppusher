#include "pp_test.h"
#include "pp_degrade.h"
#include "pp_json.h"

static void parse(const char *j, pp_degrade_msg *m, int expect)
{
	CHECK_INT(pp_degrade_parse(j, strlen(j), m), expect);
}

static void test_target_state(void)
{
	pp_degrade_msg m;

	/* verbatim from publish-degrade-protocol.zh-CN.md section 6.3 */
	parse("{\"type\":\"TARGET_STATE\",\"path\":\"appbd4cd2aa7be8/B01-frontView/h264\",\"codec\":\"h264\","
	      "\"layers\":2,\"bitrate_percent\":60,\"latency_ms\":500}",
	      &m, 0);
	CHECK_INT(m.type, PP_MSG_TARGET_STATE);
	CHECK_STR(m.path, "appbd4cd2aa7be8/B01-frontView/h264");
	CHECK_STR(m.codec, "h264");
	CHECK_INT(m.layers, 2);
	CHECK_INT(m.bitrate_percent, 60);
	CHECK_INT(m.latency_ms, 500);
	CHECK_INT(m.reconnect, 0);

	/* SendReconnect() shape (ppmmx degrade.go) */
	parse("{\"type\":\"TARGET_STATE\",\"path\":\"a/b/hevc\",\"codec\":\"hevc\",\"layers\":1,\"bitrate_percent\":100,"
	      "\"latency_ms\":1200,\"reconnect\":true,\"reason\":\"degrade-latency-raise\"}",
	      &m, 0);
	CHECK_INT(m.type, PP_MSG_TARGET_STATE);
	CHECK_INT(m.reconnect, 1);
	CHECK_INT(m.latency_ms, 1200);
	CHECK_STR(m.reason, "degrade-latency-raise");
	CHECK_STR(m.codec, "hevc");

	/* older executors' view: no reconnect / reason / latency (omitempty) */
	parse("{\"type\":\"TARGET_STATE\",\"path\":\"a/b\",\"codec\":\"\",\"layers\":1,\"bitrate_percent\":80,\"latency_ms\":0}", &m, 0);
	CHECK_INT(m.reconnect, 0);
	CHECK_STR(m.codec, "");
	CHECK_INT(m.bitrate_percent, 80);

	/* missing bitrate_percent defaults to 100 (ppobs does the same); key order / whitespace do not matter */
	parse(" { \"layers\" : 3 , \"type\" : \"TARGET_STATE\" } ", &m, 0);
	CHECK_INT(m.type, PP_MSG_TARGET_STATE);
	CHECK_INT(m.layers, 3);
	CHECK_INT(m.bitrate_percent, 100);

	/* out of range values are clamped, not trusted */
	parse("{\"type\":\"TARGET_STATE\",\"layers\":-4,\"bitrate_percent\":900,\"latency_ms\":-1}", &m, 0);
	CHECK_INT(m.layers, 0);
	CHECK_INT(m.bitrate_percent, 100);
	CHECK_INT(m.latency_ms, 0);
	parse("{\"type\":\"TARGET_STATE\",\"bitrate_percent\":0}", &m, 0);
	CHECK_INT(m.bitrate_percent, 1);
}

static void test_loss_sample(void)
{
	pp_degrade_msg m;
	parse("{\"type\":\"LOSS_SAMPLE\",\"path\":\"a/b/h264\",\"codec\":\"h264\",\"unrecoverable_delta\":12,\"expected_delta\":580}", &m, 0);
	CHECK_INT(m.type, PP_MSG_LOSS_SAMPLE);
	CHECK_INT(m.unrecoverable_delta, 12);
	CHECK_INT(m.expected_delta, 580);

	parse("{\"type\":\"LOSS_SAMPLE\",\"unrecoverable_delta\":18446744073709551615,\"expected_delta\":1}", &m, 0);
	CHECK(m.unrecoverable_delta == 18446744073709551615ULL);

	/* a negative number is not a uint64 */
	parse("{\"type\":\"LOSS_SAMPLE\",\"unrecoverable_delta\":-5,\"expected_delta\":10}", &m, 0);
	CHECK_INT(m.unrecoverable_delta, 0);
	CHECK_INT(m.expected_delta, 10);
}

static void test_alert_and_unknown(void)
{
	pp_degrade_msg m;
	/* the real ALERT reason is Chinese: raw UTF-8 and \u escapes must both survive */
	parse("{\"type\":\"ALERT\",\"path\":\"live/t1\",\"reason\":\"layer=1,bitrate=60%,\xe4\xbb\x8d\xe4\xb8\x8d\xe5\x90\x88\xe8\xa7\x84\"}", &m, 0);
	CHECK_INT(m.type, PP_MSG_ALERT);
	CHECK_STR(m.reason, "layer=1,bitrate=60%,\xe4\xbb\x8d\xe4\xb8\x8d\xe5\x90\x88\xe8\xa7\x84");
	parse("{\"type\":\"ALERT\",\"reason\":\"\\u4ecd\\u4e0d\\u5408\\u89c4 \\\"x\\\" \\ud83d\\ude00\"}", &m, 0);
	CHECK_STR(m.reason, "\xe4\xbb\x8d\xe4\xb8\x8d\xe5\x90\x88\xe8\xa7\x84 \"x\" \xf0\x9f\x98\x80");

	parse("{\"type\":\"SOMETHING_NEW\",\"x\":1}", &m, 0);
	CHECK_INT(m.type, PP_MSG_UNKNOWN);
}

static void test_malformed(void)
{
	pp_degrade_msg m;
	parse("", &m, -1);
	parse("not json", &m, -1);
	parse("[1,2,3]", &m, -1);
	parse("{\"layers\":2}", &m, -1);
	parse("{\"type\":", &m, -1);
	parse("{\"type\":42}", &m, -1);
	/* long strings are truncated, never overflow */
	{
		char big[2000];
		char j[4200];
		memset(big, 'a', sizeof big - 1);
		big[sizeof big - 1] = 0;
		snprintf(j, sizeof j, "{\"type\":\"ALERT\",\"path\":\"%s\",\"reason\":\"%s\"}", big, big);
		parse(j, &m, 0);
		CHECK_INT(m.type, PP_MSG_ALERT);
		CHECK_INT(strlen(m.reason), sizeof m.reason - 1);
		CHECK_INT(strlen(m.path), sizeof m.path - 1);
	}
}

static void test_ws_url(void)
{
	char u[256];

	CHECK_INT(pp_degrade_ws_url("https://origin.pp-cdn.org/appbd4/B01/h264/whip", u, sizeof u), 0);
	CHECK_STR(u, "wss://origin.pp-cdn.org/appbd4/B01/h264/ws/whip");
	CHECK_INT(pp_degrade_ws_url("http://10.0.0.5:8889/app/s/hevc/whip", u, sizeof u), 0);
	CHECK_STR(u, "ws://10.0.0.5:8889/app/s/hevc/ws/whip");
	CHECK_INT(pp_degrade_ws_url("https://h/app/s/h264/whip/", u, sizeof u), 0);
	CHECK_STR(u, "wss://h/app/s/h264/ws/whip");

	/* shapes we refuse to guess */
	CHECK_INT(pp_degrade_ws_url("https://h/app/s/h264/whep", u, sizeof u), -1);
	CHECK_INT(pp_degrade_ws_url("https://h/app/s/h264/whip?x=1", u, sizeof u), -1);
	CHECK_INT(pp_degrade_ws_url("ftp://h/app/s/whip", u, sizeof u), -1);
	CHECK_INT(pp_degrade_ws_url("h/app/s/whip", u, sizeof u), -1);
	CHECK_INT(pp_degrade_ws_url("", u, sizeof u), -1);
	CHECK_INT(pp_degrade_ws_url(NULL, u, sizeof u), -1);
	{
		char tiny[10];
		CHECK_INT(pp_degrade_ws_url("https://h/app/s/h264/whip", tiny, sizeof tiny), -1);
	}
}

static void test_tracker(void)
{
	pp_target_tracker t;
	pp_degrade_msg m;
	memset(&t, 0, sizeof t);
	memset(&m, 0, sizeof m);

	/* first state at full bitrate: nothing for the encoder to do */
	m.layers = 1;
	m.bitrate_percent = 100;
	CHECK_INT(pp_target_apply(&t, &m), 0);
	/* mmx re-pushes the same state after a WS reconnect: idempotent */
	CHECK_INT(pp_target_apply(&t, &m), 0);

	m.bitrate_percent = 80;
	CHECK_INT(pp_target_apply(&t, &m), PP_TGT_BITRATE_CHANGED);
	CHECK_INT(pp_target_apply(&t, &m), 0);
	m.bitrate_percent = 60;
	m.layers = 2;
	CHECK_INT(pp_target_apply(&t, &m), PP_TGT_BITRATE_CHANGED | PP_TGT_LAYERS_CHANGED);

	/* reconnect is one-shot and independent of state equality */
	m.reconnect = 1;
	CHECK_INT(pp_target_apply(&t, &m), PP_TGT_RECONNECT);
	m.reconnect = 0;
	CHECK_INT(pp_target_apply(&t, &m), 0);
	m.reconnect = 1;
	m.bitrate_percent = 100;
	CHECK_INT(pp_target_apply(&t, &m), PP_TGT_RECONNECT | PP_TGT_BITRATE_CHANGED);

	/* very first message already degraded (we restarted while mmx is at 60%) */
	memset(&t, 0, sizeof t);
	m.reconnect = 0;
	m.bitrate_percent = 60;
	CHECK_INT(pp_target_apply(&t, &m), PP_TGT_BITRATE_CHANGED);
}

static void test_json_helpers(void)
{
	const char *j = "{\"a\":{\"b\":[1,{\"c\":\"}\"}],\"d\":\"x\\\"y\"},\"e\":7,\"f\":2.9,\"g\":true}";
	char s[32];
	int64_t v;
	int b;
	const char *sub;
	size_t sn;

	/* keys nested deeper must not be found at the top level */
	CHECK(!pp_json_str(j, strlen(j), "d", s, sizeof s));
	CHECK(pp_json_obj(j, strlen(j), "a", &sub, &sn));
	CHECK(pp_json_str(sub, sn, "d", s, sizeof s));
	CHECK_STR(s, "x\"y");
	CHECK(pp_json_int(j, strlen(j), "e", &v));
	CHECK_INT(v, 7);
	CHECK(pp_json_int(j, strlen(j), "f", &v));
	CHECK_INT(v, 2);
	CHECK(pp_json_bool(j, strlen(j), "g", &b));
	CHECK_INT(b, 1);
	CHECK(!pp_json_int(j, strlen(j), "g", &v));

	/* escaping for the ppcenter request body */
	{
		char out[64];
		CHECK_INT(pp_json_escape("a\"b\\c\n\x01", out, sizeof out), 15);
		CHECK_STR(out, "a\\\"b\\\\c\\n\\u0001");
		CHECK_INT(pp_json_escape("0123456789", out, 5), -1);
	}
}

int main(void)
{
	RUN(test_target_state);
	RUN(test_loss_sample);
	RUN(test_alert_and_unknown);
	RUN(test_malformed);
	RUN(test_ws_url);
	RUN(test_tracker);
	RUN(test_json_helpers);
	TEST_DONE();
}
