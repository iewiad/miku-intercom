# 语音解析与 RTC 元数据诊断

对应 [Issue #16](https://github.com/iewiad/miku-intercom/issues/16)，已于 2026-10-26 按首版候选交付结项。诊断实现经 PR #18 合并，后续 v108 已有真实语音、候选确认、目标群媒体与停止重连验收；见 [acceptance.md](acceptance.md) 和 [v108 证据](evidence/2026-10-25-v108-closeout.json)。这些结果不证明旧两次超时的底层根因，也不外推长期稳定性。

下文保留诊断字段、工具和旧现场证据的解释。日志字段属于各自接口与版本，不能把旧精确解析器的记录代替当前候选模型结果；当前语音目标流程见 [010](specs/010-voice-target-review.md)。阅读或离线分析不需要打开音频或连接板子。

## 记录边界与指令

新增日志只保留长度、计数、固定枚举、局部序号、错误码、耗时与堆状态。
不记录录音、转写正文、群名、设备/群/会话/Peer 身份、SDP、候选地址、令牌或凭据，也不透传服务端错误消息。
`cycle`、`ticket` 是本机局部操作序号；重启可重用，不能当作身份或跨端关联键。板端 uptime 与服务端时钟不能直接拼接成同一调用链。

旧 `/voice/resolve` 精确解析接口的语法为：完整指令使用 `切换到`、`切到` 或 `进入`，随后为已加入群的完整名称。以下 `voice_resolve` 字段解释该接口，不描述当前 `/voice/proposals` 的全部自然表达候选能力。
按既有规则去首尾空白，并同时考虑原句及逐次移除句末 `。.!！` 的形式；末尾标点也可能是群名的真实组成部分。
仅按目录中完整名字精确匹配，以不同群 ID 计数；不增加“请”前缀、模糊匹配、子串匹配或自动执行推荐。
`accepted_prefix` 只能是 `none`、`switch_to`、`switch_short`、`enter`，表示已识别的语法前缀，不表示切群成功。

## 服务端 `voice_resolve`

实际字段：

```text
voice_resolve input_evaluated=… utf8_valid=… input_runes=… input_bytes=… accepted_prefix=… candidate_count=… exact_match_count=… outcome=… error_code=… elapsed_ms=…
```

`input_evaluated=true` 时，UTF-8、rune 数与字节数描述服务实际得到的字符串；它们不能恢复原始语音或 ASR 过程。
`input_evaluated=false` 表示在解析字符串之前已拒绝请求；此时 UTF-8 和长度字段的默认值没有输入诊断含义。
`candidate_count` 是本次状态快照中的已加入群数，`exact_match_count` 是同一快照中匹配到的不同群数。

| `outcome` | `error_code` / 含义 |
| --- | --- |
| `success` | `none`，恰有一个精确目标；接口只解析，不执行切换 |
| `invalid_command` | `invalid_voice_command`，无效 UTF-8、超过 256 rune 或缺少完整明确指令 |
| `unknown_target` | `voice_target_unknown`，有效指令但没有精确目标 |
| `ambiguous_target` | `voice_target_ambiguous`，匹配多个不同群 |
| `invalid_input` | `invalid_json`、`body_too_large`、`unsupported_media_type` 等请求层拒绝 |
| `service_error` | `unauthorized` 或 `service_error`，状态获取失败 |

## 板端 `GROUP_VOICE_RESOLVE`

实际字段：

```text
GROUP_VOICE_RESOLVE ticket=… input_bytes=… requested=… http=… err=… api=… outcome=… target_valid=… member=… selected=… elapsed_ms=…
```

`requested=1` 只表示调用了请求入口，本地分配、校时或配置失败仍可能使其没有执行网络传输。
`http=0` 表示没有可归于本次响应的状态；`err` 是本地数值错误码，`api` 是错误信封中的白名单代码。
`target_valid` 检查返回标识的格式，`member` 检查它是否在本机目录；`selected=1` 仅表示目标获准交给后续流程。
解析获准、RTA 清理、持久切换确认、群媒体就绪是不同阶段；`matched` 或 `selected=1` 均不能代替后面三项的证据。

| `outcome` | 判读 |
| --- | --- |
| `cancelled` / `local_error` | 已取消 / 尚未进入请求入口 |
| `transport_error` / `service_error` | 请求失败且无当前 HTTP 状态 / 当前 HTTP 为 5xx |
| `permission_error` | 当前 HTTP 为 401/403，或白名单授权错误 |
| `invalid_command` / `unknown_target` / `ambiguous_target` | 当前非 2xx 响应给出的明确解析拒绝 |
| `invalid_input` / `http_rejected` | 白名单请求格式错误 / 其余非 2xx 拒绝 |
| `response_error` / `invalid_response` | 2xx 后处理失败 / 成功请求缺少有效目标 |
| `outside_catalog` / `matched` | 有效目标不在本机目录 / 有效且属于本机目录 |

`api` 白名单为 `none`、`invalid_voice_command`、`voice_target_unknown`、`voice_target_ambiguous`、`unauthorized`、`forbidden`、`invalid_json`、`body_too_large`、`unsupported_media_type`；其余为 `other`。
仅对完整、未溢出的失败 `/voice/resolve` 非 2xx 响应检查错误信封：最多 2048 字节、深度 8、64 个结构 token，检查 UTF-8、转义与控制字符，再交给 cJSON。
要求根对象中的 `error`、其对象中的 `code` 各唯一且后者为字符串；拒绝解码 NUL，错误消息不保留、不展示。该检查服务于有界信封分类，不宣称通用严格 JSON 验证。
复用 HTTP handle 时每次清零诊断状态；只有本次 perform 成功，或本次收到 header/data/finish 响应事件，才读取 SDK 状态码。
连接/发送在当前响应到达前失败时报告 `http=0`，避免把 IDF 留存的上次 200 当作本次成功；也不能用 HTTP 200 证明 RTC 连通。

## RTC 等待与停止

`RTC wait` 字段为 `cycle mode stage expected returned failed_bit result elapsed_ms last_state`。
`mode` 为 `group`、`command` 或 `conversation`；`last_state=-1` 表示尚未观察到 Peer 状态，其余为当前 SDK 的数值状态。
`stage=local_sdp_wait` 期望 `0x001`；`channel_wait` 在 group 期望 CONNECTED `0x080`，在 command/conversation 期望 DataChannel OPEN `0x002`。
仅 command 有 `session_updated_wait`，期望 `0x100`；群 CONNECTED 与命令 DataChannel OPEN 不可互相代替。
`expected` 只含就绪条件，另行监听 FAILED `0x008`；`returned` 是原始 EventGroup 位集合，可能还含启动或完成位。
`result=failed` 表示确实返回 FAILED，优先于同时返回的就绪位，映射 `ESP_FAIL`；`cancelled` 映射 `ESP_ERR_INVALID_STATE`，不伪造 FAILED 位。
缺少就绪位且未失败/取消才记 `timeout`，映射 `ESP_ERR_TIMEOUT`；`ready` 映射 `ESP_OK`。`elapsed_ms` 是本次实际等待耗时。

`RTC stop_wait` 字段为 `cycle mode stage expected observed retired result elapsed_ms last_state`。
`audio`、`peer_data` 阶段记录各次等待耗时及 `observed`/`timeout`；`cleanup` 记录整个 stop 尝试耗时及 `safe_to_release`/`retained`。
完成位为 LOOP `0x004`、MIC `0x010`、PLAYBACK `0x020`、DATA `0x040`；启动放行位为 `0x200`。
`expected` 来自实际创建的任务，部分启动失败时可较少；command 无播放任务，group 无 DataChannel 事件任务。
用 `(observed & expected) == expected` 检查完成位；不要要求原始集合恰好相等。
DONE 可先唤醒控制器，而 worker 尚在 `EventGroupSetBits` 内；只有它返回后才发布 `retired`。因此还必须确认全部期望位已退休。
`retained` 保留 Peer、事件组、锁、队列和编解码资源，后续 stop 可继续回收；多条记录不等于多个泄漏。
`safe_to_release` 只是退休门通过，发生在 close/free 之前；`RTC heap phase=stop_complete` 才观察到本地清理走完，仍不证明重连或媒体恢复。
`RTC start_failure` 的 cycle/mode/stage/err/last_state 与等待记录一起读；保留原有 20 KiB INTERNAL worker 先预约、后 `peer_open` 的启动门与所有权。

## 分配失败与堆观察

`RTC alloc_hook` 记录注册状态；IDF 6.2 只有一个失败回调槽，无 getter/unregister。本项目仅此注册方，后续组件不能默默覆盖它。
回调放入 IRAM，静态样本缓冲位于内部 BSS；构建要求 unsigned/bool 原子操作始终无锁。目标对象、链接映射的核验与主机测试分开记录。
回调不分配、不打印、不读时钟、不查询堆，也不进入任务锁或堆锁；原子 try-lock 只尝试一次，争用立即计丢弃。
`RTC alloc_sampling` 标明 `scope=process_active_rtc policy=first_pending pending_limit=8 sampled=… dropped_total=… drop_scope=boot_active_windows`。
每次 drain 前最多保存前 8 条待处理样本；缓冲满或 try-lock 争用都会丢弃。不是每周期最多 8 条，`dropped_total` 是自启动以来活动窗口的累计值，不能逐行相加。
窗口覆盖 RTC 上下文活动及保留清理期间的全进程分配失败；样本 cycle/mode 只标窗口，不证明分配调用者或失败原因。
注册前与非活动窗口不采样；没有样本不能排除失败，日志重读也不能当作新的独立失败。
`RTC alloc_failure` 记录 `cycle mode phase size_bytes caps memory_time=drain free_caps largest_caps internal largest_internal dma largest_dma psram largest_psram`。
请求大小/caps 来自真实 SDK 回调；phase 与全部 free/largest 数值在任务 drain 时取得，不能重建失败瞬间的堆状态或先后关系。
`RTC heap` 记录 `cycle phase internal largest_internal dma largest_dma psram largest_psram`；总空闲与最大连续块需同时看，PSRAM 多不能证明 INTERNAL/DMA 请求可满足。

## 离线汇总与旧证据

工具只读取显式本地普通文件；支持 UART/服务日志或 `{events:[…]}` 元数据文件。先看帮助，再运行：

```sh
python3 tools/diagnose_intercom.py --help
python3 tools/diagnose_intercom.py /path/to/private-metadata.log --output /path/to/new-report.json
```

报告分 `observed`、`hypotheses`、`gaps`，仅输出白名单字段；默认输出 JSON 到 stdout，指定文件须不存在且以 0600 创建。
输入最多 4 MiB、4096 条事件、每行 8192 字节、128 个 cycle、每周期 4 条明细；检查 `limits`、`parser` 与 `gaps` 后再解释计数。
低 DMA 筛查阈值是工具的启发式，不是某个分配的需求值，也不是因果判定；跨重启或拼接乱序时局部 cycle 计数存在歧义。
旧 run-3 的 cycle 16/17：start 到 `channel_wait` 失败约 25.2 秒，低 DMA 观测分别为 1455/1479 字节、最大连续块 608 字节。
这支持“局部内存压力候选”；旧日志缺少实际 returned/FAILED/last_state 和分配失败样本，不能证明超时由分配失败引起，也不能把约 25 秒推断当成实测等待分类。
旧验收的 26/26 是 02:47:59（Asia/Shanghai）的较早收尾快照。完整保留日志继续到 02:48:43，实际有 29 次启动、28 次完成关闭，末次仍是群媒体就绪、未按麦；机器最终停止未被覆盖。不能把较早快照算作全量结论，也不能把活跃时截断的第 29 次直接算作泄漏。
这些记录不是四个独立泄漏、零错误、长期稳定或当前候选修复有效的证明。
旧轮次未保留 ASR 转写，无法事后恢复某次未命中的确切原因；不要把它逐条归因于 ASR、前缀、群名或解析器。

## 后续故障复现参考

以下是 PR #18 时留下的复现预案，后续候选验收另见上述 v108 记录；不据此将旧超时计为已复现或已定位。排查当前语音目标时，应同时按 010 区分转写、候选、用户确认和媒体恢复。

1. 在另行安排的实机验证中确认实际烧录候选版本、服务版本与启动边界；保留镜像标识和测试时段元数据，使用一个 UART 读取者。
2. 用户从产品界面启动语音，分别观察准备、READY、解析、清理、权威当前群确认、群媒体恢复；记录本机阶段与固定分类，不保存人声或转写。
3. 用已加入群的完整名称复测成功与未命中；需要对照无明确前缀、未知目标、重名歧义时使用隔离测试目录，记录候选/精确命中数与 error_code。
4. 对照 server `voice_resolve` 与板端分类是否一致；不要凭时间邻近或局部序号强行跨端关联，保留 HTTP 无当前响应、信封不可分类等缺口。
5. 若再次出现群恢复等待，保留 wait、start_failure、相邻 heap/alloc 与同周期全部 stop_wait，直到实际 stop_complete 或明确仍 retained；不以安全释放门替代关闭或恢复结论。
6. 用户完成停止后确认界面未按麦及群连接状态；对既有元数据离线汇总，报告观察、候选原因、缺失证据，另列听感与恢复结果。

上面的字段占位符与主机失败注入样本均为合成测试 fixture；它们不代表新的实机、ASR 或音质通过记录。
