# 声路同行 · ESP32-S31-Korvo-1

ESP-IDF firmware for 声路同行 on ESP32-S31-Korvo-1. The default build now
selects the S3 group-intercom client; the original Miku RTA application remains
an explicit Kconfig fallback. The final S3 first-slice image has passed
bidirectional board↔browser numerical transport, PTT cancellation and short
fault-recovery checks. This is not long-term stability or human-listening
acceptance; listening, board multi-group switching and voice commands remain
pending. See
[the S3 contract](../../docs/specs/004-device-lan.md) and
[the evidence log](../../docs/acceptance.md) for scope and actual results.

The boot screen and main interface display the application name **声路同行**.
The boot title uses a 48 px Chinese font, and the main header uses a 24 px font;
both subsets contain only the four application-name characters. The rest of the
Chinese interface continues to use the existing 16 px GB2312 font.

The checked-in `shenglu_brand_24.c` and `shenglu_brand_48.c` were generated with
`lv_font_conv@1.5.3`, using the SourceHanSansSC-Normal font bundled with LVGL.
Their generation commands are recorded in each file header. Font compilation
does not require Node.js. These derived fonts retain the SIL Open Font License
1.1 and Adobe copyright notice; see [the full font license](licenses/SourceHanSansSC-OFL.txt).

## Shared hardware and retained RTA foundation

The following is the existing hardware/RTA foundation. RTA Session creation,
transcripts, model responses and its earlier validation do not establish that
the new group-media client works. The two modes use a single Peer pipeline and
do not run concurrently.

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

## Boot mode

`Miku RTI device` → `Boot into Shenglu group intercom mode`
(`CONFIG_MIKU_INTERCOM_DEFAULT_MODE`) defaults to `y`. It waits for private USB
group configuration and an explicit start; missing or invalid group configuration
does not start an RTA Session. Group mode reuses saved Wi-Fi or the USB Wi-Fi
tool, uses the group `/api/v1` control/media API, and does not create
`oai-events`, welcome speech or model requests. PTT is disabled until the group
Peer is ready; releasing/cancelling first closes the local send gate, then
releases the lease. Recovery returns to unpressed.

To run the original one-to-one RTA application, disable that Kconfig option,
rebuild and update only the application region. The original `rti_auth`
credentials and touch Wi-Fi setup belong to this fallback. This is a deliberate
build choice, not an automatic fallback from a group connection failure.

Group mode starts speaker output at zero without persisting that change to the
saved volume. Numerical transport acceptance and later human listening are
separate steps.

## Group session resources and current evidence

The lifecycle worker reuses one HTTPS client handle within a configured group
session/identity. Each request sets its GET/POST/DELETE method, URL, headers and
body; it detaches request-body and response pointers before freeing temporary
storage. Only complete, successful, persistent responses retain the handle.
Errors, server-close responses, Stop or configuration replacement discard it;
uncertain mutations are not automatically replayed.

Approximately 24.2 KB of the group mode's non-DMA audio workspaces (capture,
mono/accumulator buffers and playback PCM/reference/packet buffers) explicitly
use PSRAM, with no fallback to internal heap. Allocation failure takes the cleanup
path. I2S driver buffers still use DMA; AEC DMA_ATTR arrays remain internal.
This does not move every audio allocation into PSRAM, and the RTA mode's original
`malloc` path for these workspaces is unchanged.

The accepted first-slice application is **2,606,960 B**, SHA256:

```text
a9cd7583a2621dfbab7c894fbd21d7b2b9338dda138bc69994511e307590612b
```

It was written with app-only flashing and independently verified. The final-image
results below replace earlier-image bidirectional numbers; do not combine them
with earlier human-voice evidence.

| Final-image check | Observed result |
| --- | --- |
| Browser→board | 3010 ms window; 30 enabled source observations; source RMS 0.40566; board remote cumulative peak 0.519934 |
| Board→browser | PULSE6 sent 299 packets; browser remote cumulative peak 0.0075; sent count remained 299 after release |
| Normal short watch | 120.26 s; HTTP count 7→124; 1 TLS connection; 0 failures |
| Planned service interruption | 7 connection errors from fault injection; recovery used a new epoch/Peer, same group/version, unpressed and TX=0 |
| Peer lifecycle/cleanup | 3 board Peer lifecycles without panic; browser created 2 Peers, final open=0 and capture=none |
| Available heap while audio active | internal 52,895 B; DMA 20,887 B, compared with 28,623 B / 4,299 B before the workspace allocation change |
| Final stop | internal available heap 154,467 B; existing Wi-Fi and RTA credentials retained |

The normal-watch zero-failure result does not mean the entire run had no errors:
the planned outage deliberately produced the seven errors above. These results
accept the S3 first slice's numerical transport and bounded recovery checks,
not long-term stability or listening quality. Human listening has not been
performed; board multi-group switching and voice commands also remain pending.
See the [sanitized final evidence](../../docs/evidence/2026-10-24-device-group.json)
and [acceptance log](../../docs/acceptance.md) for the authoritative test scope.

## Group mode USB configuration

First provision an independent board identity through the service's localhost
API and obtain the target group's join code. The LAN listener deliberately rejects anonymous
`POST /api/v1/devices` and serves neither web pages nor diagnostics. Keep the
browser on localhost. Enable the extra HTTPS listener only with all five
`-device-*` flags described in [the development guide](../../docs/development.md#s3-板端-lan-开发入口).

Use a private JSON file owned by the current user, mode 0600, with exactly these
five nonempty ASCII string fields; extra/duplicate fields and NUL are rejected:

| Field | Meaning and maximum encoded field length |
| --- | --- |
| `server_url` | 191 B; canonical `https://<RFC1918-IPv4>:<port>`, no path, trailing slash, hostname or IPv6 |
| `ca_pem` | 2047 B; PEM CA certificate(s) only, never a private key |
| `device_id` | 80 B; independent preprovisioned board identity |
| `token` | 128 B; that identity's Bearer Token |
| `group_code` | 16 B; initial group join code |

The three identity/token/code fields accept only letters, digits, `_` and `-`.
The input file is bounded to 16 KiB; after compact ASCII JSON encoding the
**whole payload must fit 2000 B**, including field names, quotes and escaped PEM
newlines. A field's individual limit does not override the total limit. The tool
validates the file before sending it over USB and prints only fixed status
messages. Do not pass values in shell arguments/environment variables or paste
private JSON into a serial console. Store the file under the ignored
`work/device-lan/` directory with a 0700 parent, outside tracked firmware files.

Development TLS uses a local short-lived CA and a server certificate whose IP
SAN matches the selected LAN IPv4. Keep the private key on the computer; provision
only CA certificates to the board. The board verifies the CA chain, IP/hostname
and validity period; do not disable verification or install the CA into the
computer's system trust store. Device requests use the exact IP:port Host and
omit Origin. The listener rejects cross-site/ambiguous Sec-Fetch-Site, ignores
forwarded headers and provides no CORS.

The service publishes only the selected LAN IPv4 plus loopback ICE candidates,
with an explicit UDP port interval. Candidate filtering is not a guarantee that
every underlying socket binds only that IP. Confirm those ports are reachable
on the test network; this slice adds no public candidates or STUN/TURN.

After activating the ESP-IDF Python environment and stopping all other serial
readers, run from this firmware directory:

```bash
python tools/intercom_config.py --port /dev/cu.usbserial-XXXX \
  --config-file ../../work/device-lan/board.json --restart --sync-time
python tools/intercom_config.py --port /dev/cu.usbserial-XXXX --status
```

`--restart` requires a configuration file. `--sync-time` sends the computer's
current UTC after any requested restart. **There is no automatic SNTP in this
slice: after every reboot, reset or application flash, synchronize time again**
before explicitly starting the group client:

```bash
python tools/intercom_config.py --port /dev/cu.usbserial-XXXX --sync-time
```

Time is not persisted across reboot. Without usable current time the client
waits; a configuration-ready status alone does not establish valid TLS, a
connected Peer or usable audio. Keep the computer clock correct and update
expired/rotated certificates through explicit provisioning.

The configuration is stored in the dedicated NVS `intercom` namespace and
normally preserves Wi-Fi, `rti_auth` and saved volume. Group-mode startup stops
on NVS no-free-pages/version errors and requires explicit repair instead of
automatically erasing NVS. The RTA fallback retains its older error recovery,
including automatic NVS erase for those errors; do not assume the same guarantee
when selecting that mode.

No long-lived RTIC/API POC token is compiled into firmware or committed to the
repository. The separate legacy USB credential tool stores development RTA
credentials in `rti_auth`. Production device authentication/provisioning remains
a separate task; the group-intercom Token is not an RTIC POC token.

## Toolchain

ESP32-S31 requires an ESP-IDF version supporting `esp32s31` (6.1 or newer;
the retained hardware baseline uses 6.2). The Korvo-1 BSP is pinned to `1.0.0~1`.
From the repository root:

```bash
source ~/esp/esp-idf-s31/export.sh
cd firmware/esp32-s31-rti

idf.py --preview menuconfig
idf.py --preview build
idf.py --preview -p /dev/cu.usbserial-XXXX app-flash
```

For a fresh build configuration only, select `esp32s31` with
`idf.py --preview set-target esp32s31` before configuring/building. Do not reset
an existing configuration as a routine upgrade step. Check that
`CONFIG_MIKU_INTERCOM_DEFAULT_MODE` matches the intended mode.

Preserve the current partition layout and use **`app-flash` only** for this
slice. It does not write the bootloader, partition table or NVS. Do not run
`erase-flash` or substitute a full `flash`. Before writing, retain the known-good
application and confirm the generated `build/flasher_args.json` identifies the
same application region. Independently verify the written image with esptool;
for the current factory layout the app offset is `0x10000` and its partition is
4 MiB, but confirm these against this build before using the command:

```bash
python -m esptool --chip esp32s31 --port /dev/cu.usbserial-XXXX \
  verify-flash 0x10000 build/miku_rti_esp32s31.bin
```

A successful write/digest comparison is not a control, transport or listening
acceptance result. After the resulting reboot, send USB `--sync-time` again.
If download mode is needed, hold `BOOT`, tap `RESET`, then release `BOOT`.

Group mode reuses saved 2.4 GHz Wi-Fi and supports the USB configuration tool;
it does not enter the original touch Wi-Fi setup flow. In the RTA fallback,
the touch screen can scan/select an SSID or use `ENTER NETWORK MANUALLY`, then
persist the station configuration in NVS. Pure 5 GHz and 802.1X need different
hardware or an additional authentication flow.

For USB Wi-Fi setup, stop `idf.py monitor` and any other serial reader first,
then run from this directory in the activated ESP-IDF environment:

```bash
python tools/wifi_config.py --port /dev/cu.usbserial-XXXX
python tools/wifi_config.py --port /dev/cu.usbserial-XXXX --scan-only
```

The tool scans S31-visible networks, prompts for a selection, and uses a hidden
password prompt. Credentials travel only over the local USB link, are not printed
by the firmware, and are persisted in NVS after successful connection.

## Board-free HTTP reuse regression

From the repository root, with Python 3 and a system C compiler (`cc`):

```bash
python3 firmware/esp32-s31-rti/tests/run_http_client_reuse.py
```

The runner extracts the current production request/body-detach/client-drop
functions and compiles them against an SDK double. It covers GET→POST acquire→
POST renew→GET→DELETE reuse, Content-Type/bodyless-request semantics, pointer
detachment, failure/incomplete/oversized/invalid-JSON cleanup and a server-close
response. It also runs the `GROUP_CONTROLLER_HOST_TEST` state regression. It
needs no ESP-IDF, opens no network or serial connection, and reads no credentials.
This host regression does not establish real TLS, ESP allocation behavior,
audio transport or long-running stability.

## RTI development credentials (RTA fallback only)

This section applies only after disabling `CONFIG_MIKU_INTERCOM_DEFAULT_MODE`
and rebuilding. Group mode uses `tools/intercom_config.py` above, not this RTIC
credential path.

The API control token is never compiled into the firmware or passed as a shell
argument. Provision the development app ID and token over the local USB link:

```bash
export RTI_API_POC_APP_ID='<app-id>'
export RTI_API_POC_TOKEN='<token>'

python tools/rti_config.py --port /dev/cu.usbserial-XXXX
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
python tools/rti_config.py --port /dev/cu.usbserial-XXXX --status
python tools/rti_config.py --port /dev/cu.usbserial-XXXX --clear
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
features required by `esp_peer`. Current startup pre-generates the DTLS
certificate for reuse and optionally runs the configured AEC self-test. These
steps do not negotiate group ICE or validate board↔browser media. The retained
RTA hardware baseline uses ESP-IDF 6.2; S3 connection, transport and cleanup
results are recorded separately above.

In the RTA fallback, creating the `oai-events` DataChannel happens only after
the RTI SDP/ICE exchange starts; attempting to create it before a new
PeerConnection is negotiated is not a valid `esp_peer` state transition.

## Retained RTA delivery history

The done markers below describe the earlier RTA path only. S3 group TLS/control,
board↔browser Opus/PCM and PTT/recovery have separate acceptance records. The
final-image results above accept only the S3 first numerical slice. Long-term
stability, human listening, board multi-group switching and voice commands
remain pending.

1. Build and boot on ESP32-S31-Korvo-1.
2. Validate the RGB LED, buttons, ES8389 playback, and dual-microphone capture.
3. Validate Wi-Fi and HTTPS trust configuration. (done)
4. Implement development authentication and RTI session creation. (done)
5. Replace development credentials with the production device-authentication API.
6. Connect `esp_peer` to Opus audio and the realtime event DataChannel. (done)
7. Exchange SDP through `/v1/realtime/sessions/{session_id}/rtc`. (done)
8. Add full-duplex ESP-SR AEC and validate repeated Start/Stop cleanup. (done)
9. Add production device authentication, OTA, and production provisioning.
