# 声路同行 · ESP32-S31-Korvo-1

ESP-IDF firmware for 声路同行, using ESP32-S31-Korvo-1 as a Miku RTA realtime voice endpoint.

The boot screen and main interface display the application name **声路同行**.
The boot title uses a 48 px Chinese font, and the main header uses a 24 px font;
both subsets contain only the four application-name characters. The rest of the
Chinese interface continues to use the existing 16 px GB2312 font.

The checked-in `shenglu_brand_24.c` and `shenglu_brand_48.c` were generated with
`lv_font_conv@1.5.3`, using the SourceHanSansSC-Normal font bundled with LVGL.
Their generation commands are recorded in each file header. Font compilation
does not require Node.js. These derived fonts retain the SIL Open Font License
1.1 and Adobe copyright notice; see [the full font license](licenses/SourceHanSansSC-OFL.txt).

## Current scope

- ESP32-S31 target and Korvo-1 BSP dependency;
- application state model;
- ES8389 speaker and dual-microphone codec startup on a 16 kHz I2S bus;
- 800x480 LVGL status screen and GT1151 touch input;
- touch-based Wi-Fi scan, password entry, NVS persistence, and automatic reconnect;
- HTTPS RTI Session creation with USB-provisioned development credentials;
- `esp_peer` 1.5.1 WebRTC PeerConnection, DTLS-SRTP, and DataChannel support;
- bidirectional Opus audio, realtime transcripts, multi-turn conversation UI,
  voice selection, and optional model web search;
- ESP-SR full-duplex AEC with speaker-reference buffering and barge-in support;
- repeatable Start/Stop lifecycle with heap telemetry and resource cleanup.

See [docs/memory-architecture.md](docs/memory-architecture.md) for the current
runtime architecture, explicit buffer sizes, measured Session memory profile,
and the constraints that shaped the AEC/WebRTC implementation.

中文开发、烧录、日志定位和典型问题复盘见
[docs/development-debugging-guide.zh-CN.md](docs/development-debugging-guide.zh-CN.md)。

No long-lived RTIC/API POC token is compiled into firmware or committed to the
repository. For development only, the USB tool stores it in a dedicated NVS
namespace. The production device will authenticate to a device-facing API and
receive a short-lived, session-scoped RTI access token; production hardware
must not use the POC-token provisioning path.

## Toolchain

ESP32-S31 currently requires an ESP-IDF version that supports the `esp32s31`
target. This project requires ESP-IDF 6.1 or newer and pins the Korvo-1 BSP to
`1.0.0~1`.

```bash
source ~/esp/esp-idf-s31/export.sh
cd devices/esp32-s31-rti

idf.py --preview set-target esp32s31
idf.py --preview menuconfig
idf.py --preview build
idf.py --preview -p /dev/cu.usbserial-XXXX flash monitor
```

On the first flash, if the serial port does not appear, hold `BOOT`, tap
`RESET`, and then release `BOOT` to enter download mode.

Wi-Fi is configured directly on the touch screen. Select an SSID, or choose
`ENTER NETWORK MANUALLY` for a hidden/unlisted 2.4 GHz network, enter its
password, and press the keyboard confirmation key. ESP-IDF stores the station
configuration in the board's NVS partition and reconnects on the next boot.
Pure 5 GHz and 802.1X enterprise networks need different hardware or an
additional enterprise-authentication flow.

For development, Wi-Fi can also be configured over the board's USB serial
connection. Stop `idf.py monitor` first, then run:

```bash
~/.espressif/python_env/idf6.2_py3.14_env/bin/python \
  tools/wifi_config.py --port /dev/cu.usbserial-10
```

只查看开发板能发现的 2.4 GHz 网络：

```bash
~/.espressif/python_env/idf6.2_py3.14_env/bin/python \
  tools/wifi_config.py --port /dev/cu.usbserial-10 --scan-only
```

The tool scans the S31-visible 2.4 GHz networks, prompts for a selection, and
uses a hidden password prompt. Credentials are sent only over the local USB
serial link, are not printed by the firmware, and are persisted in NVS after a
successful connection.

## RTI development credentials

The API control token is never compiled into the firmware or passed as a shell
argument. Provision the development app ID and token over the local USB link:

```bash
export RTI_API_POC_APP_ID='<app-id>'
export RTI_API_POC_TOKEN='<token>'

~/.espressif/python_env/idf6.2_py3.14_env/bin/python \
  tools/rti_config.py --port /dev/cu.usbserial-10
```

If the environment variables are omitted, the tool prompts for the app ID and
uses a hidden prompt for the token. The values are stored in the `rti_auth` NVS
namespace. The tool restarts the board after provisioning; once Wi-Fi reconnects,
the firmware calls `POST /rtic/v1/realtime/sessions`, validates the HTTPS server
with the ESP certificate bundle, and retains only the returned session-scoped
RTC token in RAM.

The current RTIC API gateway is `https://miku-rtic.qiniuapi.com`. API keys are
environment-specific and must be sent to the same gateway that created them.

Check or clear the stored credentials without printing them:

```bash
tools/rti_config.py --port /dev/cu.usbserial-10 --status
tools/rti_config.py --port /dev/cu.usbserial-10 --clear
```

Configure these RTI values under `Miku RTI device` in
`idf.py --preview menuconfig`:

- device API base URL;
- development device ID.

The generated `sdkconfig` is intentionally ignored. Wi-Fi credentials are not
compiled into the firmware and are never written to the serial log. Production
hardware should additionally enable NVS encryption.

The Korvo-1 does not route an audio MCLK signal. The BSP recommends 16 kHz, so
the firmware explicitly initializes I2S at 16 kHz instead of using the BSP's
22.05 kHz default.

The board module contains 16 MB flash and 16 MB PSRAM. Both are enabled in
`sdkconfig.defaults`; PSRAM is explicitly used for the AEC workspace, audio
task stacks, playback queue, and speaker-reference stream. `partitions.csv`
allocates 4 MB to the factory application so LVGL, Wi-Fi, WebRTC, ESP-SR,
Opus, and the Chinese font assets fit safely.

## WebRTC validation

The firmware enables the MbedTLS DTLS, DTLS-SRTP, and X.509 certificate-writing
features required by `esp_peer`. At boot it runs a short self-test in a dedicated
12 KB FreeRTOS task: it opens the ESP32-S31 default PeerConnection, verifies the
DataChannel implementation is present, and closes the peer without signaling or
network traffic. The Korvo-1 has passed this test on hardware with ESP-IDF 6.2.

Creating the `oai-events` DataChannel happens only after the RTI SDP/ICE exchange
starts; attempting to create it before a new PeerConnection is negotiated is not
a valid `esp_peer` state transition.

## Delivery sequence

1. Build and boot on ESP32-S31-Korvo-1.
2. Validate the RGB LED, buttons, ES8389 playback, and dual-microphone capture.
3. Validate Wi-Fi and HTTPS trust configuration. (done)
4. Implement development authentication and RTI session creation. (done)
5. Replace development credentials with the production device-authentication API.
6. Connect `esp_peer` to Opus audio and the realtime event DataChannel. (done)
7. Exchange SDP through `/v1/realtime/sessions/{session_id}/rtc`. (done)
8. Add full-duplex ESP-SR AEC and validate repeated Start/Stop cleanup. (done)
9. Add production device authentication, OTA, and production provisioning.
