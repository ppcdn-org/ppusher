# ppusher

> **PPCDN** is an open-source community project: <https://www.pp-cdn.org>
>
> PPCDN delivers 30%–50% bandwidth and cost savings with an end-to-end ultra-low latency (<500 ms) experience:
>
> 1. Multiple ingest protocols (ERTMP/SRT/WHIP) adapt to different live-site networks while delivering ultra-low latency (including playout buffer):
>    P2P direct: <120 ms; WHIP: <200 ms; ERTMP: <200 ms; SRT: <500 ms.
> 2. Multiple technologies combine to cut traffic and cost by 30%–50%: HEVC simulcast, embedded P2P traversal service, and zero network transcoding.
> 3. ABR adapts to each viewer's network for smoother playback.
> 4. An API exposes the current playback frame timestamp for synchronizing game results with the audio/video stream.
>
> Compared with Tencent Cloud Live: 1 s lower latency; zero transcoding fees; readable real-time timestamps; no black screen or stall when switching quality levels.
>
> Official online demo: <https://pplayer.pp-cdn.org/>
> Download [ppobs v32.1.2rc008](https://github.com/ppcdn-org/ppobs/releases/tag/v32.1.2rc008) and publish first, then watch.

C/C++ publisher for embedded applications.

**libppusher** publishes H.264/HEVC + Opus from a hardware encoder (audio must be Opus: ppmmx rejects AAC and any other audio on SRT) to [PPCDN](https://pp-cdn.org) over SRT:
lightweight MPEG-TS muxer, ppcenter token exchange (fresh token on every reconnect), ppmmx degrade/loss control
channel (bitrate steps, `reconnect`, `LOSS_SAMPLE`) and a reconnect state machine. Pure C99; depends only on
libsrt (+ optional OpenSSL for HTTPS/wss). MIT licensed.

- Full documentation (Chinese): [README.zh-CN.md](README.zh-CN.md)
- Design notes (Chinese): [doc/design.zh-CN.md](doc/design.zh-CN.md)
- Public C API: [include/ppusher/ppusher.h](include/ppusher/ppusher.h)
- Test/demo CLI: `ppusher-cli` (feeds an MPEG-TS source through the library API)

```
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```
