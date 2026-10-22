# ppusher

C/C++ publisher for embedded applications.

**libppusher** publishes H.264/HEVC + Opus/AAC from a hardware encoder to [PPCDN](https://pp-cdn.org) over SRT:
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
