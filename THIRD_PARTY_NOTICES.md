# 第三方许可说明

本项目源码的默认许可证为 [Apache License 2.0](LICENSE)。第三方材料及依赖继续适用各自的许可证和版权声明；根目录的许可证不替代这些条款。

## Source Han Sans SC 字体

板端使用 Adobe Source Han Sans SC 派生字体，适用 **SIL Open Font License 1.1（OFL-1.1）**。完整许可证、Adobe 版权声明与保留字体名见 [SourceHanSansSC-OFL.txt](firmware/esp32-s31-rti/licenses/SourceHanSansSC-OFL.txt)。

该许可覆盖 `firmware/esp32-s31-rti/main/` 中的以下字体数据文件：

- `rti_font_source_han_sans_sc_16_gb2312.c`
- `shenglu_brand_24.c`
- `shenglu_brand_48.c`
- `shenglu_ui_24.c`
- `shenglu_group_20_gb2312.c`

字体数据不改用 Apache-2.0；复制或分发时需保留适用的 OFL 文本和版权声明。

## ESP32 固件依赖

固件依赖由 [idf_component.yml](firmware/esp32-s31-rti/main/idf_component.yml) 与 [dependencies.lock](firmware/esp32-s31-rti/dependencies.lock) 声明和锁定。构建时下载的 ESP-IDF、BSP、LVGL、Espressif 音频及 WebRTC 组件继续适用各自随附的 `LICENSE`、`LICENCE` 或 `NOTICE`。

其中，`esp_peer`、`esp_audio_codec` 和 `esp_capture` 使用 **Espressif Modified MIT License**，包含仅限配合 Espressif 产品使用的条件，以及不得为非 Espressif 产品用途再分发的限制；`esp-sr` 也有适用于 Espressif 产品的专门许可条款。它们不能统一视为无硬件限制的通用 MIT 或 Apache-2.0 代码。具体版本和条款以下载组件随附的许可证为准。

## Go 服务依赖

Go 服务使用 Pion 及其他模块，版本见 [go.mod](server/go.mod) 与 [go.sum](server/go.sum)。这些模块保留其上游许可证和版权声明；分发包含这些依赖的源码或二进制时，应一并遵守适用的许可和通知要求。

## 复用来源与服务边界

复用基线及逐文件摘要保存在 [v0 清单](docs/v0-manifest.json)；该清单用于记录来源，不代替许可证。已有的单独版权或许可声明继续保留。

本仓库的源码许可不授予外部 RTA 服务的访问权限、账号或凭据，也不授予七牛、MIKU、RTA、Espressif、Adobe 等名称或标识的商标使用权。外部服务与未包含在本仓库中的上游实现不在本仓库源码许可的授权范围内。
