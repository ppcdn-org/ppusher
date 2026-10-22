// mockcenter: a stand-in for ppcenter's POST /v1/publish/requests for local
// end-to-end tests against a locally running ppmmx. It mints the same
// AES-256-GCM publish tokens (key = SHA-256(WHIP_AUTH_KEY), claims
// {uuid,appId,stream,codec,iat,exp}) so ppmmx's real token check, codec-path
// check and degrade WebSocket auth are exercised. Never point production
// credentials at it.
//
//	WHIP_AUTH_KEY=testkey ./mmx -config mmx-local.yml
//	go run ./mockcenter -key testkey -app appx -secret s3 -srt 127.0.0.1:7890 -webrtc http://127.0.0.1:8888
package main

import (
	"bufio"
	"crypto/aes"
	"crypto/cipher"
	"crypto/rand"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"strings"
	"sync/atomic"
	"time"
)

type claims struct {
	UUID   string `json:"uuid"`
	AppID  string `json:"appId"`
	Stream string `json:"stream"`
	Codec  string `json:"codec,omitempty"`
	IAT    int64  `json:"iat"`
	EXP    int64  `json:"exp"`
}

func mint(key, app, stream, codec string, ttl time.Duration) (string, error) {
	now := time.Now()
	pt, _ := json.Marshal(claims{AppID: app, Stream: stream, Codec: codec, IAT: now.Unix(), EXP: now.Add(ttl).Unix()})
	k := sha256.Sum256([]byte(key))
	b, err := aes.NewCipher(k[:])
	if err != nil {
		return "", err
	}
	g, err := cipher.NewGCM(b)
	if err != nil {
		return "", err
	}
	nonce := make([]byte, g.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(g.Seal(nonce, nonce, pt, nil)), nil
}

// controlWS answers ppmmx's control-plane WebSocket just enough for the node to
// consider itself registered (it refuses to run unmanaged): read the first
// frame (NodeRegister), reply with a protobuf NodeRegisterAck{accepted}, then
// swallow heartbeats.
func controlWS(w http.ResponseWriter, r *http.Request) {
	hj, ok := w.(http.Hijacker)
	if !ok || !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") {
		http.Error(w, "websocket only", http.StatusBadRequest)
		return
	}
	conn, rw, err := hj.Hijack()
	if err != nil {
		return
	}
	defer conn.Close()
	sum := sha1.Sum([]byte(r.Header.Get("Sec-WebSocket-Key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	fmt.Fprintf(rw, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
		base64.StdEncoding.EncodeToString(sum[:]))
	rw.Flush()
	br := bufio.NewReader(rw)
	registered := false
	for {
		var h [2]byte
		if _, err := io.ReadFull(br, h[:]); err != nil {
			return
		}
		n := int(h[1] & 0x7f)
		if n == 126 {
			var e [2]byte
			io.ReadFull(br, e[:])
			n = int(e[0])<<8 | int(e[1])
		} else if n == 127 {
			var e [8]byte
			io.ReadFull(br, e[:])
			n = int(e[4])<<24 | int(e[5])<<16 | int(e[6])<<8 | int(e[7])
		}
		if h[1]&0x80 != 0 {
			var m [4]byte
			io.ReadFull(br, m[:])
		}
		if _, err := io.CopyN(io.Discard, br, int64(n)); err != nil {
			return
		}
		if !registered {
			registered = true
			t := "COMMAND_TYPE_REGISTER_ACK"
			ack := append([]byte{0x0a, byte(len(t))}, t...)
			ack = append(ack, 0x10, 0x01) // accepted = true
			rw.Write(append([]byte{0x82, byte(len(ack))}, ack...))
			rw.Flush()
			log.Printf("ppmmx control connection registered (mock)")
		}
	}
}

func main() {
	listen := flag.String("listen", "127.0.0.1:18080", "listen address")
	key := flag.String("key", "", "WHIP_AUTH_KEY shared with the local ppmmx (required)")
	app := flag.String("app", "appx", "accepted appId")
	secret := flag.String("secret", "", "accepted appSecret (required)")
	srt := flag.String("srt", "127.0.0.1:7890", "ppcdnPublishEndpoint to hand out")
	webrtc := flag.String("webrtc", "http://127.0.0.1:8888", "ppmmx WebRTC base URL (for whipTracks); empty = omit")
	ttl := flag.Duration("ttl", 24*time.Hour, "token lifetime")
	flag.Parse()
	if *key == "" || *secret == "" {
		log.Fatal("-key and -secret are required")
	}

	var n atomic.Int64
	http.HandleFunc("/ws/mmx", controlWS)
	// ppmmx's publish whitelist: only apps listed here may publish.
	http.HandleFunc("/internal/mmx/v1/app-credentials/sync", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{"apps": []map[string]any{
			{"appId": *app, "appSecret": "local-signing-dummy", "keyId": "1"},
		}})
	})
	http.HandleFunc("/v1/publish/requests", func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			AppID        string   `json:"appId"`
			AppSecret    string   `json:"appSecret"`
			StreamName   string   `json:"streamName"`
			Protocol     string   `json:"protocol"`
			Capabilities []string `json:"capabilities"`
		}
		if r.Method != http.MethodPost || json.NewDecoder(r.Body).Decode(&req) != nil || req.AppID == "" || req.StreamName == "" {
			w.WriteHeader(http.StatusBadRequest)
			json.NewEncoder(w).Encode(map[string]string{"code": "invalid_request", "message": "invalid request"})
			return
		}
		if strings.Contains(req.StreamName, "/") {
			w.WriteHeader(http.StatusBadRequest)
			json.NewEncoder(w).Encode(map[string]string{"code": "invalid_request", "message": "valid appId, appSecret and streamName are required"})
			return
		}
		if req.AppID != *app || req.AppSecret != *secret {
			w.WriteHeader(http.StatusUnauthorized)
			json.NewEncoder(w).Encode(map[string]string{"code": "invalid_credentials", "message": "invalid credentials"})
			return
		}
		if req.Protocol != "srt" {
			w.WriteHeader(http.StatusBadRequest)
			json.NewEncoder(w).Encode(map[string]string{"code": "invalid_request", "message": "this mock only serves protocol srt"})
			return
		}
		out := map[string]any{"originNodeId": 1, "ppcdnPublishEndpoint": *srt}
		plain, _ := mint(*key, req.AppID, req.StreamName, "", *ttl)
		tokens := map[string]string{}
		for _, c := range []string{"h264", "hevc"} {
			tokens[c], _ = mint(*key, req.AppID, req.StreamName, c, *ttl)
		}
		out["publishToken"] = plain
		out["publishTokens"] = tokens
		wantTracks := false
		for _, c := range req.Capabilities {
			wantTracks = wantTracks || c == "whip-hevc-h264"
		}
		if wantTracks && *webrtc != "" {
			tracks := map[string]any{}
			for _, c := range []string{"h264", "hevc"} {
				t, _ := mint(*key, req.AppID, req.StreamName, c, *ttl)
				tracks[c] = map[string]string{
					"url":         fmt.Sprintf("%s/%s/%s/%s/whip", strings.TrimRight(*webrtc, "/"), req.AppID, req.StreamName, c),
					"bearerToken": t,
				}
			}
			out["whipTracks"] = tracks
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(out)
		log.Printf("request #%d: %s/%s -> %s (fresh tokens, degrade tracks=%v)", n.Add(1), req.AppID, req.StreamName, *srt, wantTracks)
	})
	log.Printf("mockcenter on http://%s (app %q)", *listen, *app)
	log.Fatal(http.ListenAndServe(*listen, nil))
}
