# 声路同行：本机浏览器群对讲

提供身份、建群/口令入群、当前群切换、PTT 麦权与 WebRTC Opus 群音频。用户显式启用音频后才申请系统麦克风；普通群语音由本机服务转发，不调用 RTA；默认入口仅供本机浏览器，板端入口需显式配置。

需要 Go 1.24.0+；浏览器状态测试使用 Node.js 22+。WebRTC 使用固定 Pion v4.2.23，依赖由 go.mod/go.sum 锁定。

```sh
cd server
go run ./cmd/intercom
```

打开[本机页面](http://127.0.0.1:8080)。默认快照为仓库 `work/intercom/state.json`，数据不进 Git。可通过 `-addr`、`-data` 显式设置；`-data` 的现有父目录必须是私有目录（0700），程序不会修改上级目录权限。默认只在本机运行，公开部署前另补 HTTPS、访问控制与配额。

使用两个独立浏览器配置或普通/隐私窗口创建不同身份。A 建立全员群，B 使用口令加入；再建立后勤组，第二次加入不改变当前群。两端点击启用音频并允许麦克风，再按住说话；检查双向声音、唯一授权、松手释放与手动切群，建议使用耳机，同机验证时将两个窗口并排保持可见。显示的是已加入成员数，不是在线人数。Token 只保存在浏览器本机存储，服务端保存摘要；群口令仅在创建后显示一次。

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
