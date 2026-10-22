# ppusher 设计文档

| 项目 | 内容 |
|---|---|
| 状态 | 已实现（0.2.0 起支持 SRT 同播，见第 10 节），实现与本稿的差异见第 9 节；公共 API 以 `include/ppusher/ppusher.h` 为准 |
| 日期 | 2026-10-22 |
| 目标 | 面向嵌入式 Linux（ARM/MIPS）盒子的开源 SRT 推流端，对接 PPCDN（ppcenter + ppmmx），仅支持 SRT |
| 许可证 | MIT（仓库已有 LICENSE） |
| 对照实现 | ppobs（`frontend/utility/PPCDNPublishEndpoint.cpp`、`plugins/obs-ffmpeg/*`）、ppmmx（`internal/servers/srt`、`internal/degrade`）、ppcenter（`internal/apis/publish_v1.go`） |

本文所有协议细节均来自对上述代码的阅读，不是推测；引用处标注了出处，实现时以代码为准。

---

## 1. 目标与非目标

**目标**

1. 硬件编码器把 H.264/HEVC 的 Annex-B 码流和 Opus（或 AAC）音频帧推进来，库内轻量 MPEG-TS 复用，经 libsrt 发往 ppmmx Origin。
2. 对接 ppcenter 推流鉴权：`POST /v1/publish/requests` 换节点与 token。
3. 对接 ppmmx 降级控制通道（WebSocket）：执行码率调整，处理 `reconnect`，消费 `LOSS_SAMPLE`。
4. 重连协议：每次重连都重新向 ppcenter 取 token；本地丢包过高快速重连；指数退避。
5. 依赖极少，纯 C 库 + 一个 C 命令行示例，适合交叉编译。

**非目标**

- 不做编码、采集、转码；不支持 RTMP/WHIP/RTSP。
- 不做同一进程内的 HEVC+H264 双 codec 同播（需要两路编码器，嵌入式盒子通常只有一路）。要同播，起两个 ppusher 实例即可（两个 `/h264`、`/hevc` 路径互相独立，ppmmx 也是这样设计的）。
- 不做同一连接里的**跨 codec** 混装（ppmmx 要求同一连接单 codec）。单 codec 的多层同播（H.264 ≤ 4、HEVC ≤ 3）从 0.2.0 起支持，见第 10 节。
- 不做 P2P（ppobs 的 P2P companion 与 SRT 推流是分开的）。

---

## 2. 对接事实（读代码得到的结论）

### 2.1 ppcenter：`POST /v1/publish/requests`

出处：`ppcenter/internal/apis/publish_v1.go` `Create()`。

请求体（JSON）：

```json
{"appId":"...","appSecret":"...","streamName":"B01-frontView",
 "protocol":"srt","capabilities":["whip-hevc-h264"],"requestRegion":"可选"}
```

- `appId`/`streamName` 必须匹配 `streamNameV1Pattern`；`streamName` **不能带 codec 后缀**（ppobs 的 `ParseSRTStreamName` 专门去掉 `/h264`、`/hevc`，带了会 400）。
- `protocol` 取值 `srt`；`capabilities` 带 `whip-hevc-h264` 才会返回 `whipTracks`。

响应（SRT 分支）：

| 字段 | 含义 |
|---|---|
| `originNodeId` | 选中的 Origin |
| `ppcdnPublishEndpoint` | SRT 地址 `host[:port]`（生产 `origin.pp-cdn.org:7890`，无端口时用 7890） |
| `publishToken` | 无 codec 路径（`appId/stream`）用的 token |
| `publishTokens.{h264,hevc}` | 各 codec 路径用的 token。**mmx 把 token 绑定到精确 path**，`/hevc` 路径只认 hevc 的 token |
| `whipTracks.{h264,hevc}.{url,bearerToken}` | 仅用于降级 WS；Origin 没配 WebRTC 地址时整个字段缺失（不是错误，按"无降级通道"运行） |

错误码：400 `invalid_request`；401 `invalid_credentials`；402 `account_in_arrears`；503 `no_healthy_origin`/`credential_store_unavailable`。`authKey` 未配置时 `publishToken(s)` 缺失，此时 mmx 若未开 `srtPublishTokenRequired` 仍接受无 token 推流。

### 2.2 streamid 格式

出处：ppobs `rtmp-custom.c::inject_ppcdn_publish_token`、ppmmx `srt/streamid.go`、`srt/publish_codec.go`。

```
publish:{appId}/{stream}[/{h264|hevc}]:{token}
```

- mmx 用 `:` 切分，最多 5 段：`action:pathname[:query]`。token 作为第 3 段（`query`）；因此**拼装前必须确认 token 不含 `:`**，且整个 streamid ≤ 512 字节（libsrt 上限）。
- 也接受 `token=<value>` 形式，我们用裸 token（与 ppobs 一致）。
- path 的末段是 `h264`/`hevc` 时，mmx 会校验 PMT 里的视频 codec 是否一致（`checkTracksMatchPathCodec`），不一致直接断开（连接成功后才被踢，表现为"连上又断"）。
- **单轨也要带 codec 段**：pplayer 播放时按 `hevc → h264 → 裸 path` 回退，且 ppobs 刚改成多轨关闭时也按 codec 自动加后缀。ppusher 默认 `codec_path = auto`，由调用方声明的 codec 决定后缀，并用 `publishTokens[codec]`。仅当用户显式 `codec_path = none` 时才用裸 path + `publishToken`。
- 可选：`passphrase`（`SRTO_PASSPHRASE`）与 token 正交，都配置时都要通过。

### 2.3 token 生命周期

- token 带 `exp`（ppcenter `utils.WHIPTokenTTL`），SRT 长连接推流期间不重新校验，**只在握手时校验**。
- 设计文档要求：断线重连必须重新向 ppcenter 取 token，不能复用。ppusher 的每一次"建连尝试"都是 `fetch → connect` 一个完整循环，**不缓存 token**。
- 降级 WS 用的是 `whipTracks[codec].bearerToken`（同一套 token 格式、同一把 key，绑定同样的 path），也是每次 fetch 后更新；WS 自己的重连要用最新一次 fetch 的 token。

### 2.4 降级 WebSocket

出处：`ppmmx/internal/servers/webrtc/degrade_ws_handler.go`、`internal/degrade/degrade.go`、`docs/design/publish-degrade-protocol.zh-CN.md`。

- URL：把 `whipTracks[codec].url`（`http(s)://host/{appId}/{stream}/{codec}/whip`）派生为 `ws(s)://host/{appId}/{stream}/{codec}/ws/whip`（`https→wss`，`http→ws`，末尾 `/whip` 换成 `/ws/whip`；形态不符就放弃降级，不猜）。**path 含 codec 段**，每个 codec 一条连接；SRT 单连接单 codec，所以我们只连一条。
- 鉴权：`Authorization: Bearer <bearerToken>`（也接受 `?token=`，我们用头）。
- 客户端上行：连上后发一次 `{"type":"CLIENT_INFO","client":"ppusher","version":"x.y.z"}`（mmx 只用来打日志，可选但应做）。
- 服务端下行（JSON 文本帧）：

```json
{"type":"TARGET_STATE","path":"app/stream/h264","codec":"h264","layers":2,
 "bitrate_percent":60,"latency_ms":500,"reconnect":true,"reason":"degrade-latency-raise"}
{"type":"LOSS_SAMPLE","path":"...","codec":"h264","unrecoverable_delta":12,"expected_delta":580}
{"type":"ALERT","path":"...","reason":"layer=1,bitrate=60%,仍不合规"}
```

  - `TARGET_STATE` 声明式、幂等；WS 一连上就会推当前状态（前提 mmx 已学到 maxLayers，即 SRT 连接已建立并通过校验）。`reconnect` 是一次性标志，`omitempty`，老 executor 忽略。
  - `LOSS_SAMPLE` 每个采样周期（默认 6s）推一次，与是否发生状态转移无关；`unrecoverable_delta` 已是扣除重传后的净值，**喂给本地窗口时 retrans 传 0**。
  - 取值范围：`bitrate_percent` 60–100；`latency_ms` 仅 SRT 有意义。
- 重连：WS 断开后指数退避 2s→30s（协议文档 §7.3），推流停止时关闭 WS。
- **一个重要发现**：ppobs 的 `srt-degrade-client.cpp` 的 `ParseTargetState` **没有读 `reconnect` 字段**，也就是说现在 ppobs 实际靠 mmx 的兜底（发出 reconnect 后 2s 内客户端没断就由 mmx `c.Close()` 掐掉 SRT 连接，见 `conn.go::requestPublisherReconnect`）来完成重连。ppusher 要**主动处理 `reconnect=true`**，这样不用等 mmx 兜底，中断更短。

### 2.5 SRT 延迟（TSBPD）

- 接收延迟由 mmx 侧 `LatencyFor(path)` 在 `Accept()` 前决定，握手后不可变；mmx 降级时抬高 `latency_ms`，只能靠重连生效。
- 客户端侧：在 `SRTO_PEERLATENCY`/`SRTO_LATENCY` 上设一个不低于下发 `latency_ms` 的提示值，实际取两端较大者。保守做法：默认 `latency_ms=0`（用 libsrt 默认 120ms），收到 `TARGET_STATE.latency_ms` 后记下，**下次握手**设为 `SRTO_PEERLATENCY = max(配置值, latency_ms)`。以服务端实际协商结果为准，不做强假设。

### 2.6 MPEG-TS 复用必须满足的约束

出处：ppmmx `internal/protocols/mpegts/{enhanced_reader,to_stream,video_tracks}.go`，以及它依赖的 `mediacommon/v2 pkg/formats/mpegts`（reader.go / track.go / writer.go / substructs）。

| 约束 | 说明 |
|---|---|
| SRT 消息 payload = **1316 字节**（7×188） | mmx 的 `preDemuxer` 用 1316 的缓冲读 SRT 消息，更大的消息会被截断。所以每次 `srt_send` 只发 1316（最后不足部分用 null 包 PID 0x1FFF 补齐，或等下次再发，不能发短包乱了 188 对齐） |
| 连接开头尽快给出 PAT+PMT | `Reader.Initialize` 先读到第一个 PMT 才能得到 tracks；之后会 `Rewind` 重新解析，所以 PAT/PMT 必须在流的最前面，并周期重发（每个关键帧前，或 ≤ 500ms） |
| 视频 stream_type | H.264 = `0x1B`，HEVC = `0x24`；每个连接只能一种 codec，且与 path 后缀一致 |
| 音频 Opus | `stream_type 0x06`（private data）+ 描述符：**registration_descriptor（tag 0x05，format_identifier `Opus`）** 和 **extension_descriptor（tag 0x7F，ext tag 0x80，后跟 `channel_config_code`：单声道 0x01，立体声 0x02）**。缺任一个，mmx 判成 unsupported 并丢弃该轨 |
| Opus PES 载荷 | 每个 Opus 包前加 `opus_control_header`：`0x7F 0xE0`（+StartTrim/EndTrim/Ext 标志位均为 0）+ 长度（每 255 一个 0xFF 字节，最后一个 `len%255`）；一个 PES 可含多个 AU。**PTS 必须等于 DTS（只写 PTS）**，否则 `PTS is not equal to DTS` 被丢；stream_id 用 0xBD（private_stream_1） |
| 音频 AAC（备选） | `stream_type 0x0F`，ADTS 封装。**mmx 在 PMT 之后还要从首个 PES 里解 ADTS 头取配置**（`findMPEG4AudioConfig`），所以首个 AAC PES 必须尽早出现且是完整 ADTS 帧 |
| 视频 PES | Annex-B 访问单元；stream_id 0xE0；PES_packet_length=0（无界）；关键帧 TS 包 adaptation field 置 `random_access_indicator`；**PTS/DTS 都必须有**：B 帧要求为 0 时 `DTS==PTS` 只写 PTS 即可 |
| AUD | 每个视频 AU 前加 AUD NAL（H.264: `09 F0`；HEVC: `46 01 50`）。mmx 读取时若第一个 NAL 是 AUD 会剥掉；这是为了兼容其它播放器 |
| 参数集 | 关键帧 AU 必须携带 SPS/PPS（HEVC 还有 VPS）。硬件编码器通常已带；没带时库用缓存的参数集补在 IDR 之前（见 §4.3） |
| PCR | 以视频 PID 为 PCR PID；关键帧和每隔几个 AU 写 PCR（PCR base = DTS − 0.1s，与 mediacommon 一致） |
| 时间基 | 90kHz；PTS 取单调递增，由调用方提供（微秒）并在库内换算；音视频用同一时钟 |
| 视频轨数 | 同播：同 codec 的多路视频轨，PMT 顺序 = 层顺序（最高画质在前），H.264 ≤ 4、HEVC ≤ 3（`ValidateVideoTracks`）；超限整条连接被拒绝。单层是它的特例（见第 10 节） |
| SEI 时间戳（可选） | `OBS-ABSTS-SEI-v1`：`user_data_unregistered` SEI，payload=16 字节 ASCII UUID + 8 字节**大端毫秒** UTC。H.264 插在首个 VCL NAL **之前**（AVC 无 suffix SEI，放后面严格解码器会拒绝）；HEVC 用 suffix SEI（type 40）追加在 AU 末尾。供 EP08 端到端延迟测量，默认关闭 |

### 2.7 本地丢包快速重连

出处：ppobs `plugins/common/publish-loss-reconnect.h`、`obs-ffmpeg-mpegts.c::mpegts_sample_srt_loss`、`srt-degrade-client.cpp::HandleLossSample`。

- 默认：开启；采样周期 30s；窗口 300s；阈值 1.5%。
- 每个周期用周期内累计的 `(lost, retrans, sent)` 一次性算：`unrecoverable = max(lost − retrans, 0)`，`expected = max(sent − retrans, 0)`；**不能逐秒算再夹零**（重传滞后一个 RTT 会系统性高估）。
- **热身 = 一个完整窗口**：首个窗口填满之前不判定。命中则重置窗口并触发重连。
- 数据源优先级：降级 WS 已连上时用 mmx 的 `LOSS_SAMPLE`（接收端视角、口径与 WHIP 一致，`retrans=0`）；WS 不可用时用本地 `srt_bistats`（发送端视角 `pktSndLossTotal`、`pktRetransTotal`、`pktSentTotal`）。ppobs 里 WS 连着时本地监控让位，我们保持一致。
- 这套逻辑没有 OBS 依赖，ppusher 用纯 C 重写（算法一字不改，便于对照测试）。

---

## 3. 架构

```
 硬件编码器 / 采集线程                        主线程 / 控制线程
 ─────────────────────                   ────────────────────────────
 ppusher_push_video()  ─┐                     ppcenter (HTTP/HTTPS)
 ppusher_push_audio()  ─┤→ [TS 复用] → 环形队列 → [发送线程]──libsrt──→ Origin:7890
 (线程安全，非阻塞)       │                                ▲
                         │               [状态机线程]──────┤ 驱动 fetch/connect/close
                         │                    ▲  ▲         │
                         │   [降级 WS 线程]───┘  └ 丢包监控  │ 回调：set_bitrate / on_state / on_log
                         └──── 回调：request_keyframe（重连后需要 IDR）
```

线程模型（尽量少）：

1. **调用者线程**：`push_video/push_audio`，只做 TS 复用 + 入队，不做网络 I/O；队列满按策略丢弃（见 §4.5），不阻塞编码器。
2. **会话线程**（一个）：状态机 `FETCH → CONNECT → STREAMING → …`，同时负责从队列取 TS 包调用 `srt_sendmsg`、每秒采样 SRT 统计、驱动本地丢包监控。把发送和状态机放同一线程避免跨线程操作同一个 SRT socket。
3. **降级线程**（一个，可选）：WS 客户端的 blocking 读循环 + 自带退避重连，向会话线程投递事件（`TARGET_STATE`、`LOSS_SAMPLE`、`reconnect`）。

模块划分（`src/`）：

| 模块 | 职责 | 外部依赖 |
|---|---|---|
| `streamid.c` | codec 后缀、token 校验、streamid 拼装、SRT URL 拆分 | 无 |
| `json_min.c` | 极简 JSON 读取（按 key 取字符串/整数/嵌套对象）与字符串转义写出 | 无 |
| `ppcenter.c` | 构造请求体、解析响应、错误码映射 | `net_http` |
| `net_io.c` | socket 抽象（POSIX / Winsock）、超时、TLS 钩子 | 可选 OpenSSL |
| `net_http.c` | HTTP/1.1 POST（Content-Length 与 chunked 响应）、URL 解析 | `net_io` |
| `net_ws.c` | RFC 6455 客户端：握手、掩码、ping/pong、关闭、文本帧、分片重组 | `net_io`、`sha1`/`base64`（自带） |
| `degrade.c` | 消息解析、WS URL 派生、`TARGET_STATE` 幂等去重 | `json_min` |
| `loss_monitor.c` | §2.7 窗口算法 | 无 |
| `backoff.c` | 指数退避（带抖动）、稳定期复位 | 无 |
| `session.c` | 重连状态机（§4.2） | 以上全部 + libsrt |
| `ts_mux.c` | PAT/PMT/PES/PCR/Opus/ADTS/AUD/SEI、1316 打包 | 无 |
| `annexb.c` | NAL 切分、SPS/PPS/VPS 缓存、关键帧判断 | 无 |
| `ppusher.c` | 对外 C API 胶水 | — |

核心逻辑（streamid、json、degrade、loss_monitor、backoff、ts_mux、annexb、状态机转移函数）**不含任何 I/O**，状态机用"输入事件 → 输出动作"的纯函数 + 外层执行器，单元测试无需网络与 libsrt。

---

## 4. 详细设计

### 4.1 对外 C API（设计草案；最终以 `include/ppusher/ppusher.h` 为准，见第 9 节）

```c
typedef struct ppusher ppusher_t;

typedef enum { PPUSHER_CODEC_H264 = 1, PPUSHER_CODEC_HEVC = 2 } ppusher_codec_t;
typedef enum { PPUSHER_AUDIO_NONE = 0, PPUSHER_AUDIO_OPUS = 1, PPUSHER_AUDIO_AAC = 2 } ppusher_audio_t;

typedef struct {
  /* ppcenter 鉴权（必填） */
  const char *ppcenter_url;     /* "https://api.pp-cdn.org"，库内补 /v1/publish/requests */
  const char *app_id, *app_secret, *stream_name;
  const char *region;           /* 可选 requestRegion */

  /* 媒体 */
  ppusher_codec_t video_codec;  /* 决定 /h264|/hevc 后缀与 PMT stream_type */
  ppusher_audio_t audio_codec;
  int audio_channels;           /* Opus: 1 或 2 */
  int audio_sample_rate;        /* 仅 AAC 需要，用于 ADTS 头 */
  int video_bitrate_kbps;       /* 基准码率，用于换算 bitrate_percent */

  /* 行为开关（0 表示用默认） */
  int codec_path_none;          /* 1=不加 /h264|/hevc（不推荐） */
  int enable_degrade;           /* 默认 1：连降级 WS */
  int enable_sei_timestamp;     /* 默认 0 */
  const char *passphrase;       /* SRT 链路加密，可空 */
  int srt_latency_ms;           /* 默认 0=libsrt 默认(120) */
  int loss_reconnect_enable;    /* 默认 1 */
  int loss_period_sec, loss_window_sec; double loss_threshold_pct; /* 30 / 300 / 1.5 */
  int backoff_min_ms, backoff_max_ms;                              /* 1000 / 30000 */
  int queue_ms;                 /* 发送队列上限，默认 3000 */
  int tls_verify;               /* 默认 1；ca_file 可选 */
  const char *ca_file;
} ppusher_config_t;

typedef struct {
  void (*set_bitrate)(int kbps, void *ud);      /* 降级：新的目标码率，在会话线程回调，须快速返回 */
  void (*request_keyframe)(void *ud);           /* 每次（重）连上后调用，要求编码器出 IDR */
  void (*on_state)(int state, const char *info, void *ud);
  void (*on_log)(int level, const char *msg, void *ud);
  void *ud;
} ppusher_callbacks_t;

ppusher_t *ppusher_create(const ppusher_config_t *, const ppusher_callbacks_t *);
int  ppusher_start(ppusher_t *);   /* 启动会话线程，立即返回 */
int  ppusher_push_video(ppusher_t *, const uint8_t *annexb, size_t len, int64_t pts_us, int64_t dts_us, int is_key);
int  ppusher_push_audio(ppusher_t *, const uint8_t *frame, size_t len, int64_t pts_us);
void ppusher_stop(ppusher_t *);
void ppusher_destroy(ppusher_t *);
int  ppusher_get_stats(ppusher_t *, ppusher_stats_t *);   /* 状态、重连次数、码率百分比、最近丢包率等 */
```

要点：

- 所有 `push_*` 线程安全，未连上时丢弃（返回 `PPUSHER_E_NOT_CONNECTED`，调用者无需缓存）；重连后用 `request_keyframe` 让编码器尽快给 IDR，库在拿到第一个关键帧前丢弃非关键帧，保证 SRT 连接上的首个视频 AU 是 IDR。
- `dts_us` 允许 ≠ `pts_us`，但 B 帧为 0 的约定下二者相等；库不做重排序。
- Opus 帧给裸 Opus packet（不是 Ogg）；AAC 给**裸 AU**，库加 ADTS 头；若调用者已有 ADTS 则提供 `push_audio_adts` 变体直传。
- 配置字符串在 `create` 时拷贝；`appSecret` 只存在于内存，日志中不打印；日志里的 streamid/token 一律打码（只保留前 6 位 + 长度，ppmmx 设计文档也要求不打印完整 streamid）。

### 4.2 会话状态机（重连协议）

```
        start
          │
          ▼
  ┌──── FETCH ────────────────┐  ppcenter 请求失败（网络/5xx/503）
  │  (向 ppcenter 取新 token)   │──────────────→ BACKOFF ──(到期)──→ FETCH
  │                            │  401/400 → FATAL（上报 on_state，不重试）
  ▼                            │  402 欠费 → BACKOFF 固定慢退避（60s）
 CONNECT（srt_connect）─失败──→ BACKOFF
  │ 成功
  ▼
 STREAMING ──┬ SRT 断开/发送错误 ─────────→ BACKOFF（稳定期内已清零退避则立即重取）
             ├ TARGET_STATE.reconnect=1 ──→ CLOSING → FETCH（立即，不走退避）
             ├ 本地/LOSS_SAMPLE 窗口超阈值 → CLOSING → FETCH（立即，带最小间隔）
             └ ppusher_stop ─────────────→ STOPPED
```

规则：

1. **每次进入 FETCH 都向 ppcenter 取新的 token，绝不复用**（含 reconnect 触发、丢包触发、断线触发三种路径）。同一轮 FETCH 得到的 `publishTokens[codec]` 用于 SRT streamid，`whipTracks[codec]` 用于 WS，两者同源。
2. **退避**：`delay = min(max, min·2^n)`，叠加 ±20% 抖动；`n` 在连接稳定 ≥ 30s 后清零（防止"连上即断"的节点被打满）。ppcenter 失败与 SRT 连接失败共用同一个退避计数。
3. **主动重连（reconnect 标志 / 丢包触发）不累计退避**，但有 5s 的最小间隔，防止 mmx 与本地同时触发形成风暴。
4. 收到 `reconnect=true`：先把 `latency_ms` 记下（下次握手用），然后关闭当前 SRT socket（`srt_close`），回到 FETCH。ppmmx 在发出 reconnect 后 2s 会自行掐连接，我们优先于它。
5. **幂等去重**：`TARGET_STATE` 与上一次的 `(bitrate_percent, layers)` 相同且无 `reconnect` 时忽略（WS 重连后 mmx 会重推当前状态）。
6. 重连窗口内：`push_*` 丢弃；重连成功后第一个关键帧前的非关键帧丢弃。
7. 状态变化通过 `on_state` 上报：`FETCHING / CONNECTING / STREAMING / BACKOFF / FATAL / STOPPED`，便于盒子 UI 指示灯。

### 4.3 TS 复用器

- 状态：continuity counter（每 PID）、PAT/PMT 版本、PCR 计数、参数集缓存。
- `push_video`：切 NAL → 更新缓存的 SPS/PPS/VPS → 若 `is_key` 且 AU 缺参数集则前置补齐 → 前置 AUD → （可选）注入 SEI → 打 PES（PTS/DTS 90kHz，`DTS==PTS` 只写 PTS）→ 切 188 包，首包带 `payload_unit_start`，关键帧首包带 adaptation field（RAI + PCR），尾包用 adaptation stuffing 补齐。
- PAT/PMT：连接建立后**先写**，再每个关键帧前写，另有 ≤500ms 定时兜底（无关键帧的纯音频场景）。
- 输出：包放入 1316 字节"SRT 消息"的拼装缓冲，满 7 包即成一条消息入发送队列；`push_video` 结束时对不足 7 包的尾部用 null 包补齐后立即送出（视频 AU 边界处 flush，保证低延迟；带来的少量带宽开销可忽略）。
- PTS 单调保护：回退时钳到上一值+1 并计数告警。
- 本模块不做 PES 重排序、不处理 B 帧。

### 4.4 HTTP / WebSocket / TLS：依赖取舍

| 方案 | 优点 | 缺点 |
|---|---|---|
| **A. libcurl** | 久经考验；代理、重定向、证书校验齐全 | 嵌入式发行版（Buildroot/OpenWrt）体积大（带 TLS 约数 MB）；WebSocket API 要 curl ≥ 7.86 且长期是实验特性，旧固件基本不可用；依赖链长，交叉编译麻烦 |
| **B. 自带极简 HTTP/WS + 可选 OpenSSL（采用）** | 业务只需要"一个 POST + 一条 WS"，实现约 600 行 C；无额外库（HTTP 明文时零依赖）；WS 可精确控制心跳与超时 | 要自己维护协议细节；无代理/重定向支持；TLS 仍需外部库 |
| C. 无 TLS（仅明文） | 零依赖 | 生产 `api.pp-cdn.org` 是 HTTPS，不可用 |

**结论：采用 B。** 理由：

1. **TLS 本来就绕不开**：`api.pp-cdn.org` 是 HTTPS；而 libsrt 开启加密功能时本身就链接 OpenSSL（或 GnuTLS/mbedTLS）。用 OpenSSL 的话，盒子上没有任何新增二进制依赖。
2. TLS 通过 `net_io` 的一组钩子（`connect/read/write/close`）接入，CMake 选项 `PPUSHER_TLS=openssl|none`。`mbedtls` 后端留作后续（接口已预留，**第一版不实现、也无法在本环境验证**，README 会明确写）。
3. 生产 Origin 的 WHIP/WS 地址目前可能是明文 `ws://`（见 `whip-hevc-h264-multitrack-simulcast-design.zh-CN.md` §安全一节），所以 WS 必须同时支持 `ws://` 与 `wss://`。
4. 不支持 HTTP 代理与重定向：ppcenter 端点是固定 URL；遇到 3xx 直接报错并在日志提示。证书校验默认开启（`tls_verify=1`，可配 `ca_file`；嵌入式根证书缺失时给出明确错误，而不是静默关闭校验——ppobs 在 Windows 上关闭了校验，那是 Windows OpenSSL 无系统证书库的特例，不是我们应继承的做法）。

HTTP 客户端范围：HTTP/1.1，`Connection: close`，仅 POST，支持 `Content-Length` 与 `chunked` 响应，响应体上限 64KB，连接/读写总超时 8s（与 ppobs 的 8s 一致）。
WS 客户端范围：握手（`Sec-WebSocket-Key/Accept` 校验）、文本帧收发、客户端掩码、ping→pong、close、分片重组、帧长上限 64KB；每 20s 发 ping，60s 无任何数据判死。

### 4.5 发送队列与拥塞策略

- 队列以字节和毫秒两个维度限长（默认 3s）。超限时**丢弃整个 GOP 内的非关键帧**直到下一个关键帧，并通过计数暴露；不丢音频（体积小，丢了更难听）。
- `srt_sendmsg` 使用 live 模式（`SRTT_LIVE`）、`msgttl` = 当前 TSBPD 延迟的 2 倍、`inorder=1`。
- 发送线程每秒采样 `srt_bistats` 一次，供本地丢包监控与 `get_stats` 使用。
- 不做 libsrt 的 `SRTO_MAXBW` 限速（直播默认 BW 估算即可），预留配置项。

### 4.6 安全

- 日志打码：token、appSecret、streamid 的第三段不入日志。
- `appSecret` 不进命令行参数（`ps` 可见）：CLI 示例只接受环境变量 `PPUSHER_APP_SECRET` 或 `--secret-file`。
- 证书校验默认开启。
- 响应体、帧长、streamid 长度全部有上限；JSON 解析按"取需要的字段"，不构建 DOM，输入不可信。

---

## 5. 构建、测试与交付

**目录结构**

```
ppusher/
├── CMakeLists.txt
├── include/ppusher/ppusher.h
├── src/            (§3 模块)
├── examples/ppusher-cli.c   (stdin/UDP 读 MPEG-TS 或 ES，联调用)
├── tests/          (自带极简断言框架，无第三方测试库)
├── cmake/toolchains/{arm-linux-gnueabihf,aarch64-linux-gnu,mipsel-linux-gnu}.cmake
├── doc/design.zh-CN.md   (本文)
├── README.zh-CN.md
└── LICENSE (MIT)
```

**CMake 选项**：`PPUSHER_TLS=openssl|none`、`PPUSHER_BUILD_TESTS`、`PPUSHER_BUILD_CLI`、`PPUSHER_SRT_STATIC`。libsrt 通过 `find_package`/`pkg-config` 查找，交叉编译用 `PKG_CONFIG_SYSROOT_DIR` 与 toolchain 文件。库本身是纯 C99（不使用 C++），`-ffunction-sections -Wl,--gc-sections` 控制体积。

**CLI（联调）**

- `ppusher-cli --ppcenter https://api.pp-cdn.org --app-id X --stream Y --codec h264 --input ts:-`
- 输入模式：`ts:-`（stdin 读 MPEG-TS，库内 demux 成 ES 后重新复用，保证走同一条复用路径）、`ts:udp://0.0.0.0:5000`、`es:` 先不做。Opus/AAC 同理。
- 配合 ffmpeg 联调：`ffmpeg -re -i test.mp4 -c:v libx264 -bf 0 -c:a libopus -f mpegts - | ppusher-cli ...`。
- `--dry-run`：不连网络，只把复用得到的 TS 写到文件，用于离线核对 PAT/PMT。

**单元测试覆盖（均为纯函数，无网络）**

| 范围 | 用例要点 |
|---|---|
| streamid/token | 有/无 codec 后缀、`streamName` 带后缀被拒、token 含 `:` 被拒、>512 字节被拒、token 选择（`publishTokens[codec]` vs `publishToken`）、日志打码 |
| ppcenter 响应解析 | 全字段、缺 `publishTokens`、缺 `whipTracks`、错误码映射 |
| degrade | `TARGET_STATE`（含 `reconnect`/`reason`/老格式缺字段）、`LOSS_SAMPLE`、`ALERT`、畸形/超大消息；WS URL 派生（https/http/尾斜杠/不符形态） |
| 丢包监控 | 与 ppobs `publish-loss-reconnect.h` 同输入同输出：周期聚合、热身整窗、窗口滑动、超阈值重置 |
| 状态机 | 全部转移：401 FATAL、402 慢退避、503 退避、reconnect 立即重取 token、丢包触发、稳定 30s 复位退避、最小重连间隔、`stop` |
| 退避 | 序列与上限、抖动范围、复位 |
| TS 复用 | 188 对齐、同步字节、CC 连续、PAT/PMT 内容（stream_type、Opus 双描述符、`channel_config_code`）、PES 头（PTS/DTS 编码、视频 `length=0`、Opus 仅 PTS）、PCR PID、关键帧 RAI、AUD 前置、参数集补全、1316 消息切分、Opus control header 长度编码（含 ≥255）、SEI 位置与 payload |
| 互操作校验（可选） | 用 Go 的 `mediacommon` 写一个小校验程序读入复用产物，断言 `ValidateVideoTracks`、`Opus` 轨、`H264` AU 能被 mmx 同一套解析器接受。放在 `tests/interop/`（需要 Go，非必选，CI 里可跳过） |

**交叉编译说明**：README 提供 `arm-linux-gnueabihf`/`aarch64`/`mipsel` 三份 toolchain 文件示例，并说明 libsrt 与 OpenSSL 需先用同一 toolchain 编出 sysroot（Buildroot/OpenWrt 用户直接用其自带包）。

**许可证**：仓库已选 **MIT**。与依赖兼容性：libsrt 为 MPL-2.0（动态链接，无传染；静态链接需随附 libsrt 源码链接方式说明），OpenSSL 3.x 为 Apache-2.0，均与 MIT 兼容。只要保留版权声明，商用盒子可直接集成。

---

## 6. 验证计划与环境限制（需要用户决定）

当前开发机（Windows）的实际情况：

- WSL Ubuntu 已装但**没有 gcc/cmake/libsrt**，且 `sudo` 需要密码，我无法安装。
- MSYS2 mingw64 有 gcc 14.2、cmake、ninja、OpenSSL 3.4，**没有 libsrt**（可 `pacman -S mingw-w64-x86_64-srt` 安装）。

方案：

1. **推荐**：在 `net_io` 内加极薄的 Winsock/POSIX 兼容层，核心库跑在 MSYS2 mingw64 上做开发与单测、联调（需要 `pacman -S mingw-w64-x86_64-srt`，用户态包，不碰系统）。同时代码保持 POSIX 风格，Linux x86_64 的编译通过依赖以下任一：(a) 你在 WSL 里执行 `sudo apt install build-essential cmake libsrt-openssl-dev libssl-dev` 后我来编；(b) 你在 Linux 机器上跑 `cmake -B build && ctest`，我给出命令；(c) 加 GitHub/GitLab CI 的 Ubuntu job（不推送则只是配置文件）。
2. 在 (a)/(b)/(c) 之前，我**不会声称 Linux 已验证**，只报告 MinGW 构建与测试结果。

联调（真实 ppcenter/ppmmx → `pplayer.pp-cdn.org` 播放）：生产环境只读操作。需要**你提供测试账号**（appId、appSecret，及用于播放的 stream 名）；我不会去翻或使用生产密钥。建议 appSecret 通过环境变量传入，不写进任何文件或会话记录。

---

## 7. 实现顺序

1. 脚手架 + `streamid` / `json_min` / `degrade` / `loss_monitor` / `backoff`（纯逻辑，含单测）。
2. `ts_mux` / `annexb`（含单测，必要时加 Go 互操作校验）。
3. `net_io` / `net_http` / `net_ws`（先对本地 mock 服务测试）。
4. `ppcenter` + `session` 状态机 + libsrt 发送。
5. 公共 API、CLI、README、交叉编译说明。
6. 联调（需测试账号）。

## 8. 待确认

1. 项目名/目录确认为 `ppusher`（`D:\github\ppcdn\ppusher`）；库名 `libppusher`、CLI 名 `ppusher-cli`。
2. §6 的环境方案（MinGW 开发 + Linux 验证靠你的 sudo/Linux 机）是否可以？
3. 默认 `codec_path=auto`（单轨也带 `/h264|/hevc`）是否符合预期——这是对齐 ppobs 今天刚改的行为。
4. 第一版 TLS 仅 OpenSSL（+ 明文）；mbedTLS 后端是否需要进第一版（会增加无法在本机验证的代码）。

## 9. 实现状态与相对本设计稿的变更（0.1.0）

名称与位置：库 `libppusher`、命令行 `ppusher-cli`，目录 `D:\github\ppcdn\ppusher`（设计稿里暂用的 `ppsrt-pusher` 作废）。

相对设计稿的变更：

| 项 | 设计稿 | 实现 |
|---|---|---|
| 公共 API | `ppusher_config_t` 中有 `queue_ms`、各字段零值即默认 | `queue_kb`（队列按字节限长，嵌入式更直观）；必须先 `ppusher_config_default()` 再覆盖，避免“零值=未设置”的歧义 |
| 直连模式 | 未设计 | 新增 `direct_endpoint/direct_token/direct_ws_url/direct_ws_token`：跳过 ppcenter，直连自建 ppmmx，也用于本地端到端测试 |
| 降级 WS 重定向 | 未细化 | 会话每次取到新 token 后更新 WS 目标；WS 因“目标变更”被我们自己关闭时**立即**按新 token 重连（不走 2s 退避），只有服务端断开/握手失败才退避 |
| 音频冲刷 | 未细化 | 视频 AU 边界必然冲刷；音频不足 1316 字节的尾部等下一个视频 AU，超过 25ms 仍无新数据则由会话线程冲刷，避免纯音频场景卡住 |
| 错误分类 | 400/401/403 FATAL，402 慢重试 | 一致；另外 404/3xx 等网关异常一律按“可重试”处理，不让无人值守的盒子永久停摆 |
| CLI 输入 | `ts:-`、`ts:udp://`、`es:` | `ts:-`、`ts:udp://`、`ts:FILE`（含 `--loop`、实时节奏）；`es:` 未实现（库 API 本身就是 ES 级） |
| TLS | OpenSSL / none | 同；mbedTLS 未实现 |
| 流名限制 | 仅 ppcenter 的正则 | 新增发现：ppmmx 发布白名单要求 `<table>-<view>`（`^[A-Za-z0-9_-]{1,64}-[A-Za-z0-9_-]{1,64}$`），写入 README 与排障表 |

验证情况见 README 第 6 节。补充两点测试基础设施：

- `tests/interop/`：用 ppmmx SRT 摄取同款的 `mediacommon` MPEG-TS 解析器读取复用产物（H.264+Opus、HEVC+AAC+SEI），并断言 path-codec 不一致会被拒绝。
- `tests/e2e/`：`mockcenter`（模拟 ppcenter 的 `POST /v1/publish/requests`、发布白名单同步与 ppmmx 控制面注册，用一次性密钥签发与生产同格式的 AES-GCM token）+ `run_local_e2e.sh`，对**本地源码构建的真实 ppmmx**做端到端：token 校验、codec path、降级 WS 鉴权与 `CLIENT_INFO`、服务端重启后取新 token 恢复、错误凭据 FATAL。

## 10. SRT 同播（simulcast，0.2.0）

**需求**：同一条 SRT 连接里发多层视频，H.264 最多 4 层、HEVC 最多 3 层（ppmmx `internal/protocols/mpegts/video_tracks.go` 的上限，超限整条连接被拒绝）。

### 10.1 摄取侧事实（读 ppmmx 得到）

- `ValidateVideoTracks`：同连接所有视频轨必须同 codec；H.264 ≤ 4、HEVC ≤ 3（仅当轨数 > 1 时检查上限）。MPEG-TS 不带分辨率，**画质顺序无法校验**，ppmmx 直接把 PMT 里 elementary stream 的顺序当作 highest → lowest（日志 `simulcast layer 0: MPEG-TS elementary stream order 1`）。
- path 仍是 `appId/stream/{h264|hevc}`，token 仍按精确 path 绑定；同播不增加连接数，也不增加 token。
- 降级（`publish-degrade-protocol.zh-CN.md` §4、§7.1）：`TARGET_STATE.layers = N` 表示“保留最低的 N 层”（先砍最高分辨率层）；执行端“记录目标层数并重启推流，重启时裁剪”。ppmmx 用 SRT 会话的视频轨数学习 `maxLayers`：新会话 `N > 当前目标` 视为全新/更满的推流（重置为满载），`N <= 目标` 视为降级触发的重连（保留状态机的决定）。

### 10.2 实现

- **复用器**：`pp_ts_config.video_layers / first_layer`。第 L 层的 PID = `0x100 + L`，音频固定 `0x110`（单层时视频仍是 `0x100`）。PMT 只声明 `[first_layer, video_layers)` 的层，按层序排列；**PCR PID = 第一条在线层**。每层独立的连续计数器和 SPS/PPS/VPS 缓存（各层分辨率不同，参数集不同）。PAT/PMT 只在“第一条在线层”的关键帧时刻强制发送，避免每层一份。`pp_ts_write_video_layer(..., flush_after)`：同一帧的各层共享 1316 字节消息，只在最后一层冲刷；用户若以别的顺序推，会话线程 25ms 的陈旧冲刷兜底。
- **公共 API**：`video_layers`、`video_bitrate_kbps[4]`（每层标称码率）；`ppusher_push_video(p, layer, ...)`；回调 `set_bitrate(layer, kbps)`（对每个在线层按各自标称码率换算 `bitrate_percent`）、`on_layers(first, count)`；被砍的层 `push` 返回 `PPUSHER_E_LAYER_OFF`；统计里有 `active_layers`。`ppusher_create` 在层数超限时返回 NULL，而不是等 ppmmx 在连接建立后把连接踢掉。
- **砍层/恢复**：WS 线程把 `TARGET_STATE.layers`（夹在 `1..video_layers`）写入 `target_layers`；会话线程每个循环比较 `target_layers` 与在线层数，不一致就产生 `RECONNECT_REQ`，状态机 `CLOSE_AND_FETCH`（取新 token）后按新阶梯重握手。它**不是一次性标志**，而是每步重新判断，所以被 5 秒防抖挡住的请求会在防抖结束后自动重试（`mmx` 在砍层后紧接着又要加回时不会丢）。
- **关键帧**：每层单独的 `need_key`；每次（重）连后所有在线层都等各自的 IDR，`request_keyframe` 回调一次覆盖所有层。队列超 75% 时只丢非关键帧、按层标记。
- **CLI**：`ts_demux` 识别 PMT 里同 codec 的多路视频（≤4，HEVC 多于 3 路时忽略多出的并警告），回调带 `layer`；`--bitrate-kbps A,B,C` 给每层标称码率。

### 10.3 验证

- `test_ts`：1–4 层 PMT 与 PID、裁剪阶梯（`first_layer`）、PCR 只在第一条在线层、每层独立 CC/参数集缓存、`flush_after` 与单 PAT/PMT、层数上限与非法参数。
- `test_session`：真实 libsrt 监听器解析首个 SRT 消息里的 PMT：3 层 → 模拟 `TARGET_STATE.layers=2` → 以只含最低两层的 PMT 重握手（PCR 移到第 1 层、新 token、`on_layers(1,2)`、被砍层返回 `LAYER_OFF`、各层码率回调）→ 模拟 0.5s 后要求加回，**被 5 秒防抖延后**到约 5 秒后恢复 3 层。
- `interop_mediacommon`：`mediacommon` 读取 4 层 H.264 和 3 层 HEVC（含 SEI），每层独立解码、关键帧带自己的参数集。
- 本地真实 ppmmx 端到端：4 层 H.264 + Opus、3 层 HEVC + AAC；ppmmx 日志确认 `SRT publish contains N … video tracks; treating them as a simulcast ladder`、`max layers set to N`，重启 ppmmx 后重新取 token 恢复。
- **未覆盖**：真实丢包下 ppmmx 状态机自己砍层（本地没有网络损伤工具，砍层由模拟的 `TARGET_STATE` 驱动）；对生产 ppmmx 的同播推流。
