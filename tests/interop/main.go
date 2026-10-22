// Interop check: parse ppusher's MPEG-TS with the same library ppmmx's SRT
// ingest uses (bluenviron/mediacommon mpegts.Reader, via
// internal/protocols/mpegts EnhancedReader) and apply the same acceptance
// rules: PMT must declare supported tracks, all video tracks share one codec
// (simulcast ladder: at most 4 H.264 / 3 HEVC layers, ValidateVideoTracks),
// the codec must match the publish path's /h264|/hevc suffix, every PES must
// carry a PTS, Opus/AAC must have PTS == DTS, and the elementary streams must
// decode.
//
//	go run . -codec h264 -layers 4 file.ts
package main

import (
	"bytes"
	"flag"
	"fmt"
	"os"

	"github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts"
	tscodecs "github.com/bluenviron/mediacommon/v2/pkg/formats/mpegts/codecs"
)

var obsUUID = []byte("OBS-ABSTS-SEI-v1")

func hasOBSSEI(au [][]byte, hevc bool) (uint64, bool) {
	for _, n := range au {
		if len(n) < 3 {
			continue
		}
		var hdr int
		if hevc {
			if t := (n[0] >> 1) & 0x3f; t != 39 && t != 40 {
				continue
			}
			hdr = 2
		} else {
			if n[0]&0x1f != 6 {
				continue
			}
			hdr = 1
		}
		// unescape
		var rb []byte
		zeros := 0
		for _, b := range n[hdr:] {
			if zeros >= 2 && b == 3 {
				zeros = 0
				continue
			}
			rb = append(rb, b)
			if b == 0 {
				zeros++
			} else {
				zeros = 0
			}
		}
		if len(rb) >= 26 && rb[0] == 5 && rb[1] == 24 && bytes.Equal(rb[2:18], obsUUID) {
			var ts uint64
			for _, b := range rb[18:26] {
				ts = ts<<8 | uint64(b)
			}
			return ts, true
		}
	}
	return 0, false
}

func nalTypes(au [][]byte, hevc bool) map[int]bool {
	m := map[int]bool{}
	for _, n := range au {
		if len(n) == 0 {
			continue
		}
		if hevc {
			m[int((n[0]>>1)&0x3f)] = true
		} else {
			m[int(n[0]&0x1f)] = true
		}
	}
	return m
}

type layerStats struct {
	aus, keys, sei int
	last           int64
	firstKeyParams bool
}

func main() {
	codec := flag.String("codec", "", "expected codec from the publish path (h264|hevc), empty = unconstrained")
	wantSEI := flag.Bool("sei", false, "require the OBS timestamp SEI on every video AU")
	wantLayers := flag.Int("layers", 1, "expected number of video tracks (simulcast layers)")
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: interop [-codec h264|hevc] [-layers N] [-sei] file.ts")
		os.Exit(2)
	}
	data, err := os.ReadFile(flag.Arg(0))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	fail := func(f string, a ...any) {
		fmt.Printf("FAIL: "+f+"\n", a...)
		os.Exit(1)
	}

	if len(data)%1316 != 0 {
		fail("file is %d bytes: not a multiple of 1316 (SRT message size)", len(data))
	}

	r := &mpegts.Reader{R: bytes.NewReader(data)}
	if err := r.Initialize(); err != nil {
		fail("Reader.Initialize: %v", err)
	}

	// mpegts.ValidateVideoTracks, as in ppmmx internal/protocols/mpegts/video_tracks.go
	var videoTracks []*mpegts.Track
	var audio *mpegts.Track
	for _, t := range r.Tracks() {
		fmt.Printf("track PID %d: %T\n", t.PID, t.Codec)
		if t.Codec.IsVideo() {
			videoTracks = append(videoTracks, t)
		}
	}
	if len(videoTracks) == 0 {
		fail("no video track")
	}
	name := func(t *mpegts.Track) string {
		switch t.Codec.(type) {
		case *tscodecs.H264:
			return "h264"
		case *tscodecs.H265:
			return "hevc"
		}
		return ""
	}
	vname := name(videoTracks[0])
	if vname == "" {
		fail("unsupported video codec %T", videoTracks[0].Codec)
	}
	for _, t := range videoTracks {
		if name(t) != vname {
			fail("SRT publish with multiple video tracks must use a single codec (PID %d is %T)", t.PID, t.Codec)
		}
	}
	maxLayers := 4
	if vname == "hevc" {
		maxLayers = 3
	}
	if len(videoTracks) > 1 && len(videoTracks) > maxLayers {
		fail("SRT publish contains %d %s video tracks, maximum is %d", len(videoTracks), vname, maxLayers)
	}
	if len(videoTracks) != *wantLayers {
		fail("expected %d video tracks, got %d", *wantLayers, len(videoTracks))
	}
	if *codec != "" && *codec != vname {
		fail("path requires %s but the stream carries %s", *codec, vname)
	}
	for _, t := range r.Tracks() {
		switch t.Codec.(type) {
		case *tscodecs.Opus, *tscodecs.MPEG4Audio:
			audio = t
		case *tscodecs.Unsupported:
			fail("PID %d declared as an unsupported codec", t.PID)
		}
	}
	// ppmmx reads the PMT order as the ladder order (highest quality first)
	for i := 1; i < len(videoTracks); i++ {
		if videoTracks[i].PID <= videoTracks[i-1].PID {
			fail("video tracks are not in ascending PID order: %d after %d", videoTracks[i].PID, videoTracks[i-1].PID)
		}
	}

	decodeErrs := 0
	r.OnDecodeError(func(err error) {
		decodeErrs++
		fmt.Printf("decode error: %v\n", err)
	})

	hevc := vname == "hevc"
	stats := make([]layerStats, len(videoTracks))
	for i := range stats {
		stats[i].last = -1
	}
	vcb := func(layer int, pts, dts int64, au [][]byte) error {
		st := &stats[layer]
		if pts != dts {
			fail("layer %d: video PTS != DTS (%d vs %d): B-frames present", layer, pts, dts)
		}
		if st.last >= 0 && pts <= st.last {
			fail("layer %d: video PTS not increasing: %d after %d", layer, pts, st.last)
		}
		st.last = pts
		types := nalTypes(au, hevc)
		key := types[5]
		if hevc {
			key = false
			for t := 16; t <= 21; t++ {
				key = key || types[t]
			}
		}
		if key {
			st.keys++
			ok := types[7] && types[8]
			if hevc {
				ok = types[32] && types[33] && types[34]
			}
			if st.keys == 1 {
				st.firstKeyParams = ok
			}
			if !ok {
				fail("layer %d: keyframe #%d lacks parameter sets: %v", layer, st.keys, types)
			}
		}
		if _, ok := hasOBSSEI(au, hevc); ok {
			st.sei++
		}
		st.aus++
		return nil
	}
	for i, t := range videoTracks {
		i := i
		if hevc {
			r.OnDataH265(t, func(pts, dts int64, au [][]byte) error { return vcb(i, pts, dts, au) })
		} else {
			r.OnDataH264(t, func(pts, dts int64, au [][]byte) error { return vcb(i, pts, dts, au) })
		}
	}
	var aFrames int
	var lastA int64 = -1
	if audio != nil {
		switch audio.Codec.(type) {
		case *tscodecs.Opus:
			r.OnDataOpus(audio, func(pts int64, pk [][]byte) error {
				if lastA >= 0 && pts < lastA {
					fail("opus PTS went backwards")
				}
				lastA = pts
				for _, p := range pk {
					if len(p) == 0 {
						fail("empty opus packet")
					}
				}
				aFrames += len(pk)
				return nil
			})
		case *tscodecs.MPEG4Audio:
			r.OnDataMPEG4Audio(audio, func(pts int64, aus [][]byte) error {
				lastA = pts
				aFrames += len(aus)
				return nil
			})
		}
	}

	for {
		if err := r.Read(); err != nil {
			break
		}
	}

	for i, st := range stats {
		fmt.Printf("layer %d (PID %d): AUs=%d keyframes=%d obsSEI=%d\n", i, videoTracks[i].PID, st.aus, st.keys, st.sei)
		if st.aus == 0 || st.keys == 0 {
			fail("layer %d: no video decoded", i)
		}
		if !st.firstKeyParams {
			fail("layer %d: first keyframe lacks parameter sets", i)
		}
		if *wantSEI && st.sei != st.aus {
			fail("layer %d: SEI timestamp on %d of %d AUs", i, st.sei, st.aus)
		}
		if st.aus != stats[0].aus {
			fail("layer %d decoded %d AUs, layer 0 decoded %d", i, st.aus, stats[0].aus)
		}
	}
	fmt.Printf("audio frames=%d | decode errors=%d\n", aFrames, decodeErrs)
	if audio != nil && aFrames == 0 {
		fail("audio track present but nothing decoded")
	}
	if decodeErrs > 0 {
		fail("%d decode errors", decodeErrs)
	}
	fmt.Println("OK: accepted by mediacommon mpegts.Reader with ppmmx's rules")
}
