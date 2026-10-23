# 声路同行：本机浏览器群对讲

提供身份、建群/口令入群、当前群切换、PTT 麦权与 WebRTC Opus 群音频。用户显式启用音频后才申请系统麦克风；普通群语音由本机服务转发，不调用 RTA、不连接板子。

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

接口与后续顺序见仓库 `docs/specs/002-control-api.md`、`003-browser-media.md`、`docs/architecture.md` 和 `docs/development.md`。控制状态、数字音频诊断、真实听测、板端与语音切群分别验收，结果只记入 `docs/acceptance.md`。
