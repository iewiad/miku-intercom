# 声路同行 · Miku Intercom

戴伟（MIKU-流媒体）参加 Qiniu Builder Day 的嵌入式语音对讲机项目。

## 当前产品方向

面向小组即时协作的智能语音对讲工具：让分散的小组随时沟通，需要协助时，一句话找到对的组。首版完成快速组队、PTT 和按群名切换；根据需求推荐对应小组作为待验证增强。活动执行、户外结伴、车队出行都可使用，产品不限定某一行业。

当前处于产品规划阶段，尚未新增对讲功能实现。产品流程见 `docs/product.md`，架构边界见 `docs/architecture.md`，真实验收状态见 `docs/acceptance.md`，轻量执行规格见 `docs/specs/001-intercom-mvp.md`。

## v0：复用基线

此初始提交仅导入既有源码与来源记录，尚未开始本项目功能实现。

- `firmware/esp32-s31-rti/`：ESP32-S31-Korvo-1 设备工程，来源旧工程 commit `02b8433407911c7673187665861af5e4bebe7dde` 加 2026-10-23 当时选定的工作树改动。**不是该 commit 的纯净快照**。包括 9 个已修改文件与 1 个原未跟踪的中文调试文档；逐文件哈希和状态见 `docs/v0-manifest.json`。
- `reference/rta-browser/`：既有 RTA RTC/WSS 浏览器示例的五个源码/说明文件，来源 `qbox/miku-rti` commit `f1c3722daacbab03b4d923d7e0ee6562020c893d`。仅作原接口与生命周期参考，非本项目群聊服务。
- 未导入旧工程的生成物、运行日志、sdkconfig、managed_components、地理数据库或凭据。旧工作树未修改。

旧工程说明中的历史通过记录不能作为本项目的硬件验收。设备已接入；复用固件的感叹号键修复已构建、烧录并由用户确认可输入，见 [PR #2](https://github.com/iewiad/miku-intercom/pull/2)，合并待确认。群对讲与 RTA 最终转写仍未通过本项目真实音频测试。项目保持私有；公开前须核验内部源码再分发授权、依赖与字体许可证及凭据。

本项目目标：建群与口令入群、嵌入式 PTT 半双工群对讲、使用现有 RTA 最终用户转写切群、状态与断线恢复。开发截止 2026-10-26 09:00（Asia/Shanghai），以赛事原文核实结果为准。
