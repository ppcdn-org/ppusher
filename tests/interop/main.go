// Interop check: parse ppusher's MPEG-TS with the same library ppmmx's SRT
// ingest uses (bluenviron/mediacommon mpegts.Reader, via
// internal/protocols/mpegts EnhancedReader) and apply the same acceptance
// rules: PMT must declare supported tracks, one video codec, the codec must
// match the publish path's /h264|/hevc suffix, every PES must carry a PTS,
// Opus/AAC must have PTS == DTS, and the elementary streams must decode.
//
//	go run . -codec h264 file.ts
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

func main() {
	codec := flag.String("codec", "", "expected codec from the publish path (h264|hevc), empty = unconstrained")
	wantSEI := flag.Bool("sei", false, "require the OBS timestamp SEI on every video AU")
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: interop [-codec h264|hevc] [-sei] file.ts")
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

	var videoTracks []*mpegts.Track
	var video *mpegts.Track
	var audio *mpegts.Track
	for _, t := range r.Tracks() {
		fmt.Printf("track PID %d: %T\n", t.PID, t.Codec)
		if t.Codec.IsVideo() {
			videoTracks = append(videoTracks, t)
		}
	}
	if len(videoTracks) != 1 {
		fail("expected exactly one video track, got %d", len(videoTracks))
	}
	video = videoTracks[0]
	vname := ""
	switch video.Codec.(type) {
	case *tscodecs.H264:
		vname = "h264"
	case *tscodecs.H265:
		vname = "hevc"
	default:
		fail("unsupported video codec %T", video.Codec)
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

	decodeErrs := 0
	r.OnDecodeError(func(err error) {
		decodeErrs++
		fmt.Printf("decode error: %v\n", err)
	})

	var vAUs, vKeys, vSEI, aFrames int
	var lastV, lastA int64 = -1, -1
	var firstKeyHasParams bool
	hevc := vname == "hevc"
	vcb := func(pts, dts int64, au [][]byte) error {
		if pts != dts {
			fail("video PTS != DTS (%d vs %d): B-frames present", pts, dts)
		}
		if lastV >= 0 && pts <= lastV {
			fail("video PTS not increasing: %d after %d", pts, lastV)
		}
		lastV = pts
		types := nalTypes(au, hevc)
		key := types[5]
		if hevc {
			key = false
			for t := 16; t <= 21; t++ {
				key = key || types[t]
			}
		}
		if key {
			vKeys++
			ok := types[7] && types[8]
			if hevc {
				ok = types[32] && types[33] && types[34]
			}
			if vKeys == 1 {
				firstKeyHasParams = ok
			}
			if !ok {
				fail("keyframe #%d lacks parameter sets: %v", vKeys, types)
			}
		}
		if _, ok := hasOBSSEI(au, hevc); ok {
			vSEI++
		}
		vAUs++
		return nil
	}
	if hevc {
		r.OnDataH265(video, func(pts, dts int64, au [][]byte) error { return vcb(pts, dts, au) })
	} else {
		r.OnDataH264(video, func(pts, dts int64, au [][]byte) error { return vcb(pts, dts, au) })
	}
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

	fmt.Printf("video AUs=%d keyframes=%d obsSEI=%d | audio frames=%d | decode errors=%d\n", vAUs, vKeys, vSEI, aFrames, decodeErrs)
	if vAUs == 0 || vKeys == 0 {
		fail("no video decoded")
	}
	if audio != nil && aFrames == 0 {
		fail("audio track present but nothing decoded")
	}
	if decodeErrs > 0 {
		fail("%d decode errors", decodeErrs)
	}
	if !firstKeyHasParams {
		fail("first keyframe lacks parameter sets")
	}
	if *wantSEI && vSEI != vAUs {
		fail("SEI timestamp on %d of %d AUs", vSEI, vAUs)
	}
	fmt.Println("OK: accepted by mediacommon mpegts.Reader with ppmmx's rules")
}
