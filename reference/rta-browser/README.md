# Lingxi Realtime 浏览器 Demo（WSS / RTC）

支持授权音色选择、麦克风开关、连续语音对话、手动提交、文本输入、音频播放、用户/助手实时字幕、打断和连接清理。Demo 创建 RTIC session 后，会在正常断开、初始化失败、网络错误和页面离开时调用 `POST /rtic/v1/realtime/sessions/{session_id}/stop`，由 RTIC 负责清理 session、并发占用和配额状态。**不提供音色克隆，不修改 RTIC / RTID runtime 或协议。**

## 前置条件

- 使用 [RTID go.mod](../../go.mod) 要求的 Go 版本。
- 启动 RTIC / RTID；后端可访问 RTIC，浏览器可访问 RTID。
- 启用 Lingxi Runtime Profile 和相应 WSS / RTC 路线；account/app 有当前有效的 Lingxi grant，音色、额度和准入条件满足。RTC 与 WSS 是独立路线：选 RTC 请求 `platform_rtc/provider_rtc`，选 WSS 请求 `platform_wss/provider_wss`，不自动降级。
- RTID 服务端配置 Provider endpoint、凭据、内部调用身份及对应路线准入。不要把 Provider key 填入 Demo。
- RTIC 必须返回完整的客户端 endpoint，并且严格匹配 `/v1/realtime/sessions/{session_id}/ws` 或 `/rtc`；Demo 不再补全相对 endpoint。WSS 将 HTTP(S) 转换为 WS(S)，不允许任意 endpoint。
- 麦克风需要 HTTPS 或 localhost、浏览器授权和音频设备。RTC 需要可达的 ICE candidate / UDP 网络；SDP POST 需要 RTID CORS 允许 Demo origin 和 `Authorization` / `Content-Type`。没有额外的 browser TURN 配置或 trickle ICE API。

配置背景见 [开发文档](../../../../docs/development.zh-CN.md)。启动 Demo 不会自动开启默认关闭的 Lingxi 路线。

## 启动

在 Bash 中，从仓库根目录执行。未配置环境变量时，Demo 会使用下表中的默认值：

```bash
cd cmd/rtid
go run ./examples/lingxi-realtime-browser
```

如需覆盖默认值，可在启动前配置环境变量。采样率必须与服务端所选 WSS Profile 的 audio policy 相同：

```bash
export LISTEN_ADDR='127.0.0.1:18081'
export LINGXI_INPUT_SAMPLE_RATE='16000'
export LINGXI_OUTPUT_SAMPLE_RATE='24000'
go run ./examples/lingxi-realtime-browser
```

打开 `http://127.0.0.1:18081/`，在页面输入 RTIC 地址、RTIC API Token 和 RTI App ID。Token 仅通过请求头传给 Demo 后端，不会写入 URL 或返回给浏览器。

| 环境变量 | 说明 |
| --- | --- |
| `LISTEN_ADDR` | 监听地址，未配置时默认 `127.0.0.1:18081` |
| `LINGXI_INPUT_SAMPLE_RATE` | WSS 输入采样率，未配置时默认 `16000`，范围 8000–96000 |
| `LINGXI_OUTPUT_SAMPLE_RATE` | WSS 输出采样率，未配置时默认 `24000`，范围 8000–96000 |
| `LINGXI_WSS_DIAGNOSE_ORIGIN` | 可选的可信 RTID HTTPS origin，例如 `https://rtid.example.com:8443`；必须仅含 origin，诊断目标 host（含端口）须精确匹配，未配置时拒绝服务端 WSS 诊断请求 |

WSS 握手失败诊断会携带短期 RTID token 发起出站请求，因此其目标仅由服务端环境变量授权，不信任浏览器传入的 endpoint 或 RTIC 地址作为诊断来源白名单。诊断仅支持 WSS 对应的 HTTPS origin，禁止跟随重定向；该配置不影响建会话和浏览器直接连接。

**采样率不是 Lingxi 的固定常量。** 已有公开 Profile list 不包含 audio policy，CreateSession 也不返回音频采样率，因此 Demo 显式配置，不猜测隐藏契约。管理员必须与所选 WSS Profile 校准；RTC 音频使用 SDP 协商的 Opus，不使用这些 PCM 参数。

## 操作

1. 选择 WSS / RTC。页面从后端 `GET /api/voices?transport=...` 获取当前 app 授权的匹配 Lingxi Profile 系统音色，选择最新 revision；不显示克隆音色。后端创建会话前重新校验授权。
2. 选择音色和轮次，点击 **连接**。等待 `session.created → session.update → session.updated` 后才能发送音频或文本。WSS 与 RTC 自动模式均显式请求 `server_vad`，其中 `interrupt_response` 随自动打断复选框设置；只有 Profile 宣告 `manual` 时允许手动模式。
3. 点击 **开启麦克风**。自动模式可保持开启连续对话；关闭 WSS 麦克风补送 1 秒静音以结束 VAD 轮次。手动模式点击 **结束本轮并回复**，发送 `input_audio_buffer.commit` 和 `response.create`；关闭麦克风也会尝试提交。
4. 输入文本并发送，使用 `conversation.item.create` 的 `input_text` 后请求 `response.create`。
5. 点击 **打断回复**，立即停止本地播放；若 response 仍在生成，立即发送 `response.cancel` 并等待 `response.done` 终态，期间丢弃迟到音频和字幕。生成已结束但本地仍有排队音频时只停止播放，不发送 idle cancel。`speech_started` 也触发同样处理，语音触发时间依赖上游 VAD 事件。不会发送未支持的自定义 interrupt / session.close 事件。
6. **断开** 关闭 socket / data channel / peer，停止媒体轨、AudioContext、定时器和待处理请求，并请求 RTIC stop 当前 session；明确标记 `terminal: true` 的错误、初始化超时、网络失败、pagehide 走同样清理。普通 Provider `error` 只展示格式受限的错误 code，不展示原始 message，也不主动断连；服务端仍可按协议关闭连接。再次连接创建新会话。
7. 打断等待 10 秒仍未收到匹配的 `response.done` 时，页面提示未确认但保留连接及取消状态，不重复发送 cancel，也不假定回复已结束；收到终态后恢复下一轮操作。持续未确认时可手动断开。
8. PCM 播放积压超过 5 分钟、音频无效或设备播放异常时，停止并丢弃本轮剩余播放，保留连接，下一轮重新尝试播放。事件日志区分 `playback.error reason=queue_overflow` 与 `invalid_audio_or_device`；清空积压可能截断本轮音频，不会加速或变调播放。

## 协议依据与边界

- WSS 根据 `pkg/realtimeprotocol/lingxi_audio_v1.go`：等待 Provider 创建，再发送一次 session update；麦克风流式重采样为单声道 signed little-endian PCM16，Base64 `input_audio_buffer.append`；按配置采样率排队播放 `response.audio.delta` / `response.output_audio.delta`。
- RTC 根据 `cmd/lingxi-rtc-smoke/main.go` 和 `pkg/realtimegateway/handler.go` / `provider_rtc_bridge.go`：建立 sendrecv Opus 音频 transceiver 与有序 `oai-events` data channel，完整 ICE gathering 后向客户端 `/rtc` URL POST `{type:"offer",sdp:...}`，Bearer 使用短期 RTID token；应用返回 `{type:"answer",sdp:...}`。音频由原生远端 track 播放，事件由 data channel 收发。
- RTIC token 留在后端；浏览器仅获得短期 RTID token。后端限制请求体、响应字段、origin 和会话路径；生产 HTTP client 禁止 redirect。事件日志只记事件类型、格式受限的错误 code 和终止标记，不记录原始错误 message、音频、转写文本或凭据。
- RTC 与 WSS 均支持默认勾选的自动打断和输入转写复选框，连接期间不可修改；手动轮次禁用自动打断选项。参照 Lingxi 原生 demo，`session.update` 使用 `session.audio.input.transcription = {}` / `null` 开关转写，使用 `session.audio.input.turn_detection.interrupt_response` 开关自动打断，不指定 ASR 模型。WSS adapter 将配置归一化为 GA 嵌套格式，与 RTC 共享相同的复选框逻辑。当前页面标记为 `browser.build lingxi-session-options-v6`。
- 字幕渲染已有用户 transcription / conversation item 和助手 text / audio transcript 事件。**Provider 不发送用户转写时无法凭空生成用户字幕**；Demo 不加入另一套 ASR。助手音频转写优先于文本，避免重复展示。
- RTC 手动打断通过静音远端音轨并等待 Provider cancel 生效；不假定可以清空浏览器内部 RTP jitter buffer。自动 barge-in 依赖 Provider VAD 事件及复选框配置，由 Provider 执行取消，页面不重复发送 cancel；此规则替代上文第 5 步对 RTC 的描述。手动 RTC 提交的输入量由 Provider 音频缓冲决定。
- 当前 runtime 提供 RTC offer/answer、音轨与 data channel 桥接契约，没有发现必须修改共享 runtime 才能实现的浏览器 RTC 缺口；**不等于实际 Provider / 浏览器链路已经验证**。
- Demo 无自身访问鉴权，会代用后端 token 创建会话，仅用于本机或受控开发环境，禁止直接公开部署。刷新或断线不恢复旧会话。

## 验证

从仓库根目录执行：

```bash
cd cmd/rtid
go test ./examples/lingxi-realtime-browser
go vet ./examples/lingxi-realtime-browser
cd ../..
node cmd/rtid/examples/lingxi-realtime-browser/browser_test.js
```

没有 Node 时，macOS 可使用：

```bash
/System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc cmd/rtid/examples/lingxi-realtime-browser/browser_test.js
```

后端测试覆盖两种 transport、服务端鉴权与浏览器凭据边界、授权音色、无授权 fail-closed、endpoint origin/path 校验、诊断来源白名单与禁止重定向、无效请求/响应和环境变量。浏览器脚本测试解析页面实际 JS，覆盖 PCM 字节序、手动提交、字幕去重、单次 cancel、迟到事件抑制与过期回调。它不是真实 WebRTC / 麦克风测试。

本次 Go tests / vet 和 JavaScriptCore 脚本测试通过；Node 未安装。**没有使用真实 Provider 凭据，没有实际浏览器麦克风 / Provider WSS / RTC 端到端验证。** 上线前需分别验证两种路线的连接、双向声音、字幕、手动/自动轮次、连续打断、断网重连和权限拒绝。

## 排障

打断断连诊断：重新构建或重启 Demo 后刷新页面，连接时展开事件日志，确认出现 `browser.build lingxi-session-options-v6`。客户端主动关闭现在携带固定关闭码，RTID 的 `peer_close_code` 可区分：4000 通用客户端失败、4001 终止错误事件、4002 发送队列拥塞、4003 发送异常、4004 无效 JSON、4006 初始化超时、4007 WebSocket error；正常断开为 1000。旧版的 4005 表示 PCM 播放失败，新版不再因此断连。请同时保留页面事件日志与错误提示。仅有 1005 不能确定具体前端失败分支，也不能证明正在运行最新页面。

| 现象 | 检查 |
| --- | --- |
| 无授权音色 / RTIC rejected session | API token、app grant 有效期、相应路线开关、音色与额度；创建会话失败时 Demo 保留 RTIC 状态码并透传其错误响应体 |
| invalid client endpoint | RTIC 节点 origin 是否匹配配置、session ID / `/ws` 或 `/rtc` 路径、是否带 query / fragment |
| RTC SDP 交换失败 | 浏览器 Network、CORS、RTID RTC 准入、Provider endpoint 与 credentials |
| RTC ICE 超时 / 无声音 | SDP candidates、UDP/NAT/防火墙、Opus 协商；RTC 无自动 WSS 回退 |
| WSS 声音速度异常 | 两个采样率环境变量是否与冻结 Profile audio policy 一致 |
| 没有用户字幕 | Provider 是否发 transcription 或包含 transcript 的 conversation item 事件 |
| 麦克风不可用 / 自动播放被拒绝 | HTTPS / localhost、权限、设备；手动点击音频控件允许播放 |
| 打断未确认 / 初始化超时 | Provider / RTID 日志及页面错误 code；cancel 等待 10 秒后提示并保留连接，45 秒初始化超时后释放资源 |
