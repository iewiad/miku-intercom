# 开发指南

PR #10 已合并，S3 首切片的镜像已通过板↔浏览器双向数值、PTT 取消与短时故障恢复验证。[005 板端已加入群切换](specs/005-device-multigroup.md) 已完成本轮多群数值、十次连续切换及软重启恢复验证。用户确认显示正常；主要串口驱动不代表完整触控验收，两次软重启不代表实际断电。本分支补齐板端口令键盘、主动语音切群、浏览器信令代理与启动工具；实际 RTA、取消恢复与最终人工验收分别记录。实际证据见 [acceptance.md](acceptance.md)，控制接口见 [002](specs/002-control-api.md)，媒体接口见 [003](specs/003-browser-media.md)，板端 LAN 增量见 [004](specs/004-device-lan.md)。S1 控制面评审记录见 [PR #5](https://github.com/iewiad/miku-intercom/pull/5)。

## 启动浏览器群音频

需要 Go 1.24.0+ 和 Node.js 22+（Node 仅用于浏览器状态测试）。媒体使用固定版本 Pion WebRTC；首次构建会下载 Go 模块，版本由 go.mod/go.sum 锁定。无需七牛 Key、外部模型、数据库或 ESP-IDF。

从仓库根目录执行：

```sh
cd server
go run ./cmd/intercom
```

打开[本机页面](http://127.0.0.1:8080)。建立身份和群，点击“启用音频”并允许系统麦克风；连接及播放就绪后再按住说话。远端播放受阻时按页面提示主动开始收听。使用耳机可避免同一电脑多端播放产生回声。

两个成员可用独立浏览器配置、普通/隐私窗口，或分别打开 `http://127.0.0.1:8080` 与 `http://localhost:8080`（这两个来源的身份存储独立）。A 建立全员群，B 用口令加入，两端启用音频。检查 A→B 与 B→A、快速松手和同时抢麦；再建立后勤组，加入第二群不会打断当前群，主动切群才重新连接媒体。第三成员需独立浏览器身份，验证一对多与跨群隔离。同机验证时将两个窗口并排保持可见；隐藏的产品页面会关闭媒体。成员数仍为已加入人数，不是在线人数。

普通页面隐藏、失焦、断线或切群会停发；隐藏时关闭媒体，回到页面后按确认的当前群恢复，始终回到未按麦。每次系统授权、播放失败都需以页面实际状态处理，不能凭 HTTP answer 宣称连接可用。

默认状态写在仓库 `work/intercom/state.json`，已忽略。`-addr` 可设置本机监听端口，`-data` 设置快照；现有数据父目录需为私有目录（0700），程序不修改上级权限。单快照只允许一实例，正常退出释放锁；异常锁需人工核实。重启保留群/成员/当前群，清空麦权与媒体。默认仍只开放 localhost/回环 Host 与 ICE。S3 通过完整显式参数增加指定 LAN 设备入口，见下文；不启用 TURN 或公网访问。

## 无板音频诊断

开发诊断默认关闭，需要显式启动：

```sh
go run ./cmd/intercom -addr 127.0.0.1:18792 -data ../work/media-diagnostics/state.json -media-diagnostics
```

打开[媒体诊断页](http://127.0.0.1:18792/diagnostics)，点击页面按钮才启用合成测试音和远端 PCM 检测。它使用独立测试身份和真实 WebRTC/Opus 链路，不读取产品身份或 RTA Key，不录音。三个逻辑端同页的自动矩阵与独立浏览器页模式分别标注；报告中的接收频率/RMS来自远端 ontrack，不能用本地音源或单纯包计数替代。

矩阵覆盖双向、一对多、跨群、松手排空后的静音、抢麦、租约到期与重复切换/重连。独立单端模式还可显式选择真实系统麦克风，沿用普通页面的 AudioClient 与麦权流程；两个窗口保持可见，使用各自身份加入同群，按麦后观察远端 PCM 和 Opus 解码统计，再交换方向。真实模式默认收听静音，人工听测时再主动开启收听并使用耳机。报告分别记录音源、采集状态、测量窗口及远端指标，不保存声音或设备标识。

固定语句测试可在独立单端选择“本机合成语音 WAV”，载入本机生成的 RIFF/WAVE 文件（不超过 2 MiB、12 秒，单声道或双声道），再启用并加入测试群。两个可见窗口各用独立身份，点击“申请并发送一次合成语音”后，只有正常获权才从头播放一次，时长加 250 ms 后自动放麦、最多 12 秒；取消、失焦、隐藏、离线、切群或关闭都会停止，包括音频恢复仍在等待的阶段。该模式不申请系统麦克风，文件只在页面内存中解码，语音经原群媒体链路发送；报告使用 `synthetic_speech`，仅包含聚合 PCM、按麦窗口和最近 200 条远端 RMS 标量，不保存声音或文件名。

macOS 可离线生成固定测试样本，再在页面中选择：

```sh
say -v Tingting -r 180 -o /tmp/shenglu-test.aiff '声路同行，群语音测试。一，二，三。测试结束。'
afconvert -f WAVE -d LEI16@16000 -c 1 /tmp/shenglu-test.aiff /tmp/shenglu-test.wav
```

板端同时入群时，浏览器收到的远端 PCM 与板端解码指标分别记录。要验证物理扬声器，可将浏览器收听静音、音源禁止本地监听，再用电脑麦克风观察板端出声；仅在明确启用的测试时段采样，保留包络统计并丢弃原始 PCM。固定合成语音及声学检测都不能单独作为人工听清、回声或精确延迟的结论。

没有人说话时可能只有静音；采集就绪、传输/解码、有声 PCM 和人工听清分别记录，不能只凭 RTP 增长判定说话成功。合成音矩阵与真实麦克风结果分别验收。详见 [003](specs/003-browser-media.md)。

真实麦克风原生隐藏验证使用被动生命周期日志：先用系统 UI 切走标签、等待并返回，再读取报告。核对可信 hidden/visible 事件、旧 track ended / Peer closed、新 Peer 就绪且未按麦；单纯失焦只验停发，不能当作隐藏。直接读取或激活测试标签可能影响观察时序，报告保留最多 128 条白名单快照及丢弃计数。

## S3 板端 LAN 开发入口

这是开发配置步骤，不能作为实际互通或人工听测通过的证据。浏览器继续使用 localhost HTTP；不要把 `-addr` 改成通配或 LAN 地址。先在受控测试网络准备本机活动网卡的 RFC1918 IPv4、短期开发 CA、匹配该 IP SAN 且在有效期内的服务端证书与私钥。私钥和运行数据放在仓库忽略的 `work/device-lan/`，目录权限 0700；不安装系统根证书。

从 `server/` 执行；`INTERCOM_DEVICE_IP` 是开发者自行设置的本机 LAN IPv4，下面不包含机器实际地址或凭据：

```sh
go run ./cmd/intercom -addr 127.0.0.1:8080 \
  -data ../work/intercom/state.json \
  -device-addr "${INTERCOM_DEVICE_IP}:8443" \
  -device-cert ../work/device-lan/server-cert.pem \
  -device-key ../work/device-lan/server-key.pem \
  -device-udp-min 40000 -device-udp-max 40199
```

五项设备参数必须完整：`-device-addr`、`-device-cert`、`-device-key`、`-device-udp-min`、`-device-udp-max`。地址须为本机已分配的规范 RFC1918 IPv4 literal:port，不能用域名、通配、回环、公网或 IPv6；端口与 UDP 上下界须有效。错误配置、证书或任一监听失败会使整个启动失败。

设备 HTTPS 最低 TLS 1.2，要求精确 Host（含端口）、无 Origin，并拒绝跨站或歧义 Sec-Fetch-Site；不信任转发头、不开 CORS。该入口只有鉴权 API，不提供页面、诊断或 `/api/v1/devices` 匿名注册。先通过本机入口预配独立板端身份，并取得目标群的入群口令，再将 `device_id`、Token 和首次入群口令写入私有配置。API 前缀仍为 `/api/v1`，板端配置 URL 不含此前缀。

ICE 仅增加所选 LAN IPv4，保留 loopback 供同机浏览器使用，UDP 使用给定闭区间。候选过滤不等于每个底层 socket 都只绑定该 IP；在测试网络按该范围核对 UDP 可达性，不增加其他网卡、公网候选或 STUN/TURN。

固件默认 `CONFIG_MIKU_INTERCOM_DEFAULT_MODE=y`：等待私有 USB 配置与显式启动，使用已有 Wi-Fi 配置或 USB 配网，不因缺少群配置自动启动 RTA。需要原 RTA 时在 `Miku RTI device` 菜单关闭该项，重新构建并只刷应用区；原 `rti_auth` 与群配置独立，两个模式互斥。

USB 配置文件必须归当前用户所有且仅本人可读写（0600），包含 `server_url`、`ca_pem`、`device_id`、`token` 四个非空 ASCII 字符串，`group_code` 可省略或为空以使用板端键盘入群；旧预配口令保持兼容。原文件最多 16 KiB，工具压缩编码后的 JSON 总计最多 2000 B，CA 的 JSON 换行转义也计入；单字段限制见[固件 README](../firmware/esp32-s31-rti/README.md#group-mode-usb-configuration)。只写 CA 证书，不写私钥；板端验证 CA 链、IP/主机名与证书有效期。

激活 ESP-IDF Python 环境并停止其他串口读取者，从仓库根目录执行：

```sh
python firmware/esp32-s31-rti/tools/intercom_config.py \
  --port /dev/cu.usbserial-XXXX \
  --config-file work/device-lan/board.json --restart --sync-time
python firmware/esp32-s31-rti/tools/intercom_config.py \
  --port /dev/cu.usbserial-XXXX --status
```

工具不回显配置内容；不要把 Token 放入命令行、环境变量、日志或手工串口粘贴。`--restart` 只允许与配置文件一起使用，`--sync-time` 在重启后发送电脑当前 UTC。联网后会自动 SNTP 校时；无外网时重启后仍可执行单独的 `--sync-time`，再在板上显式启动；配置状态不代表时钟、TLS 或媒体已就绪。

```sh
python firmware/esp32-s31-rti/tools/intercom_config.py \
  --port /dev/cu.usbserial-XXXX --sync-time
```

构建与升级使用[固件 README](../firmware/esp32-s31-rti/README.md#toolchain) 的 `app-flash`，独立校验本次镜像；保持现有分区布局，不执行整片擦除或常规全量 `flash`。应用区写入不覆盖 NVS。群模式遇到 NVS 无空页/版本不兼容会停止并要求显式修复，不自动擦除；RTA 回退模式保留原异常处理。

群会话在同一身份/配置内复用单个 HTTPS client handle，每次设置 GET/POST/PUT/DELETE 及请求体，结束后清除请求体/响应指针；失败或不可复用的响应会关闭 handle；HTTP helper 不自动重放写请求，005 切群 worker 另按固定 NVS 操作查询与重放。群模式约 24.2 KB 非 DMA 音频工作区显式使用 PSRAM 且无 internal fallback；I2S 驱动仍用 DMA，AEC DMA 数组仍在内部内存，RTA 原 `malloc` 路径未改。

以下是 PR #10 首切片历史证据，不是当前多群镜像的验收：该镜像已完成 app-only 写入和独立 verify。浏览器→板端为 3010 ms/30 次 enabled 采集观察，源 RMS 0.40566，板端远端累计峰 0.519934；板端→浏览器 PULSE6 发送 299 包，浏览器远端累计峰 0.0075，松手后发送计数不再增长。120.26 秒正常观察中 HTTP 请求计数 7→124、TLS 连接数为 1、失败为 0；计划服务中断另引入 7 次连接错误，重连后新 epoch/Peer、同群同版本、未按麦 TX=0。不能将正常窗口的零失败写成全程零错误。三次板端 Peer 生命周期无 panic，停止后浏览器 open Peer=0、capture=none；Wi-Fi 和 RTA 凭据保留。完整镜像标识、内存数值及范围见[最终证据](evidence/2026-10-24-device-group.json)和[固件 README](../firmware/esp32-s31-rti/README.md#group-session-resources-and-current-evidence)。这些通过项不替代真人听测或长期稳定性验收。

## 板端已加入群切换（005，本轮数值与软重启恢复已验证）

先完成临时 CA/TLS、USB 身份配置和有效校时（自动 SNTP 或 `--sync-time`），显式连接群音频。板端下拉列表只包含后台确认的已加入群；选择不会自动提交，点击切换才执行，当前群名称以权威 state 为准。展开列表取消 PTT。目录按 `view_generation` 标识，旧视图提交拒绝；目录、ID 和 UI 副本在 PSRAM，最多 128 条，非法或超限整体失败。

工具前提是板端已校时、已显式连接、目录已知，且打开串口没有造成复位。软件虽先设置 DTR/RTS=false，**当前 Mac 实测独立打开串口仍会复位板子**；本轮真机使用一条持续打开的串口会话完成校时、START、GROUPS 与 SWITCH。下列是满足前提时的独立工具入口，不会自动连接或校时；不能在会复位的环境中照抄连续调用并沿用旧代次。停止其他串口读取者，从仓库根目录执行：

```sh
python3 firmware/esp32-s31-rti/tools/intercom_config.py \
  --port /dev/cu.usbserial-XXXX --groups
python3 firmware/esp32-s31-rti/tools/intercom_config.py \
  --port /dev/cu.usbserial-XXXX \
  --switch-group "${INTERCOM_GROUP_ID}" --view-generation "${INTERCOM_VIEW_GENERATION}"
```

`INTERCOM_GROUP_ID` 和 `INTERCOM_VIEW_GENERATION` 须由开发者明确设置为目录中的目标 ID 和当次整数代次；工具不会自动选群或刷新代次。`--groups` 只打印经过校验的 `view_generation/current_group_id/current_version/known/switching/groups[id,name]` 字段，最多 128 条，串口响应按完整行分段累积，上限 64 KiB。它不回显原始日志、Token 或口令；两种群操作不读取凭据文件，不能与配置、重启、校时或 `--status` 混用。现有 `--status` 仍仅报告配置是否存在。若开口复位，必须重新校时并显式连接；`known:false` 的未就绪目录会拒绝切换。长会话使用 `INTERCOM_TIME_SET`、`INTERCOM_START`、`INTERCOM_GROUPS`、`INTERCOM_SWITCH N ID` 协议，工具未提供自动建立该会话的功能。

切群回执 `REQUESTED` 只说明控制器受理。再次查看目录并结合后台当前群/版本和板端媒体就绪状态确认结果，不能把回执或群名变化单独当作音频已连接。接受意图时先同步暂停 TX、RX 入队及 PCM 播放准入，再释放麦权、关闭旧 Peer；已经进入网络/I2S 的尾音不可撤回。

首次 PUT 前将固定操作号、目标、预期版本及服务 URL/身份绑定写入 NVS 并 commit。超时、Stop/Start、断网、服务重启和板端重启不替换操作；先查原 ID，404 再按原载荷重放。终态不明不建媒体；取得同一操作终态和最新 state、成功清理日志后，才按最新当前群重建单 Peer，恢复为未按麦。配置与日志的服务/身份不匹配时保留日志并暂停，需恢复原配置，不能自动清除日志或猜测回原群。

本轮最终多群镜像完成 app-only 与独立 verify。三群 A/B/C、单浏览器合成 440 Hz 且播放静音：同 A/B 板端 RMS 为 0.071842/0.071816；板 B 听 A、板 C 听 B 时累计 remote_rms_peak=0。持麦 A→B 接受后 pressed/held=false、新 Peer TX=0。十次连续切换 version 3→13 均 ready、未按麦、TX=0，记录内部可用 heap 48,523–50,447 B；未知目标本地拒绝仍为 version 13，旧 view 同样拒绝。

SWITCH+STOP 后 pending 保留，软重启读回相同 operation_id；START 确认至 A/version 14，日志清理后再次软重启 pending=false。共 16 次 Peer start/close 无 panic，停止后内部可用 heap 152,227–152,243 B，最终启动保持停止为 152,711 B，Wi-Fi/RTA 凭据保留。这是两次软重启，未做实际断电；用户确认当前群/下拉/按钮显示正常，但切换路径主要由长串口会话调用同一控制器，不标完整触控验收。

该版本将新操作的预期 GET operation=404 也计入 http_failures，正常 13 次操作对应 13 次非 2xx，不能声称零 HTTP 失败。镜像标识及范围见[本轮多群证据](evidence/2026-10-24-device-multigroup.json)；与 PR #10 首切片独立，不构成人工听测、长期稳定性、板端口令键盘或语音切换通过。

## 自动验证

在 `server/`：

```sh
go test -race ./...
go vet ./...
node --test internal/httpapi/web/*.test.js
```

Go 测试覆盖控制语义与真实 Pion RTP 传输、隔离和生命周期；Node 测试覆盖客户端状态、媒体权限、迟到请求及关闭。浏览器诊断补充真实编码/解码证据，普通页面交互再验 PTT 失焦/隐藏与播放重试。不要以 `reference/rta-browser/` 的旧命令或原 RTA 语音回复替代这些验证。

从仓库根目录运行无板 HTTP 复用回归（需要 Python 3 与系统 `cc`，无需 ESP-IDF）：

```sh
python3 firmware/esp32-s31-rti/tests/run_http_client_reuse.py
```

该脚本提取当前生产 `request`、请求体解绑和 client 清理函数，用 SDK double 编译执行；覆盖 GET→POST acquire→POST renew→GET→DELETE 复用、Content-Type/空请求体语义、临时指针解绑，以及失败/不完整/超限/无效 JSON/服务端关闭后的清理，并运行 `GROUP_CONTROLLER_HOST_TEST` 状态回归。它不打开网络、读取凭据或连接板子，不验证真实 TLS、ESP 内存分配、音频或长期稳定性。

新增多群主机回归同样从仓库根目录运行；`--sanitize` 仅用于日志测试脚本：

```sh
python3 firmware/esp32-s31-rti/tests/run_group_cli.py
python3 firmware/esp32-s31-rti/tests/run_group_media_gate.py
python3 firmware/esp32-s31-rti/tests/run_group_journal.py --sanitize
python3 firmware/esp32-s31-rti/tests/run_switch_resolver.py
```

| 入口 | 实际覆盖 | 明确边界 |
| --- | --- | --- |
| `run_group_cli.py` | 提取生产 C parser/dispatch；Python 参数、分段目录白名单、uint32/ID/CR/多余字段、凭据隔离、DTR/RTS 与 REQUESTED 语义 | 串口/控制器为 double，无真实 UART、触控或网络 |
| `run_group_media_gate.py` | 提取生产发送/接收准入、不可逆暂停、迟到授权、到期、晚到包入队与 RTA 路径隔离 | 不执行 playback_task 的出队/解码/I2S 调用点，也不验证真实并发或硬件尾音 |
| `run_group_journal.py --sanitize` | 编译生产日志存储；NVS double 注入提交前/提交结果不明错误、损坏 blob、身份/载荷冲突、终态与旧状态；ASan/UBSan | 不验证真实 NVS/Flash 原子性或实际断电；不是完整 HTTP worker/媒体集成测试 |
| `run_switch_resolver.py` | 提取生产 resolver/终态解析/STOP/session 检查并用真实 cJSON；脚本化 HTTP 404→固定 PUT 重放、最新 state、拒绝/迟到、日志清理门禁和配置绑定 | 请求、state 应用与日志边界为 double；无真实网络、RTOS 调度、媒体或 NVS 持久性 |

resolver 还需要已存在的 `firmware/esp32-s31-rti/managed_components/espressif__cjson/cJSON/cJSON.c` 和 `.h`；干净检出可用 `--cjson-dir "${CJSON_DIR}"` 指向已下载的 cJSON 源码目录，脚本不自动下载。其余仍需系统 `cc`，无需启动 ESP-IDF。

这些入口不连接板子、读取凭据、录音或访问网络；通过只证明列出的主机范围，不能记为多群真机、听测或长期稳定性通过。

## 按切片推进

| 顺序 | 交付物 | 完成条件 |
| --- | --- | --- |
| S1 已通过 | Go 控制面、浏览器、身份/群/版本/麦权 API | 契约与状态测试通过，两身份控制流程可运行 |
| S2 浏览器 | WebRTC 媒体契约、浏览器实音频、服务端门控 | 控制、Opus/PCM 数值、生命周期与人工听测分别记录 |
| S3 首切片及 005 本轮数值已验证 | 固件群客户端、显式 LAN/TLS、已加入群选择与生命周期管理 | PR #10 历史证据独立；005 软重启/串口驱动不替代完整触控、断电、听测及长期稳定性 |
| S4 首版实现 | 板端键盘、明确群名主动切换、RTA 终态确认与恢复 | 自动与真机证据见 006 / acceptance；真人转写、十次语音切换与听测待最终验收 |
| S5 增强 | 按需求推荐协作群与确认 | 独立模型证据、候选权限、过期/重复确认验证 |

S3 复用已定义的 S1/S2 群控制与媒体接口，不能把 RTA AI 音频桥当群转发。真机指标用实际测试确定，不预报延迟、资源、局域网以外可达性或听测结论。


## 首版启动与命令回归

首次运行使用[一条启动命令](first-run.md)，口令与语音流程见 [006](specs/006-first-version.md)。浏览器 RTA 通过显式 `-voice-config` 开启，默认关闭；私有配置及固定/受限数字节点模式以 [server README](../server/README.md) 为准。长期 Key 不进入浏览器。

板端新增主机回归（本地已存在的 cJSON 可用 `--cjson-dir` 显式指定）：

```sh
python3 firmware/esp32-s31-rti/tests/run_group_commands.py
python3 firmware/esp32-s31-rti/tests/run_group_command_ui.py
python3 firmware/esp32-s31-rti/tests/run_group_command_flow.py
python3 firmware/esp32-s31-rti/tests/run_legacy_join.py
python3 firmware/esp32-s31-rti/tests/run_rta_command.py --sanitize
python3 firmware/esp32-s31-rti/tests/run_rta_stop.py --sanitize
python3 firmware/esp32-s31-rti/tests/run_rta_create_cleanup.py --sanitize
python3 firmware/esp32-s31-rti/tests/run_command_bootstrap.py --sanitize
python3 firmware/esp32-s31-rti/tests/run_wifi_recovery.py
python3 -m unittest discover -s tools -p 'test_dev.py'
python3 firmware/esp32-s31-rti/tests/run_group_keyboard_layout.py
```

键盘布局回归另外链接真实 LVGL，不需要 SDL。它覆盖 800×480 边界和底行触点，并要求旧锚点定位变体失败；可用 `--lvgl-dir` 指定依赖、`--build-dir` 复用私有 CMake 缓存。首次编译需要系统 C/C++ 编译器和 CMake。

这些测试执行实际生产解析、意图和 worker 函数；SDK/HTTP/NVS 边界为 doubles，不能代替真实语音或物理触控。RTA stop 的 2xx 仅表示受理：命令路径还查询同一 session 的 `ended / failed / expired` 终态，失败则保持停麦并重试。界面 worker 发布只渲染，不反向回调 PTT，避免打断正在建立的连接代次。

## 变更与文档约定

- 产品体验在 `product.md`，组件边界在 `architecture.md`；接口写入 `specs/` 对应切片，不复制多套字段。
- `acceptance.md` 保存当前结论、测试命令、提交/PR 证据。编译、控制状态、实音频分别验收。
- 每个可独立评审的变更保留分支/PR；不覆盖参考工程或改写 `v0` 来源清单。
- `work/` 放运行数据与临时文件；凭据不写源码，不提交构建产物。
- 继续轻量规格驱动。现阶段无需引入 spec-kit；后续若引入，用独立 PR 映射已有规格，保留单一权威入口。

板端构建和安全升级入口见[固件 README](../firmware/esp32-s31-rti/README.md)。构建成功、镜像校验、板端控制、实际音频传输和人工听测分别验收，以 `acceptance.md` 的实际记录为准。
