# ppusher

面向嵌入式 Linux（ARM / MIPS 推流盒、摄像头、编码器板卡）的 **PPCDN SRT 推流库**，纯 C99，依赖只有 libsrt（外加可选的 OpenSSL，用于 HTTPS/wss）。

硬件编码器把 **H.264 / HEVC 的 Annex-B 码流**和 **Opus 音频帧**推进来（SRT 推流的音频必须是 Opus：ppmmx 会明确拒绝 AAC 等其它音频，因为节点间转发走 WHIP、WebRTC 不承载 AAC），库内完成：

- 轻量 MPEG-TS 复用（与 ppmmx 的 SRT 摄取校验兼容），经 libsrt 发往 PPCDN Origin；
- **SRT 同播（simulcast）**：同一条 SRT 连接里发多层视频，**H.264 最多 4 层、HEVC 最多 3 层**（ppmmx 的上限），降级时按 ppmmx 的 `TARGET_STATE.layers` 砍最高层并重握手；
- 向 ppcenter 换推流节点和 **每次连接都全新的 token**；
- 与 ppmmx 的**降级/丢包控制通道**（WebSocket）对接：执行码率调整、处理 `reconnect`、消费 `LOSS_SAMPLE`；
- **重连状态机**：断线指数退避、mmx 要求重连、本地丢包窗口超阈值快速重连。

只支持 SRT 推流（不含 RTMP / WHIP / RTSP）。

```
 硬件编码器                       ppusher                                      PPCDN
 ──────────         ┌──────────────────────────────────────────┐
 H.264/HEVC AU ───▶ │ TS 复用 ─▶ 发送队列 ─▶ libsrt ───────────┼──SRT──▶ ppmmx Origin :7890
 Opus/AAC 帧   ───▶ │                                          │         (streamid 带 token)
 ◀─ set_bitrate()   │ 会话状态机 ──POST /v1/publish/requests───┼──HTTPS─▶ ppcenter
 ◀─ request_keyframe│            ──降级 WebSocket (wss) ───────┼──WS────▶ ppmmx /<path>/ws/whip
                    └──────────────────────────────────────────┘
```

设计细节和所有协议结论的出处见 [`doc/design.zh-CN.md`](doc/design.zh-CN.md)。

---

## 1. 与 PPCDN 的协议对应关系

| 环节 | ppusher 的做法 | 对应实现（以代码为准） |
|---|---|---|
| 鉴权取节点 | `POST {ppcenter}/v1/publish/requests`，体：`appId` `appSecret` `streamName` `protocol:"srt"` `capabilities:["whip-hevc-h264"]` `requestRegion?`。取 `ppcdnPublishEndpoint`、`publishToken`、`publishTokens{h264,hevc}`、`whipTracks` | ppcenter `internal/apis/publish_v1.go`；ppobs `PPCDNPublishEndpoint.cpp` |
| 推流 path | `{appId}/{stream}/{h264\|hevc}`，**默认带 codec 后缀**（pplayer 靠它播放；ppmmx 也会校验 PMT 的 codec 与后缀一致）。`stream` 本身不得带后缀 | ppmmx `srt/publish_codec.go`；ppobs 多轨关闭时的单轨后缀 |
| streamid | `publish:{appId}/{stream}/{codec}:{token}`，token 是第 3 段，**绑定精确 path**，所以用 `publishTokens[codec]` 而不是 `publishToken`；拼装前拒绝含 `:` `,` 空白的 token，总长 ≤ 512 | ppmmx `srt/streamid.go`；ppobs `rtmp-custom.c::inject_ppcdn_publish_token` |
| token 有效期 | 每次**进入取节点阶段**（首次、断线、`reconnect`、丢包重连）都重新向 ppcenter 取 token，**从不复用** | 设计文档“过期与重连” |
| 降级通道 | 把 `whipTracks[codec].url`（`https://…/{path}/whip`）换成 `wss://…/{path}/ws/whip`；`Authorization: Bearer <whipTracks 的 bearerToken>`；连上后发 `CLIENT_INFO`；SRT 连上**之后**才连（与 ppobs 同序） | ppmmx `webrtc/degrade_ws_handler.go`；`docs/design/publish-degrade-protocol.zh-CN.md` §6 |
| `TARGET_STATE` | `bitrate_percent` 变化 → 回调 `set_bitrate(基准码率 × 百分比)`（不重启编码器）；相同状态幂等忽略；`layers` 仅记录（单层编码器已在终止态）；`latency_ms` 记下，**下次握手**用作 SRT 延迟 | ppmmx `internal/degrade/degrade.go` |
| 同播层数 | 一条连接里多条视频 elementary stream，PMT 顺序 = 层顺序（**第 0 层 = 最高画质**，ppmmx 把 PMT 顺序当作 highest → lowest）；H.264 ≤ 4、HEVC ≤ 3，同一连接只能同一种 codec（超限 ppmmx 直接拒绝，库在创建时就拒绝）。每层各有自己的 SPS/PPS 与 IDR，同一帧的各层共用 pts | ppmmx `protocols/mpegts/video_tracks.go::ValidateVideoTracks`；`srt/conn.go` |
| `TARGET_STATE.layers` | 层数变化（砍最高层 / 恢复）→ 记录目标层数，**用裁剪后的 PMT 重握手**（保留最低的 N 层，重新取 token）；被砍掉的层再 `push` 会返回 `PPUSHER_E_LAYER_OFF`；`on_layers(first, count)` 回调告知新阶梯。5 秒防抖挡住的请求不会丢，防抖结束后自动重试。码率百分比对所有在线层生效（每层以自己的标称码率为基准） | `docs/design/publish-degrade-protocol.zh-CN.md` §7.1、ppobs `srt-degrade-client.cpp` |
| `reconnect=true` | 立即关闭当前 SRT、重新取 token、重连（不累计退避，5 秒内最多触发一次）。**ppobs 目前未处理该标志**，靠 mmx 2 秒后掐连接兜底；ppusher 主动处理，中断更短 | `srt/conn.go::requestPublisherReconnect` |
| `LOSS_SAMPLE` | 以 `retrans=0` 喂给本地丢包窗口（算法与 ppobs `publish-loss-reconnect.h` 逐行一致：周期聚合、热身一个窗口、默认 30s/300s/1.5%）；降级通道在线时用它，不在线才用本地 `srt_bistats`（发送端口径） | ppobs `srt-degrade-client.cpp::HandleLossSample` |
| 退避 | 1s → 30s 指数退避，±20% 抖动，连接稳定 ≥ 30s 清零；降级 WS 自己 2s → 30s | 协议文档 §7.3 |
| 错误分类 | 400/401/403 → `FATAL`（不重试，等人处理凭据）；402 欠费 → 每 60s 重试；其余（含 503 `no_healthy_origin`、网络错误、404 网关抖动）→ 退避重试 | `publish_v1.go` 的错误码 |

### MPEG-TS 复用必须满足的约束（已按 ppmmx 读取侧核对）

- 每条 SRT 消息正好 **1316 字节**（7×188）；ppmmx 用 1316 的缓冲读，更大的会被截断。
- 连接开头先发 PAT+PMT，之后每个关键帧前、以及 ≥500ms 无 PSI 时重发。
- 视频 `0x1B`(H.264) / `0x24`(HEVC)，每连接只一种 codec，PES 无长度，关键帧置 RAI，AU 前补 AUD，关键帧缺参数集时用**该层**缓存的 SPS/PPS(/VPS) 补齐。同播时第 N 层的 PID 是 `0x100+N`，音频固定 `0x110`；**PCR 只放在第一条在线的视频层**上（每个 AU 带一个）；PAT/PMT 每个关键帧时刻只发一份（不是每层一份）。
- Opus：`stream_type 0x06` + **registration(`Opus`) + extension(0x80, channel_config_code)** 两个描述符，PES 载荷带 `7F E0 <len>` 控制头，**PTS 必须等于 DTS**。
- AAC：`0x0F` + ADTS，首个 PES 必须是完整 ADTS 帧（mmx 从里面取配置）。
- 不支持 B 帧（`pts == dts`），请把编码器设为 B 帧数 0。
- 同播时同一帧的各层依次 `push`，**最后一层**（`video_layers - 1`）结束时才冲刷 SRT 消息，其余层共用消息，避免每层一次填充浪费带宽。
- 可选 `OBS-ABSTS-SEI-v1` 绝对时间戳（`enable_sei_timestamp`）：H.264 放在首个 VCL 之前，HEVC 用 suffix SEI。

> **ppmmx 的发布白名单要求流名形如 `<table>-<view>`**（两段，字符集 `[A-Za-z0-9_-]`，各 ≤64），例如 `B01-frontView`。名字不匹配会在 SRT 连上后被 `'…' not configured caused publish refusal!` 拒绝，表现为“连上又断”。

---

## 2. 构建

依赖：C99 编译器、CMake ≥ 3.10、**libsrt ≥ 1.4**（开发包；在 1.5.4 上验证）、可选 OpenSSL（HTTPS/wss；默认开启）。

```bash
# Linux x86_64（Debian/Ubuntu）
sudo apt install build-essential cmake pkg-config libsrt-openssl-dev libssl-dev
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CMake 选项：

| 选项 | 默认 | 说明 |
|---|---|---|
| `PPUSHER_TLS` | `openssl` | `openssl` / `none`。`none` 时只能连 `http://` 的 ppcenter 与 `ws://`（局域网/自建 ppmmx）；生产 `api.pp-cdn.org` 是 HTTPS，必须用 `openssl` |
| `PPUSHER_WITH_SRT` | `ON` | `OFF` 只编译纯逻辑与网络层（无 libsrt），用于单元测试 |
| `PPUSHER_BUILD_CLI` | `ON` | 构建 `ppusher-cli` |
| `PPUSHER_BUILD_TESTS` | `ON` | 构建测试 |
| `SRT_ROOT` | – | libsrt 前缀（找不到 pkg-config 时用） |
| `BUILD_SHARED_LIBS` | `OFF` | 默认静态库 `libppusher.a` |

体积参考（x86_64 MinGW，未 strip，不含 libsrt/OpenSSL）：全部目标文件 `.text` 合计约 53 KB；`libppusher.a` 约 110 KB。运行时内存主要是发送队列（`queue_kb`，默认 2 MB，可调小）和 WebSocket 接收缓冲 64 KB。

### Windows（MSYS2 / MinGW-w64）

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DSRT_ROOT=<libsrt 前缀> -DOPENSSL_ROOT_DIR=C:/msys64/mingw64
cmake --build build -j
```

MinGW 的 OpenSSL **没有系统证书库**，访问 HTTPS 的 ppcenter 需要 `ca_file`（例如 `C:/msys64/usr/ssl/certs/ca-bundle.crt`，CLI 用 `--ca-file`），否则会明确报“TLS certificate check failed”。**不要**为了省事默认关闭校验。

### 交叉编译到 ARM / MIPS

`cmake/toolchains/` 下有三份示例（`arm-linux-gnueabihf`、`aarch64-linux-gnu`、`mipsel-linux-gnu`）。前提：目标板的 **libsrt 与 OpenSSL 必须先用同一交叉工具链编出来**，装进一个 sysroot（Buildroot / OpenWrt / Yocto 用户直接用发行版自带包即可）。

```bash
# 1) libsrt（一次性）
cmake -S srt -B build-srt -DCMAKE_TOOLCHAIN_FILE=<ppusher>/cmake/toolchains/arm-linux-gnueabihf.cmake \
      -DPPUSHER_SYSROOT=$SYSROOT -DCMAKE_INSTALL_PREFIX=$SYSROOT/usr \
      -DENABLE_APPS=OFF -DENABLE_SHARED=OFF -DUSE_ENCLIB=openssl
cmake --build build-srt && cmake --install build-srt

# 2) ppusher
cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-linux-gnueabihf.cmake \
      -DPPUSHER_SYSROOT=$SYSROOT -DPPUSHER_BUILD_TESTS=OFF
cmake --build build-arm
```

要点：

- 只想要极小体积、且不需要 SRT 加密：libsrt 用 `-DENABLE_ENCRYPTION=OFF`，这时可以不依赖 OpenSSL 作 SRT 加密，但访问 HTTPS 的 ppcenter 仍需 OpenSSL（`PPUSHER_TLS=openssl`）。
- libsrt 是 C++ 库，最终链接需要 `libstdc++`（静态链接用 `-static-libstdc++ -static-libgcc` 即可）。
- OpenWrt 用其 SDK 的工具链名：`-DPPUSHER_TRIPLE=mipsel-openwrt-linux-musl`（mipsel 文件已支持）。
- 嵌入式板子通常没有根证书：把 `ca-certificates.crt` 放到板上，通过配置 `ca_file` 指定。
- **说明**：这三份 toolchain 文件和上述步骤尚未在真实交叉工具链上验证过（开发机是 Windows，没有 Linux 编译器），首次使用时请留意；核心代码只用 POSIX 接口（pthread、BSD socket），无 x86 专有假设。

---

## 3. 命令行示例 `ppusher-cli`

把一路 MPEG-TS（stdin / UDP / 文件）拆成 ES 后通过库的公共 API 推送，走的是和硬件编码器完全相同的路径，便于联调。

```bash
export PPUSHER_APP_SECRET=...      # appSecret 刻意不能走命令行参数（ps 可见）
ffmpeg -re -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440 \
       -c:v libx264 -preset veryfast -bf 0 -g 60 -pix_fmt yuv420p -c:a libopus -b:a 64k -f mpegts - \
  | ppusher-cli --app-id <appId> --stream B01-frontView --ca-file /etc/ssl/certs/ca-certificates.crt
```

常用选项：

| 选项 | 说明 |
|---|---|
| `--app-id` `--stream` | PPCDN 应用 ID、流名（流名不带 `/h264` `/hevc`；注意上面“`<table>-<view>`”限制） |
| `--ppcenter URL` | 默认 `https://api.pp-cdn.org` |
| `--secret-env VAR` / `--secret-file F` | appSecret 来源（默认环境变量 `PPUSHER_APP_SECRET`） |
| `--input ts:-` / `ts:udp://0.0.0.0:5000` / `ts:FILE` | 输入；文件默认按时间戳实时节奏发送，`--loop` 循环 |
| `--bitrate-kbps A[,B,..]` | 每层标称码率（第 0 层在前），用于打印降级目标 |
| `--no-degrade` / `--no-codec-path` / `--sei` | 关闭降级通道 / 不加 codec 后缀（pplayer 无法播放）/ 注入 SEI 时间戳 |
| `--passphrase P` `--latency MS` | SRT 加密、延迟 |
| `--ca-file F` `--insecure` | TLS CA 文件 / 跳过证书校验（仅测试） |
| `--direct HOST:PORT [--direct-token T] [--direct-ws URL --direct-ws-token T]` | 跳过 ppcenter，直连自建 ppmmx |
| `--dry-run FILE` | 不联网，只把复用结果写成 TS 文件，用于核对 PAT/PMT |

输入要求：H.264/HEVC + Opus，**无 B 帧**；音频不是 Opus（如 AAC）的推流会被 ppmmx 拒绝，请先转成 Opus。输入含 B 帧时 CLI 会警告并改用 DTS。

**同播输入**：TS 里有几条同 codec 的视频 elementary stream 就推几层（H.264 最多 4、HEVC 最多 3，多出的忽略并警告），PMT 顺序即层顺序、第一条应是最高画质。例如用 ffmpeg 生成 3 层：

```bash
ffmpeg -re -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440 \
       -filter_complex "[0:v]split=3[a][b][c];[b]scale=960:540[b2];[c]scale=640:360[c2]" \
       -map "[a]" -map "[b2]" -map "[c2]" -map 1:a -c:v libx264 -bf 0 -g 60 -pix_fmt yuv420p -c:a libopus -f mpegts - \
  | ppusher-cli --app-id <appId> --stream B01-frontView --bitrate-kbps 3000,1500,600 --ca-file ...
```

---

## 4. C API

完整声明见 [`include/ppusher/ppusher.h`](include/ppusher/ppusher.h)。

```c
#include <ppusher/ppusher.h>

static void set_bitrate(int layer, int kbps, void *ud) { encoder_set_bitrate(layer, kbps); } /* 不要重启编码器 */
static void want_idr(void *ud)                         { for (int l = 0; l < 3; l++) encoder_request_idr(l); }
static void on_layers(int first, int count, void *ud)  { /* first > 0：低于 first 的层被降级砍掉，可停掉其编码器 */ }

ppusher_config_t cfg;
ppusher_config_default(&cfg);                 /* 必须先取默认值，再覆盖 */
cfg.ppcenter_url      = "https://api.pp-cdn.org";
cfg.app_id            = "appxxxxxxxx";
cfg.app_secret        = secret;               /* 库内拷贝；日志不打印 */
cfg.stream_name       = "B01-frontView";
cfg.video_codec       = PPUSHER_CODEC_H264;   /* 或 HEVC；决定 path 后缀和 PMT */
cfg.audio_codec       = PPUSHER_AUDIO_OPUS;   /* 或 NONE；AAC 会被 ppmmx 拒绝 */
cfg.audio_channels    = 2;
cfg.video_layers      = 3;                    /* 同播层数：H.264 1..4，HEVC 1..3；0/1 = 单层 */
cfg.video_bitrate_kbps[0] = 3000;             /* 每层的标称码率（第 0 层 = 最高画质） */
cfg.video_bitrate_kbps[1] = 1500;
cfg.video_bitrate_kbps[2] = 600;                /* set_bitrate() 收到的是它 × bitrate_percent */
cfg.ca_file           = "/etc/ssl/certs/ca-certificates.crt";

ppusher_callbacks_t cb = { .set_bitrate = set_bitrate, .on_layers = on_layers, .request_keyframe = want_idr };
ppusher_t *p = ppusher_create(&cfg, &cb);
ppusher_start(p);                             /* 立即返回，库自己的线程负责联网/重连 */

/* 编码线程里：未连上时返回 PPUSHER_E_NOT_CONNECTED，直接丢帧即可，不用缓存 */
for (int l = 0; l < 3; l++)                   /* 同一帧的各层依次推，pts 相同；单层时 layer 恒为 0 */
    ppusher_push_video(p, l, annexb_au[l], len[l], pts_us, pts_us, is_idr[l]);   /* pts == dts，微秒 */
ppusher_push_audio(p, opus_packet, len, pts_us);

ppusher_stop(p);  ppusher_destroy(p);
```

要点：

- 同播：每层一路独立编码器（独立 SPS/PPS、独立 IDR，**各层 IDR 尽量对齐**），`request_keyframe` 回调对所有在线层都要出 IDR。被降级砍掉的层 `push` 返回 `PPUSHER_E_LAYER_OFF`，直接丢弃即可。
- `push_*` 线程安全、非阻塞。每次（重）连上后库会回调 `request_keyframe`，并在拿到第一个 IDR 之前丢弃非关键帧，所以服务端看到的首个视频 AU 一定是 IDR。
- 发送队列超过 75% 时库会主动丢非关键帧并请求 IDR，不会让延迟无限增长。
- 状态：`on_state` 回调 / `ppusher_get_stats()`（`FETCHING → CONNECTING → STREAMING → BACKOFF …`，`FATAL` 表示凭据被拒）。
- 回调在库线程里执行，必须快速返回。
- libsrt 默认会往 stderr 打警告，产品里可 `srt_setloglevel()` 调整。

### 配置项（`ppusher_config_t`，`ppusher_config_default()` 给出的默认值）

| 字段 | 默认 | 说明 |
|---|---|---|
| `ppcenter_url` `app_id` `app_secret` `stream_name` `region` | – | 鉴权与选路 |
| `video_codec` `audio_codec` `audio_channels` `audio_sample_rate` | H264 / Opus / 2 / 48000 | 媒体；Opus 仅支持 1/2 声道；`audio_sample_rate` 只用于 AAC 的 ADTS 头 |
| `video_layers` | 1 | 同播层数：H.264 1–4，HEVC 1–3；超限 `ppusher_create` 返回 NULL；0 视为 1 |
| `video_bitrate_kbps[4]` | 0 | 每层标称码率（kbps），0 则该层不回调 `set_bitrate` |
| `codec_path` | 1 | 1 = 发布到 `<stream>/h264|hevc`（推荐）；0 = 裸 path |
| `enable_degrade` | 1 | 连接 ppmmx 降级 WebSocket |
| `enable_sei_timestamp` | 0 | 每个视频 AU 注入 UTC 毫秒 SEI（端到端延迟测量） |
| `passphrase` | NULL | SRT 链路加密（10–79 字符） |
| `srt_latency_ms` | 0 | 0 = libsrt 默认；mmx 下发的 `latency_ms` 会在下次握手时取较大者 |
| `loss_reconnect_enable` / `loss_period_sec` / `loss_window_sec` / `loss_threshold_pct` | 1 / 30 / 300 / 1.5 | 本地丢包快速重连窗口 |
| `backoff_min_ms` / `backoff_max_ms` | 1000 / 30000 | 重连退避 |
| `queue_kb` | 2048 | 发送队列（每条 1316 B）；内存紧张的板子调小 |
| `tls_verify` / `ca_file` | 1 / NULL | HTTPS/wss 证书校验 |
| `http_timeout_ms` | 8000 | ppcenter 请求总超时 |
| `direct_endpoint` `direct_token` `direct_ws_url` `direct_ws_token` | NULL | 直连模式：跳过 ppcenter（自建 ppmmx / 实验室） |

---

## 5. 依赖取舍：为什么不用 libcurl

ppusher 只需要“一次 HTTPS POST + 一条 WebSocket”，所以自带约 600 行极简实现（`pp_net` / `pp_http` / `pp_ws`），TLS 通过可选的 OpenSSL 接入。

| | libcurl | 自带 + 可选 OpenSSL（采用） |
|---|---|---|
| 体积/依赖 | 带 TLS 约数 MB，依赖链长，交叉编译麻烦 | 明文模式零依赖；HTTPS 只多一个 OpenSSL（libsrt 开加密时本来就链接它） |
| WebSocket | 需要 curl ≥ 7.86 且长期是实验特性，旧固件基本不可用 | 完整的 RFC 6455 客户端（握手、掩码、ping/pong、分片、close） |
| 能力 | 代理、重定向、HTTP/2 | 仅 HTTP/1.1 POST，不跟随重定向、不支持 HTTP 代理 |

mbedTLS 后端**没有实现**（接口留在 `pp_net.c` 的 `PPUSHER_HAVE_OPENSSL` 分支里，想接的话只需补 `pp_conn_*` 的 TLS 部分）。

---

## 6. 测试与验证状态

```bash
ctest --test-dir build --output-on-failure
```

| 测试 | 内容 |
|---|---|
| `test_streamid` | streamid/token 拼装（codec 后缀、`:` `,` 空白、512 字节上限）、流名校验、日志打码、端点解析 |
| `test_degrade` | `TARGET_STATE`（含 `reconnect`/`reason`、老格式、越界值）、`LOSS_SAMPLE`、`ALERT`（含中文与 `\u` 转义）、畸形输入、WS URL 推导、幂等跟踪 |
| `test_loss_sm` | 丢包窗口（热身、周期聚合、重传抵扣、滑窗、复位）、退避、重连状态机（401 致命、402 慢重试、503 退避、`reconnect` 立即重取 token、5s 防抖、30s 稳定清零） |
| `test_ts` | TS 复用关键字段（含同播：1–4 层 PMT 顺序与 PID、PCR 只在第一条在线层、裁剪后的阶梯、每层独立 CC 与参数集缓存、冲刷规则、层数上限）：PAT/PMT/CRC、Opus 双描述符与 channel code、PES 头（PTS/DTS、33 位回绕）、PCR/RAI、AUD、参数集补齐、Opus 控制头长度编码、ADTS、SEI（含防竞争字节）、1316 对齐与 CC 连续 |
| `test_net` | SHA-1/Base64/RFC 6455 示例、URL、chunked、ppcenter 请求/响应解析、HTTP 错误分类、超时、重定向、WebSocket（分片、ping/pong、掩码、16 位长度、close、坏握手） |
| `test_session` | 真实 libsrt 监听器 + 模拟 ppcenter + 模拟降级 WS：每次重连 token 不同、`reconnect` 标志、延迟提示在新握手生效、断线重连、401 不重试、503 退避后恢复、`LOSS_SAMPLE` 触发重连；**同播降级**：3 层 → `layers=2` 后用只含最低两层的 PMT 重握手（PCR 随之移动、新 token、被砍层返回 `LAYER_OFF`）→ 5 秒防抖后恢复 3 层，每层码率回调 |
| `interop_mediacommon` | 用 ppmmx SRT 摄取所用的 `mediacommon` MPEG-TS 解析器读取复用产物：单层 H.264/HEVC、**4 层 H.264、3 层 HEVC**（需要 Go） |
| `tests/e2e/run_local_e2e.sh` | **本地真实 ppmmx**（源码构建）+ 模拟 ppcenter 的端到端：token 校验、codec path 校验、降级 WS 鉴权、服务端重启后取新 token 恢复、错误凭据 FATAL。H.264+Opus、HEVC+AAC、**4 层 H.264 同播、3 层 HEVC 同播**均通过（ppmmx 日志确认 `treating them as a simulcast ladder`、`max layers set to N`） |

已验证 / 未验证（诚实说明）：

- ✅ 在 Windows + MinGW-w64 gcc 14 + libsrt 1.5.4 + OpenSSL 3.4 上全部通过，包括对本仓库同级 `ppmmx` 当前源码的本地端到端。
- ✅ 对 `api.pp-cdn.org:443` 做了仅 TLS 握手的探测（不发 HTTP 请求）：带 CA 文件时证书链与主机名校验通过，无 CA 时按预期拒绝。
- ⏳ **未验证**：Linux x86_64 / ARM / MIPS 上的编译与运行（开发机没有 Linux 编译器）；对**生产** ppcenter / ppmmx / `pplayer.pp-cdn.org` 的真实推流（需要测试账号，见下）。
- ⏳ 降级的“阶梯”行为（真实丢包下 mmx 逐级降码率、砍层）没有在真实网络损伤下复现，目前覆盖的是协议解析、`set_bitrate` 回调、`reconnect`、`LOSS_SAMPLE` 触发重连，以及用模拟的 `TARGET_STATE.layers` 驱动的砍层/恢复重握手。
- ⏳ 同播尚未对**生产** ppcenter/ppmmx 推过（生产 Origin 的节点是否放开 ≥2 层，ppmmx 源码里是允许的，但没有实测）。

联调生产环境：需要一个**测试账号**（appId、appSecret、一个符合 `<table>-<view>` 的流名）。建议通过环境变量传入，不写入文件；联调只发布一路测试流，不做任何写配置的操作。

---

## 7. 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| `TLS certificate check failed: unable to get local issuer certificate` | 板子上没有根证书：设置 `ca_file`（见上） |
| 状态 `FATAL` | ppcenter 返回 400/401/403：appId/appSecret/流名有误。修正后重启实例 |
| 反复 `retry in … ms`，日志 `no_healthy_origin` | ppcenter 当前没有可用 Origin，库会持续退避重试 |
| SRT 连上随即断，mmx 日志 `not configured caused publish refusal` | 流名不符合 `<table>-<view>`，或 appId 不在白名单 |
| SRT 连上随即断，mmx 日志 `path … requires h264, but the publish carries hevc` | `video_codec` 与实际码流不一致 |
| SRT 被拒绝（`SRT_REJ_PEER`） | token 缺失/过期/与 path 不匹配；确认 `codec_path` 与 ppcenter 返回的 `publishTokens` 对应 |
| mmx 日志 `decode error` / 画面花 | 输入含 B 帧或不是 Annex-B；关键帧没有 SPS/PPS（库会补，但编码器至少要在第一个 IDR 前出过一次） |
| 降级 WS 反复失败 | Origin 没有 WebRTC 基地址时 ppcenter 不返回 `whipTracks`，此时库明确提示并在无降级通道下运行；`wss` 同样需要 CA |

---

## 8. 许可证建议

本仓库使用 **MIT**（见 `LICENSE`），对嵌入式商用最友好：保留版权声明即可集成进盒子固件。

依赖许可（均与 MIT 兼容）：

| 依赖 | 许可 | 对固件的影响 |
|---|---|---|
| libsrt | MPL-2.0 | 文件级弱 copyleft。**动态或静态链接都不传染到你的代码**；只有当你**修改了 libsrt 自身的源文件**并分发时，才需要公开这些修改。需随附 libsrt 的许可文本 |
| OpenSSL 3.x | Apache-2.0 | 随附许可文本与 NOTICE 即可 |
| 测试用 Go 模块（mediacommon、astits） | MIT | 仅用于测试，不随固件分发 |

如果你更看重**明确的专利授权条款**，可以把本项目改为 **Apache-2.0**（与上面两个依赖一致）；在不涉及专利诉求的场景 MIT 更简单。无论选哪个：H.264 / HEVC 的编码专利授权是你的**编码器/芯片**的事，与本库的许可证无关。

---

## 9. 已知限制

- 同播是“**单 codec 多层**”（H.264 ≤ 4 层 / HEVC ≤ 3 层，ppmmx 的限制）；**HEVC+H.264 双 codec 同播**要起两个 `ppusher` 实例（两个 path、两条 SRT 连接本来就是独立的），各自带自己的层数。
- 不支持 B 帧；不支持 AV1。
- `ppusher-cli` 只读 MPEG-TS 输入；裸 ES 输入没有实现（库 API 本身就是 ES 级的）。
- 不支持 HTTP 代理，不跟随 ppcenter 的 3xx。
- mbedTLS 后端未实现；32 位 MIPS/ARM 上 `time_t` 等按目标平台处理，未实测。
