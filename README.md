# picpak-tesserae-client

Battery-powered **ESP32-C3** firmware that turns the **PicPak** 4.2" e-paper photo frame into an
embedded client for the [Tesserae](https://github.com/dmellok/tesserae) server. On each wake it
connects to WiFi, pulls the current dashboard frame over REST, paints the panel, reports a heartbeat
(battery, RSSI), and deep-sleeps.

Modelled on Tesserae's battery-native reference client
[tesserae-device-photopainter-7.3-bin](https://github.com/dmellok/tesserae-device-photopainter-7.3-bin),
but retargeted to the PicPak's hardware: a smaller 4-colour panel, an ESP32-C3 (RISC-V) instead of an
S3, no PMIC (battery is read straight off an ADC), and a 2-bits-per-pixel frame format.

> **Status:** experimental, working end-to-end over REST on real hardware. `FW_VERSION 0.2.0-dev`.
> See [`CHANGELOG.md`](CHANGELOG.md) for release notes.

## Hardware

| Component | PhotoPainter reference (7.3") | **This firmware (PicPak 4.2")** |
| --- | --- | --- |
| SoC / module | ESP32-S3 | **ESP32-C3** (RISC-V), 16 MB usable flash |
| Panel | 7.3" 800×480, 6-colour Spectra E6 | **4.2" 400×300, 4-colour BWRY** (Black/White/Yellow/Red), UC81xx-class |
| Frame size | 192 000 B (4 bpp) | **30 000 B (2 bpp)** |
| Panel power | AXP2101 PMIC | direct (no PMIC) |
| Battery sense | AXP2101 fuel gauge (I²C) | **ADC1 ch2 (GPIO2)** + ×1.45 divider, curve-fit calibrated |
| User button | BOOT hold + RESET double-tap | **single button (GPIO2, shared with the battery ADC)** |
| Transport | MQTT + REST | **REST** (MQTT wired but deferred) |

**Pin map** (`firmware/main/board.h`): EPD `SCLK 6 · MOSI 3 · MISO 4 · CS 9 · DC 8 · RST 10 · BUSY 20`
(SPI @ 1 MHz); button `GPIO2` (active-low, shared with the battery ADC). The board also carries an
LSM6 IMU (`CS 7 · INT 5`), unused by this firmware. Cell: single-cell Li-Po, 3.7 V nominal, ~500 mAh.

## Transport modes

The firmware speaks REST to Tesserae. The captive portal shows a REST/MQTT radio, but **MQTT is not
implemented yet** (the fields persist to NVS and nothing reads them — it's labelled "coming soon" and
defaults to REST).

| Mode | Status |
| --- | --- |
| `1` REST (default) | **working** — device polls the Tesserae REST API each wake; no broker needed |
| `0` MQTT | deferred — portal toggle present but inert until a `mqtt_handler` lands |

## Frame format

Raw, headerless, exactly **30 000 bytes** (`400 × 300 ÷ 4`), **2 bits per pixel**, 4 pixels per byte,
**MSB-first** (leftmost pixel in bits 7:6). Palette indices: `0`=Black, `1`=White, `2`=Yellow, `3`=Red.
Rows are packed **bottom-to-top** (the panel scans that way; the renderer flips vertically before
packing — otherwise the image paints upside-down).

The heartbeat reports `kind: "picpak_client"` and `panel_w: 400, panel_h: 300`. The server needs the
matching renderer + device from [`server-plugin/`](server-plugin/) (see [Server plugin](#server-plugin)).

## Heartbeat schema

Posted at the end of every wake to `/api/v1/device/<id>/status`:

```json
{
  "battery_mv": 4164,
  "battery_pct": 96,
  "rssi": -63,
  "ip": "10.0.20.40",
  "fw_version": "0.2.0-dev",
  "kind": "picpak_client",
  "panel_w": 400,
  "panel_h": 300,
  "sleep_interval_s": 900,
  "next_sleep_s": 900,
  "wake_reason": "timer"
}
```

`battery_pct` uses a piecewise-linear Li-Po discharge curve (`firmware/main/battpct.h`); `battery_mv`
comes from ADC1 ch2 (20-sample median + `adc_cali` curve-fitting, ×1.45 divider), measured once early
each wake before WiFi/EPD load the rail.

## REST contract

Every wake hits `/api/v1/device/<id>/...` over plain HTTP:

| Method + path | Purpose |
| --- | --- |
| `POST /device/discover` | Unauthenticated. Posts identity (`device_id`, `kind`, `panel_w/h`, `fw_version`, `mac`). `device_id` is the portal's custom **Device id**, or `picpak-<mac3>` when left blank. `registered:false` → deep-sleep + retry next wake; `registered:true` → returns `device_token`. Default first-boot path. |
| `POST /device/register` | Opt-in: same body + `X-Pairing-Code` header (from the portal's optional Pairing-code field). Returns `device_token` (server auto-claims — no admin click). `403` clears the code and backs off; the code is single-use and burned on success. |
| `GET /device/<id>/frame` | `Authorization: Bearer <device_token>` + optional `If-None-Match`. `200` → `{url, format, panel_w, panel_h}` + `ETag` (fetch `url`, paint); `304` → skip; `204` → not rendered yet. |
| `POST /device/<id>/status` | `Bearer` auth. Body = the heartbeat JSON. Response `{config, next_poll_s, server_time}` — firmware applies `config.sleep_interval_s` to NVS and uses `next_poll_s` for this cycle's deep sleep. |

The `device_token` is persisted to NVS on first pairing; later wakes go straight to the frame GET. A
`401` on any request wipes the token and forces re-pairing.

## Build & flash

> **BACK UP THE STOCK FIRMWARE BEFORE FLASHING.**
> 
Requires **ESP-IDF v5.4.x**.

```sh
cd firmware
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodemxxxx flash monitor
```

> **BACK UP THE STOCK FIRMWARE BEFORE FLASHING.** 

`secrets.h` is optional: `firmware/main/defaults.h` includes it via `__has_include`,
so the firmware **builds and boots without it** (empty WiFi/server defaults → it comes up in the
captive portal). Copy `firmware/main/secrets.example.h` → `secrets.h` only if you want to bake in dev
credentials.

## Provisioning

WiFi + server come from an on-device **SoftAP captive portal** (`firmware/main/provisioning.c`) — no
recompiling. Two triggers:

- **No usable creds at boot** (empty NVS + empty `secrets.h`) → portal auto-starts.
- **Hold the button ~20 s at wake** → portal (deliberate re-provision). A quick tap just wakes.

The AP is **`Tesserae-Setup`** (password `tesserae`, IP `192.168.4.1`); a DNS-hijack pops the captive
sheet on your phone. Fill in:

- **WiFi** network + password.
- **Server URL** — the Tesserae server, either its LAN IP or `<host>.local:8765` (the C3 resolves
  `.local` via mDNS). Use the LAN IP so it keeps working if your internet drops.
- **Device id** *(optional)* — a custom name (`picpak-1`, validated `^[a-z][a-z0-9_-]{1,31}$`); blank
  auto-derives `picpak-<mac>`. This is the id the device claims and shows in Tesserae.
- **Pairing code** *(optional)* — a 6-digit code from Tesserae's **Pair new device** to self-claim
  without an admin click; blank uses the discovery flow (admin clicks **Register**).

Save, and it reboots into the normal cycle. Credentials precedence is `NVS → secrets.h → empty`.
On the LAN the frame advertises its DHCP hostname as **`tesserae-picpak`** (not the default `espressif`).

## Splash screens

Three panel screens (baked 2 bpp blobs, generated by `tools/gen_splash.py`, embedded via CMake):

- **Setup** — logo + `Tesserae-Setup` / `tesserae` — shown when the portal comes up so you can join.
- **Paired** — "Connected — waiting for first frame" — once after you submit, until the first photo.
- **Battery low** — "please charge" — shown by the low-battery gate.

## Power notes

- **Brownout mitigation.** PicPak's power delivery is marginal — at default TX power the WiFi current
  spike browns the board out on radio-on. Fixed with a low-power profile: cap PHY TX power to 10 dBm
  (`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10` — caps the PHY-cal spike), CPU @ 80 MHz, trimmed WiFi buffers,
  AMPDU off, plus a brownout-aware defer (on a power-fault reset, deep-sleep to let the cell recover).
- **Low-battery gate** (`firmware/main/lowbatt*`). Below ~10 % (3300 mV) the frame paints the "charge
  me" screen and enters a 15-min low-power poll instead of the normal cycle, resuming only when the
  voltage **rises** (charging inferred — the C3 has no VBUS sense) or crosses ~30 %. State lives in
  RTC-RAM; a button-wake always resumes. The decision is a pure, host-tested FSM.
- **Battery reading** is taken once early each wake (pre-load) and cached, so the reported value is
  accurate and there's a single ADC read per cycle.

## Project layout

```
picpak-tesserae-client/
├── firmware/
│   ├── CMakeLists.txt · partitions.csv · sdkconfig.defaults
│   └── main/
│       ├── main.c              # wake loop: boot → gesture/provision → low-batt gate → wifi → fetch → paint → heartbeat → sleep
│       ├── board.h · defaults.h # pin map + compile-time tunables (secrets.h optional override)
│       ├── epd_driver.{c,h}     # 400×300 BWRY UC81xx panel driver + init sequence
│       ├── fb2bpp.{c,h}         # 2 bpp framebuffer packer (+ host test)
│       ├── power.{c,h}          # battery ADC, deep sleep, boot-button gesture
│       ├── battpct.h            # pure mV→% Li-Po curve (host-tested)
│       ├── lowbatt*.{c,h}       # low-battery gate (pure FSM + RTC glue)
│       ├── config_store.{c,h}   # NVS config (creds, token, etag, sleep) with secrets.h fallback
│       ├── wifi_manager.{c,h}   # STA connect + NTP
│       ├── provisioning*.{c,h}  # SoftAP captive portal + pure form parser
│       ├── splash.{c,h}         # embedded setup / paired / low-batt screens
│       ├── image_fetcher.{c,h}  # HTTP frame download
│       ├── rest_handler.{c,h}   # discover/register + frame GET + status POST
│       └── heartbeat.{c,h}      # battery / RSSI / IP / panel JSON
└── tools/
    ├── gen_splash.py           # generate the 2 bpp splash blobs
```
## Credits

The wake state machine, REST/heartbeat contract, captive-portal provisioning, and NVS schema follow
Tesserae's reference client
[tesserae-device-photopainter-7.3-bin](https://github.com/dmellok/tesserae-device-photopainter-7.3-bin).
The panel init sequence and 4-colour packing were reverse-engineered for the PicPak's specific 400×300
BWRY hardware.

Not affiliated with the PicPak manufacturer.

## License

AGPL-3.0-or-later. See [LICENSE](LICENSE).
