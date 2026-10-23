# 本机控制面契约 v1

状态：首个开发切片的实现契约，2026-10-23。对应 [001](001-intercom-mvp.md) 的 U1，以及 U2/U4 的状态部分。此切片不采集麦克风、不传音频、不调用 RTA、不连接板子。页面必须明确显示“控制流程演示，尚无音频”。

## 运行与存储

独立 Go 模块位于 `server/`，使用标准库；`go run ./cmd/intercom` 默认监听 `127.0.0.1:8080`，提供同源浏览器页面和 JSON API。开发者可用 `-addr`、`-data` 显式配置；数据默认写入仓库忽略目录 `work/intercom/state.json`。开发入口不需要任何七牛凭据。

首版为单进程、本机开发服务。群、身份凭据摘要、成员资格、当前群版本和切群操作结果，以 JSON 快照持久化；临时文件写入、同步、原子替换成功后才确认写入，失败不得发布新状态。目录 0700、快照 0600；仅允许一个进程使用同一数据目录。发言权、取消记录及口令失败限流为内存状态，服务重启后所有麦权失效。部署到局域网/公网前需增加 HTTPS、入口访问控制、配额及部署配置；不可直接把本机开发服务当公网服务。

终端首次创建身份，服务端生成随机 ID 和 256 bit 随机 Bearer Token；Token 仅创建时返回，服务端只存 SHA-256 摘要。浏览器为演示存入本机 localStorage；板端后续使用自己的安全存储。昵称只用于显示。除创建身份和静态页面外，API 均从 Bearer Token 确定调用者，不能由请求中的 device_id 冒充身份。两个终端用两个独立浏览器配置/隐私窗口，避免共享同一个 localStorage 身份。

## API

统一前缀 `/api/v1`。JSON 请求；未知字段拒绝，单次请求体上限 8 KiB。错误格式 `{ "error": { "code": "...", "message": "..." } }`，不回显 Token 或口令。所有 API 响应禁止缓存；只接受同源浏览器请求，不开启 CORS。

| 方法 / 路径 | 请求 | 返回与规则 |
| --- | --- | --- |
| POST `/devices` | `{name}` | 201：`{device:{id,name},token}`。昵称 1–32 个 Unicode 字符 |
| GET `/state` | — | 200：当前权威状态，见下文 |
| POST `/groups` | `{name}` | 201：`{group,code,state}`。群名 1–32 字符，服务内唯一；创建者自动加入，首次自动进入 |
| POST `/memberships` | `{code}` | 200：`{state}`。8 位大写随机口令（去掉易混字符），大小写与首尾空白归一；只存摘要；重复加入无副作用。每身份每分钟至多 5 次错误口令，超限 429 |
| PUT `/current-group` | `{operation_id,group_id,expected_version}` | 返回 `{operation,state}`；原子核权、版本比较、释放原麦权、修改当前群。操作结果持久化 |
| GET `/operations/{id}` | — | 返回本身份对应操作的不可变终态；未知 404。不能用未知推断请求永不生效，应重试同一操作 |
| POST `/floor/acquire` | `{group_id,current_version,request_id,server_epoch,valid_until}` | 200：`{floor}`；当前群内一次仅一位获权，忙时 409。每次按下生成新的 request_id，重复申请不能延长租约 |
| POST `/floor/renew` | `{group_id,current_version,request_id,lease_id,server_epoch}` | 200：`{floor}`；仅当前持有人、相同群/版本/申请/租约可续租 |
| POST `/floor/release` | `{group_id,current_version,request_id,server_epoch}` | 200：`{released:true}`；同时取消此申请。未收到 acquire 响应也可先调用，之后迟到申请不得获权 |

`state` 形状：`{device:{id,name},server_epoch,server_time,current_group_id,current_version,groups:[{id,name,member_count,floor}],media_ready:false}`。只列本身份已加入群。`member_count` 是已加入成员数，不是在线人数；首切片没有 presence。`floor` 空闲时为 null，否则 `{device_id,device_name,request_id,lease_id,expires_at}`；时间为服务端 UTC RFC3339。客户端通过轮询刷新状态，不能把轮询结果称为实时媒体连接。

## 切群幂等与版本

`operation` 形状为 `{operation_id,group_id,expected_version,status,error_code,current_group_id,current_version}`；业务终态（含拒绝）统一 HTTP 200，`error_code` 在 applied 时为空。GET 操作返回 `{operation}`。字段验证失败返回 400；认证失败 401；标识复用冲突 409。

`operation_id`、`request_id` 为终端生成的 UUID 或等价随机标识，允许 8–80 位 ASCII 字母、数字、`-`、`_`。同一身份的 operation_id 只能对应同一目标与 expected_version；复用标识但载荷不同返回 409。终态为 applied 或 rejected（带错误码），包含执行完成时的 current_group_id/current_version。重复相同操作返回原终态，响应中的 state 则是最新状态，不能把旧终态快照当成当前状态。每身份最多保存 1024 个切群操作，达到上限拒绝新操作，不悄悄淘汰幂等记录；这是开发服务的容量边界。

`expected_version` 必须匹配执行前的当前群版本，否则记录 rejected/version_conflict。无权限目标记录 rejected/not_member，当前群与麦权不变。进入不同群时版本加一并释放原麦权；进入同群为已应用的无操作，版本不变。首次加入自动进入也加一。服务重启保持版本，防止旧切群请求或旧群麦权申请重新生效。

超时后重试相同 operation_id 和载荷直至取得终态，也可查询该 ID。单次 GET /state 不足以证明排队中的操作不会生效。首切片只支持手动切群；未来语音最终转写也必须使用同一接口和新指令代次。

## 麦权状态机

租约 3 秒；获权且仍按住时每 1 秒续租。申请必须携带当前群与版本，旧群/旧版本请求拒绝。一个 request_id 只用于一次按下，获权后不通过 acquire 续租；被忙拒绝、取消或过期的申请不自动排队或复活。服务端在所有读取和操作前按时钟清理到期租约。

release 记录取消标识，即使 acquire 尚未到达也有效；同身份/群/版本/request_id 的迟到申请被拒绝。取消与终态记录至少保留 60 秒。`state` 返回每次服务启动新生成的 `server_epoch` 和 `server_time`，所有 floor 写操作必须匹配启动代次；acquire 另带固定 `valid_until`（RFC3339），首次按下按服务时间偏移估算，建议给 5 秒窗口，重试不刷新。服务端拒绝过期或超过未来 10 秒的期限，故 60 秒保护期覆盖全部仍可生效的申请；重启前或超期请求不能重新获权。客户端任何迟到成功回调在已松手、页面隐藏、切群或代次变化时均不得进入“持有”，应再次释放。租约 ID 每次新授权随机生成；旧 release 不得清除新 request_id 的麦权，旧 renew 不得延长新租约。内存申请记录每身份限制 256 项，未过保护期的记录不淘汰，达到上限拒绝新申请。

客户端松手/取消/失焦/页面隐藏/断线立即退出持有、停止续租并尽力 release；网络失败也必须回到未按麦。单终端交互串行处理未结束的申请，重连后仅刷新状态，绝不自动恢复按下。客户端串行轮询，切群后丢弃此前发起的轮询，较低 current_version 不得覆盖较高版本；server_epoch 改变立即退出持有并重新核实状态。当前切片的“持有”只表示控制授权，绝不显示“正在讲话/已接通”。媒体切片还须独立校验租约、群和版本，服务端门控后才允许转发。

## 无板验收

使用 Go `testing`、`httptest`、注入时钟和 `go test -race ./...`，覆盖鉴权、错误口令、重复入群、第二群不打断、跨群越权、切群幂等/版本/重启、并发抢麦唯一持有、先释放后到达、旧请求释放、续租与到期、持久化失败不确认。浏览器用两个独立身份检查同一状态和切群反馈；不把状态演示当作 U2 实音频通过。群 WebRTC、媒体门控、语音切群和板端验收在后续切片完成。
