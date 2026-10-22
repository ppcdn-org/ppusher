#!/usr/bin/env bash
# End-to-end test of ppusher against a LOCAL ppmmx (built from the ppmmx repo) and a mock ppcenter.
# Nothing here touches production: the mock ppcenter mints tokens with a throw-away key.
#
#   MMX=/path/to/mmx CLI=/path/to/ppusher-cli INPUT=/path/to/h264_opus.ts tests/e2e/run_local_e2e.sh
#
# Optional: MOCKCENTER=/path/to/prebuilt/mockcenter (default: go run), CODEC=h264|hevc (path suffix expected),
# LAYERS=N (simulcast ladder size of INPUT, default 1; see the ffmpeg recipe below).
# The input must be MPEG-TS with H.264 (or HEVC) and Opus/AAC, B-frames off, e.g.
#   ffmpeg -f lavfi -i testsrc2=size=640x360:rate=30 -f lavfi -i sine=frequency=440 -t 8 \
#          -c:v libx264 -bf 0 -g 30 -pix_fmt yuv420p -c:a libopus -f mpegts h264_opus.ts
# Simulcast input (3 layers, highest first; PMT order = layer order):
#   ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440 -t 8 \
#          -filter_complex "[0:v]split=3[a][b][c];[b]scale=960:540[b2];[c]scale=640:360[c2]" \
#          -map "[a]" -map "[b2]" -map "[c2]" -map 1:a -c:v libx264 -bf 0 -g 30 -pix_fmt yuv420p \
#          -c:a libopus -f mpegts h264_x3_opus.ts     # then LAYERS=3
set -u
: "${MMX:?set MMX to a ppmmx binary}" "${CLI:?set CLI to ppusher-cli}" "${INPUT:?set INPUT to a .ts file}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CODEC="${CODEC:-h264}"
LAYERS="${LAYERS:-1}"
# native Windows binaries (MSYS2/Git Bash) cannot open /c/... style paths
if command -v cygpath >/dev/null 2>&1; then INPUT="$(cygpath -m "$INPUT")"; fi
WORK="$(mktemp -d)"
KEY="e2e-local-key-$RANDOM$RANDOM"
SECRET="e2e-secret-$RANDOM"
STREAM="B01-frontView" # ppmmx's publish whitelist wants <table>-<view>
PIDS=()
fails=0

killp() {
  kill "$1" 2>/dev/null
  if command -v taskkill >/dev/null 2>&1 && [ -r "/proc/$1/winpid" ]; then taskkill //F //PID "$(cat /proc/$1/winpid)" >/dev/null 2>&1; fi
}
cleanup() { for p in "${PIDS[@]:-}"; do [ -n "$p" ] && killp "$p"; done; }
trap cleanup EXIT

check() { # check <description> <command...>
  local d="$1"; shift
  if "$@"; then echo "  ok    $d"; else echo "  FAIL  $d"; fails=$((fails + 1)); fi
}
wait_for() { # wait_for <seconds> <file> <fixed string>
  local t=$(( $1 * 5 ))
  while [ $t -gt 0 ]; do grep -qF -- "$3" "$2" 2>/dev/null && return 0; sleep 0.2; t=$((t - 1)); done
  return 1
}
count() { grep -cF -- "$2" "$1" 2>/dev/null || true; }

cat > "$WORK/mmx.yml" < "$HERE/mmx-local.yml"
cd "$WORK" || exit 2

echo "== mock ppcenter"
if [ -n "${MOCKCENTER:-}" ]; then
  "$MOCKCENTER" -listen 127.0.0.1:18000 -key "$KEY" -app appx -secret "$SECRET" > center.log 2>&1 &
else
  (cd "$HERE" && go run ./mockcenter -listen 127.0.0.1:18000 -key "$KEY" -app appx -secret "$SECRET") > center.log 2>&1 &
fi
PIDS+=($!)
wait_for 40 center.log "mockcenter on" || { echo "mockcenter did not start"; cat center.log; exit 2; }

start_mmx() {
  WHIP_AUTH_KEY="$KEY" MMX_NODE_SECRET=local-dummy-not-a-real-secret "$MMX" mmx.yml >> mmx.log 2>&1 &
  MMX_PID=$!
  PIDS+=($MMX_PID)
  wait_for 30 mmx.log "[SRT] started with listener" || { echo "ppmmx did not start"; tail -20 mmx.log; exit 2; }
  wait_for 15 center.log "control connection registered" || echo "  (warn) ppmmx never registered with the mock control plane"
}
echo "== ppmmx (local, token required, degrade on)"
start_mmx

echo "== ppusher-cli -> mock ppcenter -> ppmmx"
PPUSHER_APP_SECRET="$SECRET" "$CLI" --app-id appx --stream "$STREAM" --ppcenter http://127.0.0.1:18000 \
  --input "ts:$INPUT" --loop --bitrate-kbps 3000,1500,800,400 > cli.log 2>&1 &
PIDS+=($!)

P="appx/$STREAM/$CODEC"
check "publish accepted on the codec path ($P)" wait_for 30 mmx.log "is publishing to path '$P'"
check "ppmmx decoded video+audio tracks" wait_for 5 mmx.log "stream is available and online, $((LAYERS + 1)) tracks"
if [ "$LAYERS" -gt 1 ]; then
  check "ppmmx treats the $LAYERS video tracks as a simulcast ladder" wait_for 5 mmx.log "SRT publish contains $LAYERS"
fi
check "degrade WebSocket authenticated with the whipTracks bearer" wait_for 10 mmx.log "[degrade] path=$P executor client=ppusher"
check "ppmmx learned the layer count from the SRT session" wait_for 10 mmx.log "[degrade] path=$P max layers set to $LAYERS"
sleep 8
check "no decode errors on the ppmmx side" bash -c "! grep -q 'decode error' mmx.log"
check "ingest loss stays at zero on loopback" wait_for 10 mmx.log "unrecoverableLoss=0(0.00%)"
req1=$(count center.log "request #")
echo "  (ppcenter requests so far: $req1)"

echo "== restart ppmmx: ppusher must back off, ask ppcenter for NEW tokens and resume"
killp "$MMX_PID"; sleep 2
: > mmx.log
start_mmx
check "publishing resumed after the node came back" wait_for 45 mmx.log "is publishing to path '$P'"
req2=$(count center.log "request #")
echo "  (ppcenter requests now: $req2)"
check "a fresh ppcenter request was made for the reconnect (tokens never reused)" test "$req2" -gt "$req1"

echo "== wrong secret is fatal, not retried"
before=$(count center.log "request #")
PPUSHER_APP_SECRET="not-the-secret" timeout 8 "$CLI" --app-id appx --stream "$STREAM" --ppcenter http://127.0.0.1:18000 \
  --input "ts:$INPUT" > cli-badsecret.log 2>&1
after=$(count center.log "request #")
check "bad credentials reach the FATAL state" grep -q "FATAL" cli-badsecret.log
check "bad credentials produced no successful ppcenter answer" test "$after" -eq "$before"

echo
if [ "$fails" -eq 0 ]; then echo "E2E PASSED (logs in $WORK)"; else echo "E2E FAILED: $fails check(s) (logs in $WORK)"; fi
exit "$fails"
