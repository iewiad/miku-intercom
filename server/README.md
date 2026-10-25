# 声路同行：本机浏览器群对讲

提供身份、建群/口令入群、当前群切换、PTT 麦权与 WebRTC Opus 群音频。用户显式启用音频后才申请系统麦克风；普通群语音由本机服务转发，不调用 RTA；默认入口仅供本机浏览器，板端入口需显式配置。

需要 Go 1.24.0+；浏览器状态测试使用 Node.js 22+。WebRTC 使用固定 Pion v4.2.23，依赖由 go.mod/go.sum 锁定。

```sh
cd server
go run ./cmd/intercom
```

打开[本机页面](http://127.0.0.1:8080)。默认快照为仓库 `work/intercom/state.json`，数据不进 Git。可通过 `-addr`、`-data` 显式设置；`-data` 的现有父目录必须是私有目录（0700），程序不会修改上级目录权限。默认只在本机运行，公开部署前另补 HTTPS、访问控制与配额。

使用两个独立浏览器配置或普通/隐私窗口创建不同身份。A 建立全员群，B 使用口令加入；再建立后勤组，第二次加入不改变当前群。两端点击启用音频并允许麦克风，再按住说话；检查双向声音、唯一授权、松手释放与手动切群，建议使用耳机，同机验证时将两个窗口并排保持可见。显示的是已加入成员数，不是在线人数。Token 只保存在浏览器本机存储，服务端保存摘要；群口令仅在创建后显示一次。

建群时可填写最多 256 个 Unicode 码点的职责简介，已加入群可按名称或职责筛选，再手动进入。职责只在创建时填写；筛选不切群或启用音频。旧快照可由新版本读取；已有数据升级前应保留一致副本，含非空职责的新快照不能直接交给旧二进制读取。设备 HTTPS 入口省略职责，保持原目录规模；具体边界见 [007](../docs/specs/007-group-purpose.md)。需求推荐另按 [009](../docs/specs/009-model-recommendation.md) 的显式启用和确认事务工作。

```sh
go test -race ./...
go vet ./...
node --test internal/httpapi/web/*.test.js
```

开发诊断需显式传 `-media-diagnostics`，再访问 `/diagnostics`。其中合成测试音经真实 WebRTC/Opus 后测量远端 PCM，不等于系统麦克风/扬声器听测；普通页面不会默认注入合成音。

快照保存群、成员、当前群版本与切群操作结果；麦权和媒体 Peer 在重启后清空。使用 Ctrl-C 正常停止；一个快照只允许一个服务进程。异常退出会保留 `<快照路径>.lock` 目录，必须人工确认原进程已结束后才能移除此锁，不要删除或重置仍在使用的数据。


## 可选板端 HTTPS 入口

本机浏览器继续使用原来的 loopback HTTP。`-addr` 只接受 loopback/localhost 地址，不能用于开放明文 LAN 服务。板端入口是同一进程的额外 HTTPS 监听，复用同一控制状态和媒体 Hub；默认关闭，以下五项须完整配置，否则启动失败。

| 参数 | 规则 |
| --- | --- |
| `-device-addr` | 本机活动网卡已分配的 RFC1918 IPv4 literal 与端口，端口 1–65535；不接受域名、通配、回环、公网或 IPv6 |
| `-device-cert` | PEM 服务端证书链，首张证书须在有效期内且 IP SAN 匹配设备入口 IPv4 |
| `-device-key` | 匹配的 PEM 私钥，只保存在本机受限的忽略目录 |
| `-device-udp-min` / `-device-udp-max` | 1–65535 的非空闭区间，min <= max；限制本 Hub 的 ICE UDP 端口 |

下面是示例，先把地址改为实际本机 LAN IPv4，并准备匹配的证书/私钥及 0700 数据父目录：

```sh
go run ./cmd/intercom -addr 127.0.0.1:8080 \
  -data ../work/intercom/state.json \
  -device-addr 192.168.1.10:8443 \
  -device-cert ../work/device-lan/server-cert.pem \
  -device-key ../work/device-lan/server-key.pem \
  -device-udp-min 40000 -device-udp-max 40199
```

板端 URL 为 `https://192.168.1.10:8443`，API 前缀仍为 `/api/v1`。TLS 最低 1.2；终端验证受信 CA、IP/主机名和有效期，不能关闭验证。浏览器继续用 localhost，无需安装系统根证书。所有监听会在开始处理请求前准备完成；任意监听失败或运行异常会关闭其余监听，正常退出同时关闭 HTTP、HTTPS 和媒体。

设备入口只接受精确 Host（含端口）、完全不带 Origin 的请求，并拒绝跨站/歧义 Sec-Fetch-Site；不信任转发头，不开放 CORS、静态页或诊断，也禁止 `/api/v1/devices` 注册。先在本机入口预配独立终端身份，再由板端使用 Bearer Token 访问 002/003 的鉴权 API。不要将 Token 放进 URL、命令行或日志。

启用后仅额外发布指定 LAN IPv4 的 ICE host 候选，保留 loopback 供本机浏览器使用；不启用公网候选或 STUN/TURN。候选过滤不等于保证每个底层 socket 只绑定该地址。需在测试网络确认该 UDP 范围可达；服务端协议测试不能替代板↔浏览器实音频验收。完整板端契约见 `docs/specs/004-device-lan.md`。

接口与后续顺序见仓库 `docs/specs/002-control-api.md`、`003-browser-media.md`、`docs/architecture.md` 和 `docs/development.md`。控制状态、数字音频诊断、真实听测、板端与语音切群分别验收，结果只记入 `docs/acceptance.md`。

## 主动语音切群

`POST /api/v1/voice/resolve` 使用终端 Bearer 鉴权，请求为 `{ "transcript": "切到后勤组" }`，成功返回 `{ "group_id": "…", "name": "后勤组" }`。板端和浏览器共用此接口。它只解析，不切群；真正执行仍使用 002 中的 `PUT /current-group`、同一 `operation_id` 的终态查询与持久恢复。

首版接受完整句式“切到 / 切换到 / 进入”加已加入群的完整名称，可去除句末句号或感叹号；不使用子串、模糊或模型助手回复猜测，不接受普通聊天、问句、未知群或跨权限目标。输入上限为 256 个 Unicode 字符。拒绝码为 `invalid_voice_command`（400）、`voice_target_unknown` / `voice_target_ambiguous`（422）。普通对讲音频不送模型。

浏览器提供“语音切群”按钮：明确触发后停麦并关闭群 Peer，再建立原 RTA RTC，只接受本次会话首次最终用户转写。取消、隐藏、离线、30 秒总超时和会话失败立即停止采集。提交切群前可取消，提交后只等待原操作终态。首版先提交并确认控制操作，再关闭 RTA，最后恢复群 RTC；任何阶段都不会并行创建两个 Peer。若后台已切群而 RTA stop 失败，界面明确显示请求已提交、会话清理待确认，保持暂停而不提供撤销。RTA stop 获得网关成功应答后，代理还必须 GET 同一 session，核验返回 session_id 匹配且 state 为 ended / failed / expired；只有确认远端终态且本地 Peer 关闭后，才允许核实当前群并恢复群音频；202 只代表已受理，不作为结束证据；非终态、查询失败或返回身份不符均继续阻塞并重试 POST stop + GET 状态。恢复始终未按麦。首次部署及页面恢复时也先查询并清理上次遗留会话。

浏览器 RTA 默认关闭；`GET /api/v1/voice` 返回 `available:false`，页面明确显示尚未启用。开发者显式准备一个 0600 私有 JSON 文件，并在已有启动参数后增加 `-voice-config ../work/private/voice.json`。文件示例仅展示结构；按已跑通板端的网关、授权和 Profile 填写，勿提交真实文件或把 Key 写在命令行中：

```json
{
  "rtic_url": "https://miku-rtic.qiniuapi.com",
  "token": "<existing development API key>",
  "app_id": "<authorized app id>",
  "rtc_origin": "https://<explicitly trusted RTC endpoint host>",
  "model_profile": "qwen3.5-omni-plus-realtime",
  "role_profile": "default_assistant",
  "voice_profile": "Tina",
  "region": "cn-east"
}
```

`rtic_url` 必须是 HTTPS origin，不带路径、用户信息、查询参数或片段。RTC endpoint 的授权策略必须明确选择一种，默认不开放动态域：

- 固定节点：配置 `rtc_origin` 为完整 HTTPS origin，返回 endpoint 的 origin 必须精确匹配。
- 动态数字节点：省略 `rtc_origin`，改配 `"rtc_node_domain": "mikudns.com", "rtc_node_port": 7882`。仅允许 `https://<1–63 位纯数字>.mikudns.com:7882`；单层节点名、完整域后缀和显式端口均严格核验。非数字节点、多层子域、伪造后缀、错端口、缺端口或带前导零的端口均拒绝。两个模式互斥，不能同时填写。

两种策略都要求收到的原始路径严格为当前 `/v1/realtime/sessions/{session_id}/rtc`，不接受短链、别的 session、点段归一化、userinfo、query 或 fragment（包括空的 `?` / `#`）；禁止跟随重定向。若网关返回相对 URL，它按 RTIC origin 解析后仍必须符合所选策略。这里沿用板端已跑通的 legacy `input_audio_transcription`、`qwen3-asr-flash-realtime`、`semantic_vad` 配置；不自动切到 reference 中另一套 Lingxi Profile 或 GA 嵌套配置。命令会话在 DataChannel `open` 后发送一次 legacy `session.update`，不依赖上游先发送 `session.created`；若收到后者也复用同一次初始化。只在 `session.updated` 确认后开麦。页面区分准备麦克风、收集连接信息、连接语音服务、等待配置确认和听取指令。命令会话不发欢迎语，关闭自动回答，不播放助手音频。实际 Profile 对这些选项的支持与最终转写仍需真实 RTA 验证。

浏览器只访问本地 `/voice/sessions` 建会话、`/voice/sessions/{command_id}/offer` 换 SDP、`/voice/sessions/{command_id}/stop` 清理；所有接口按身份绑定。API Key 和短期 endpoint Token 均不返回浏览器。WebRTC 音频和事件直接走配置中的 RTA 路线，本地代理只做 HTTP 信令；此路径仍需可达的 RTA ICE 网络。

服务端在 `<data>.voice.json` 留存私有原子会话 journal，绑定 RTIC origin、app 与完整 RTC 授权策略（固定 origin 或数字节点域及端口）；不写 Key、短期 Token、SDP、转写或录音。先写待确认记录再请求创建；重启后只清理旧会话，不恢复为新语音命令。正常退出尝试 stop 全部已知会话；浏览器消失后 45 秒开始后台 stop，失败每 5 秒重试。**创建请求结果不明且没有 session_id 时，无法证明是否已创建，服务和页面保持阻塞；必须先在网关确认并处理该悬挂会话，再由开发者修复 journal，不能通过删除文件或重启来声称已清理。** 更换绑定配置同样要求先解决旧 journal；只有所有记录均已 terminal，才可由开发者归档旧 journal 后使用新策略，不能遗弃待确认记录。异常退出的私有存储锁沿用上文人工核实进程规则。

每身份仅一个活动代理会话，全局上限 32 个；历史 command_id 保护记录上限 4096，不静默淘汰。已结束或先 stop 的 command_id 不得复活；新语音操作使用新随机 ID。数值、控制与 TLS 代理测试不替代真实转写、单 Peer 真机资源切换或真人听测。

## 按需求推荐已加入群

默认 `GET /api/v1/recommendations` 返回 `available:false`。原有 `-voice-config` 只开启语音信令；有已批准的同一份私有配置后，另外传 `-recommendation-model` 才开启真实模型推荐。缺少配置时明确启动失败，不使用关键词、模拟推荐或其他 Provider 兜底。

```sh
go run ./cmd/intercom -addr 127.0.0.1:8080 \
  -data ../work/intercom/state.json \
  -voice-config ../work/private/voice.json -recommendation-model
```

推荐器复用该配置的原网关、RTC 节点授权策略和模型 Profile，服务端用 Pion 发送需求和后台生成的授权候选 JSON。创建推荐会话显式设置 `enable_search=false`，DataChannel 请求 `modalities:["text"]`、关闭自动语音轮次、最多 512 输出 token；不使用 tools、结构化输出 API 或置信度字段。真实原 Profile 的有界文本终态已独立验证，结果见 [模型证据](../docs/evidence/2026-10-25-model-text.json)，不代表推荐准确率或板端验收。

推荐器没有麦克风、音频解码器或播放路径。RTC 初始化只发送固定 Opus 静音，收到远端音轨只丢弃 RTP；普通群音频不进入推荐器。最多接收 256 个事件，每个 32 KiB，最终文本最多 4096 B；只接受同一 response ID 的 `completed` 终态。音频 DataChannel 事件、模型错误、超限、未知候选和不合法 JSON 均失败。请求模型时间上限 20 秒，清理另用独立 7 秒总期限反复查询同一个会话终态；结果与清理都确认后才发布建议。监听写期限 40 秒容纳该流程，浏览器请求期限仍有界。

推荐会话使用原 `<data>.voice.json` journal 中的独立内部身份 `recommendation_model`，全局串行执行；清理尚未确认时保留原会话并拒绝新推荐。重启只清理遗留会话。journal、候选/需求和模型输出的处理边界及确认端点见 009；创建结果不明的处理沿用原 journal 规则，不能通过删文件绕过。

独立宿主验证命令只操作新建的合成身份与群，不开启浏览器、音频设备或板端。私有目录须先建好并设为 0700；配置直接读取原文件，勿复制 Key。默认只做一个文本能力探测；`-through-control -confirm` 明确确认合成建议，并校验 HTTP 推荐、持久控制事务、重放和服务重建后的终态恢复。`-state` 必须是新的空快照，命令拒绝复用已有控制数据：

```sh
go run ./cmd/recommend-probe -voice-config ../work/private/voice.json \
  -journal ../work/model-probe/cleanup.json \
  -through-control -confirm -state ../work/model-probe/new-synthetic-state.json
```

命令仅输出白名单布尔值、字节数和事件类型，不输出 Key、会话标识、群标识、需求或模型理由。`confirmed_and_replay_verified` 表示本次合成控制链路通过；用户交互、语音需求、实际群音频恢复和板端触控仍需分别验收。
