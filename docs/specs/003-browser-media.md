# 浏览器群音频契约 v1

S2 实现契约，2026-10-24；在 [002 控制面](002-control-api.md) 上增加真实 WebRTC 群音频。范围为 localhost 浏览器，不连接板子、不调用 RTA。普通群音频始终经过独立对讲服务；没有模型参与。

## 媒体与生命周期

每身份一个有效 Peer，固定绑定身份、server_epoch、当前群 ID 和 current_version。每 Peer 单个 sendrecv 音频 transceiver，单上行/下行；Opus RTP 时钟 48000 Hz，SDP 使用浏览器兼容的 opus/48000/2，优先单声道采集，无混音、转码或录像。首版无外部 STUN/TURN，服务端仅提供本机 ICE 候选；HTTP Host 与 API 同源规则沿用 002，仅限本机开发。

后端固定 Pion WebRTC v4.2.23（Go >=1.24.0），独立 `internal/media/` 包。控制层仍决定权限，不允许媒体层复制一份会漂移的群或麦权状态。应用退出先停 HTTP，再关闭全部 Peer；Pion 回调不在自身任务中等待关闭。

创建时完整收集 ICE，通过一次 offer/answer 交换 SDP，不新增 WebSocket 或 trickle 接口。媒体请求须鉴权、限定期限，禁止记录 SDP/Token/音频内容。每身份至多一个正在协商或已连接的 Peer；服务全局上限 64 个。创建失败、超时、取消和旧代次回调只能清理对应 Peer，不能关掉新 Peer。

## API 增量

所有路径以 `/api/v1` 开头，沿用 JSON 错误形状、Bearer、no-store、Host 和 API Origin 检查。

| 方法 / 路径 | 请求/响应 |
| --- | --- |
| GET `/media` | `{available:true,diagnostics_enabled,codec:"audio/opus",clock_rate:48000,server_epoch,peer_id,phase,group_id,current_version}`；只返回本身份 Peer，未建立时 peer_id/group_id 为空、phase 为 none（协商与连接阶段分别为 negotiating、connecting，成功为 connected） |
| POST `/media/peers` | `{peer_id,replace_peer_id,group_id,current_version,server_epoch,valid_until,offer:{type:"offer",sdp}}` → 201 `{peer_id,group_id,current_version,server_epoch,answer:{type:"answer",sdp}}` |
| DELETE `/media/peers/{peer_id}` | 200 `{closed:true}`，只清理本身份该 ID；同时取消尚未到达或正在协商的创建 |

peer_id 使用终端随机生成的 8–80 位 ASCII 字母/数字/`-`/`_` 标识。replace_peer_id 必须等于 GET /media 得到的当前 Peer ID（无则空）；服务端在创建开始时比较并原子占位，旧请求不能覆盖新 Peer。占位后再次核验权限与当前群；协商结束前检查占位仍属于同一 ID、期限未到、群版本未变，之后才返回 answer。替换立即令旧 Peer 失效并释放其麦权，再在锁外关闭旧连接。

valid_until 固定在首次请求，按服务时间估算，最多未来 10 秒；服务器协商等待上限 5 秒且受请求取消约束。相同 ID/相同载荷的已完成有效创建可返回同一 answer；不同载荷、已取消/结束、正在协商或 replace 冲突返回 409。DELETE 未知 ID 也保存 60 秒取消标识，防止先关闭后到达的创建复活；每身份最多保护 256 个 ID，不淘汰保护期内记录，满时 429。只有拥有该身份的凭据能创建/查询/取消其 Peer。

POST /media/peers 的 JSON 总体上限 64 KiB，其余请求仍 8 KiB。只接受音频 Opus offer；不承诺连接任意网络、视频、多轨或任意 SDP。解析失败、超限、无权限和未知字段必须明确拒绝并清理资源。

002 的 floor 请求增加可选 `peer_id`。S2 页面申请时必须携带当前已连接 Peer ID，服务器核验其绑定与就绪状态；租约记录该 ID。旧控制测试可省略该字段，但这类租约不能授权任何音频。renew/release 继续绑定原请求与版本；旧 Peer 断线回调只能撤销自己绑定的租约。

002 的 state.media_ready 是旧控制演示兼容字段，不能代表某终端媒体已可用；S2 以 GET /media 的能力、Peer 连接状态和本地播放状态分别判断。不能把全局 available 或 HTTP answer 当作可对讲。

## 权威门控与 RTP

1. 入站 RTP 与出站队列写出前，均重新核验源/目标的活动 Peer、不可变群与版本、双方当前群/成员资格、源租约 peer_id/request_id/lease_id 及当前服务时钟。未获权、租约到期、已切群或旧 Peer 包丢弃；不能依赖前端静音、控制 API 轮询或后台定时事件才停止。
2. 入队帧记录授权时的租约标识和双方 Peer 代次；不允许把旧帧改套新租约。切群成功立即失效旧绑定，持久化失败则保留旧群与原绑定；松手/取消/到期使队列帧立即不再具备写出资格。
3. 每接收端独立 Track 与 writer，队列同时限制 10 包、200ms 和 24 KiB，满时丢最旧帧。不得阻塞整个群；超过写出预算或连接失效关闭对应 Peer。清理和网络写入不在控制面全局锁内执行。
4. 下行固定 SSRC，服务端每接收端独立重写序号、时间戳和换源首包 marker；剥离源扩展头，不能直接拼接不同发言人的 RTP 头。按有效 Opus 包时长推进 48k 时间轴并处理空闲、丢包、回绕，拒绝无效包和异常时长。
5. 读取 RTCP，启用必要的收发报告；不启用可在撤权后重发旧音频的 NACK/RTX 缓存。序号连续不等于音频正确，必须验证浏览器解码后的远端声音。

门控保证未分发队列与权限隔离，不能撤回已进入网络/浏览器抖动缓冲的尾音。普通 RTP 不携带按次 lease_id，持久上行也不能绝对识别跨按次才到达的旧网络包；客户端仍须按获权代次停发。不得宣称松手后物理零尾音。切群则关闭旧 Peer、解绑远端播放，旧群连接绝不能作为新群连接复用。

## 浏览器交互

新增独立 AudioClient，媒体代次与 ControlClient.epoch 分开；普通松手不重建 Peer。用户点击“启用音频”才申请系统麦克风，track 默认 disabled；音频就绪、acquire ACK 与仍按住均满足后才开启 track。cancelFloor 首先同步静音，再异步 release；迟到授权、续租失败、到期与断线不得恢复按下。

播放使用远端 ontrack，autoplay 被拒绝时显示“点击开始收听”，用户显式恢复；页面分开显示未启用/连接中/可收听/播放受阻/连接失败。提供关闭音频、收听静音，推荐耳机防止同机双端啸叫。实际可收听且当前 Peer 已连接才开放 PTT；麦权 ACK 后才显示正在讲话。

手动切群开始或结果不明时立即静音、关闭旧 Peer、停止采集、解绑播放。取得同一操作终态及最新状态后再恢复目标群音频；隐藏页面、身份失效/接管、重启、离线同样停止。自动恢复仅在用户已选择启用音频、页面可见且控制状态已确认时进行，始终回到未按麦；权限失败或播放被阻止要提供显式重试。

## 验证与证据边界

Go 测试使用真实 Pion Peer 与 RTP，验证无麦权/无 peer_id 零转发、一对多、跨群隔离、无额外控制调用的租约到期、旧队列/旧回调、先取消后创建、RTP 换源续接和资源回收，并运行 race/vet。浏览器状态测试覆盖权限拒绝、autoplay、迟到协商、松手/失焦/切群/身份变化停发。

默认关闭的 `-media-diagnostics` 仅开放本机 `/diagnostics` 页面及其静态脚本。诊断必须明确标注“合成音源，真实 WebRTC 编解码；不代表系统麦克风/扬声器听测”。合成源由用户点击后创建，不进入普通产品默认流程；三个测试身份独立保存在诊断页自己的会话中，不复用产品身份 Token。

诊断提供三端自动矩阵及独立页面模式：测试音 440/997/1777 Hz，经同一 AudioClient 和后端，接收测量只能来自 ontrack 的远端 MediaStream。显示远端 PCM RMS/FFT、Opus codec、RTP 包/字节、绑定代次；本地 oscillator 能量不能当接收证据。固定 3 秒发音和并发抢麦是开发驱动器，不改变普通页面失焦/隐藏停发规则。

最低矩阵：A→B/C 与 B→A/C；C 切第二群后双向隔离；释放后观察尾音排空并连续 2 秒无测试频率；同时抢麦仅获权频率可收听；停止续租但故意持续发包后，到期无声音且可再获权；至少十次切群/重连检查旧声音和 Peer 残留。报告只保存代号、测试时间窗、群版本、获权、远端频率/RMS、Opus 与 RTP 统计，不保存 Token、SDP 或真实音频。

自动数字诊断与真实麦克风/扬声器听测分别记录。只有前者通过时就如实报告，不把按钮变化、包计数或宿主合成 RTP 当成完整听测。

参考：[Pion 固定版本](https://github.com/pion/webrtc/releases/tag/v4.2.23)、[Pion GatheringCompletePromise](https://pkg.go.dev/github.com/pion/webrtc/v4#GatheringCompletePromise)、[TrackLocalStaticRTP](https://github.com/pion/webrtc/blob/v4.2.23/track_local_static.go)、[浏览器静音语义](https://developer.mozilla.org/en-US/docs/Web/API/MediaStreamTrack/enabled)、[远端音频能量统计](https://www.w3.org/TR/webrtc-stats/#dom-rtcinboundrtpstreamstats-totalaudioenergy)。
