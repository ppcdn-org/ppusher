#include "pp_test.h"
#include "pp_streamid.h"

#include <stdlib.h>

static void test_build(void)
{
	char sid[PP_STREAMID_MAX];

	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01-frontView", PP_CODEC_HEVC, "TOK.en-_1"), PP_SID_OK);
	CHECK_STR(sid, "publish:app1/B01-frontView/hevc:TOK.en-_1");

	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01", PP_CODEC_H264, "T"), PP_SID_OK);
	CHECK_STR(sid, "publish:app1/B01/h264:T");

	/* codec-less path + token (codec_path = none) */
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01", PP_CODEC_NONE, "T"), PP_SID_OK);
	CHECK_STR(sid, "publish:app1/B01:T");

	/* no token: ppmmx without srtPublishTokenRequired */
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01", PP_CODEC_H264, NULL), PP_SID_OK);
	CHECK_STR(sid, "publish:app1/B01/h264");
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01", PP_CODEC_H264, ""), PP_SID_OK);
	CHECK_STR(sid, "publish:app1/B01/h264");
}

static void test_build_rejects(void)
{
	char sid[PP_STREAMID_MAX];
	char *big;

	/* stream name must not carry a codec suffix or any '/' */
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01/h264", PP_CODEC_H264, "T"), PP_SID_INVALID);
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "", PP_CODEC_H264, "T"), PP_SID_INVALID);
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "-app", "B01", PP_CODEC_H264, "T"), PP_SID_INVALID);
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app:1", "B01", PP_CODEC_H264, "T"), PP_SID_INVALID);
	/* ':' would be split by ppmmx, ',' starts an SRT field list, whitespace is never valid */
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01", PP_CODEC_H264, "a:b"), PP_SID_INVALID);
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01", PP_CODEC_H264, "a,b"), PP_SID_INVALID);
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01", PP_CODEC_H264, "a b"), PP_SID_INVALID);

	/* libsrt's 512 byte limit */
	big = (char *)malloc(600);
	memset(big, 'x', 599);
	big[599] = 0;
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01", PP_CODEC_H264, big), PP_SID_TOO_LONG);
	big[400] = 0; /* "publish:app1/B01/h264:" is 22 bytes -> 422, fits */
	CHECK_INT(pp_streamid_build(sid, sizeof sid, "app1", "B01", PP_CODEC_H264, big), PP_SID_OK);
	CHECK_INT(strlen(sid), 22 + 400);
	free(big);

	/* output buffer smaller than the result */
	{
		char tiny[8];
		CHECK_INT(pp_streamid_build(tiny, sizeof tiny, "app1", "B01", PP_CODEC_H264, "T"), PP_SID_TOO_LONG);
	}
}

static void test_names(void)
{
	char s[64];

	CHECK(pp_stream_name_valid("B01-frontView"));
	CHECK(pp_stream_name_valid("a"));
	CHECK(pp_stream_name_valid("a.b_c-d"));
	CHECK(!pp_stream_name_valid(""));
	CHECK(!pp_stream_name_valid(".a"));
	CHECK(!pp_stream_name_valid("a b"));
	CHECK(!pp_stream_name_valid("a/b"));
	CHECK(!pp_stream_name_valid(NULL));

	strcpy(s, "B01/hevc");
	CHECK_INT(pp_strip_codec_suffix(s), 1);
	CHECK_STR(s, "B01");
	strcpy(s, "B01/h264");
	CHECK_INT(pp_strip_codec_suffix(s), 1);
	CHECK_STR(s, "B01");
	strcpy(s, "B01");
	CHECK_INT(pp_strip_codec_suffix(s), 0);
	strcpy(s, "demo/av1");
	CHECK_INT(pp_strip_codec_suffix(s), 0);
	CHECK_STR(s, "demo/av1");
}

static void test_mask(void)
{
	char m[64];
	pp_mask_secret("abcdef0123456789", m, sizeof m);
	CHECK_STR(m, "abcdef...(len=16)");
	CHECK(strstr(m, "0123") == NULL);
	pp_mask_secret("abc", m, sizeof m);
	CHECK_STR(m, "abc...(len=3)");
	pp_mask_secret("", m, sizeof m);
	CHECK_STR(m, "(empty)");
	pp_mask_secret(NULL, m, sizeof m);
	CHECK_STR(m, "(empty)");
}

static void test_endpoint(void)
{
	char h[128];
	int p;

	CHECK_INT(pp_split_endpoint("origin.pp-cdn.org:7890", h, sizeof h, &p, 7890), 0);
	CHECK_STR(h, "origin.pp-cdn.org");
	CHECK_INT(p, 7890);

	CHECK_INT(pp_split_endpoint("origin.pp-cdn.org", h, sizeof h, &p, 7890), 0);
	CHECK_STR(h, "origin.pp-cdn.org");
	CHECK_INT(p, 7890);

	CHECK_INT(pp_split_endpoint("srt://1.2.3.4:9000/x?y=1", h, sizeof h, &p, 7890), 0);
	CHECK_STR(h, "1.2.3.4");
	CHECK_INT(p, 9000);

	CHECK_INT(pp_split_endpoint("[::1]:7000", h, sizeof h, &p, 7890), 0);
	CHECK_STR(h, "::1");
	CHECK_INT(p, 7000);

	CHECK_INT(pp_split_endpoint("", h, sizeof h, &p, 7890), -1);
	CHECK_INT(pp_split_endpoint(":7890", h, sizeof h, &p, 7890), -1);
	CHECK_INT(pp_split_endpoint("host:0", h, sizeof h, &p, 7890), -1);
	CHECK_INT(pp_split_endpoint("host:99999", h, sizeof h, &p, 7890), -1);
	CHECK_INT(pp_split_endpoint("host:abc", h, sizeof h, &p, 7890), -1);
	CHECK_INT(pp_split_endpoint("[::1", h, sizeof h, &p, 7890), -1);
}

int main(void)
{
	RUN(test_build);
	RUN(test_build_rejects);
	RUN(test_names);
	RUN(test_mask);
	RUN(test_endpoint);
	TEST_DONE();
}
