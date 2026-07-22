# picpak-tesserae-client

Battery-powered **ESP32-C3** firmware that turns the **PicPak** 4.2" e-paper photo frame into an
embedded client for the [Tesserae](https://github.com/dmellok/tesserae) server. On each wake it
connects to WiFi, pulls the current dashboard frame over REST or MQTT, paints the panel, reports a
heartbeat (battery, RSSI), and deep-sleeps.

Modelled on Tesserae's battery-native reference client
[tesserae-device-photopainter-7.3-bin](https://github.com/dmellok/tesserae-device-photopainter-7.3-bin),
but retargeted to the PicPak's hardware: a smaller 4-colour panel, an ESP32-C3 (RISC-V) instead of an
S3, no PMIC (battery is read straight off an ADC), and a 2-bits-per-pixel frame format.

> **Status:** working end-to-end over REST and MQTT on real hardware. `FW_VERSION 0.7.0`.
> See [`CHANGELOG.md`](CHANGELOG.md) for release notes.
> Tested on PicPak **hardware revision v0.0.1**, migrating from **official firmware v1.1.11**
> to this firmware and back (stock restore verified).

**Contents:**
[Hardware](#hardware) ·
[Installing the firmware](#installing-the-firmware) (backup → flash → set up) ·
[Going back to stock](#going-back-to-stock) ·
[Building from source](#building-from-source) ·
[How it works — Tesserae integration](#how-it-works--tesserae-integration) ·
[Disclaimer](#disclaimer) ·
[License](#license)

## Hardware

| Component | PhotoPainter reference (7.3") | **This firmware (PicPak 4.2")** |
| --- | --- | --- |
| SoC / module | ESP32-S3 | **ESP32-C3** (RISC-V), 16 MB usable flash |
| Panel | 7.3" 800×480, 6-colour Spectra E6 | **4.2" 400×300, 4-colour BWRY** (Black/White/Yellow/Red), UC81xx-class |
| Frame size | 192 000 B (4 bpp) | **30 000 B (2 bpp)** |
| Panel power | AXP2101 PMIC | direct (no PMIC) |
| Battery sense | AXP2101 fuel gauge (I²C) | **ADC1 ch2 (GPIO2)** + ×1.45 divider, curve-fit calibrated |
| User button | BOOT hold + RESET double-tap | **single button (GPIO2, shared with the battery ADC)** |
| Transport | MQTT + REST | **MQTT + REST** (REST is the recommended default) |

**Pin map** (`firmware/main/board.h`): EPD `SCLK 6 · MOSI 3 · MISO 4 · CS 9 · DC 8 · RST 10 · BUSY 20`
(SPI @ 1 MHz); button `GPIO2` (active-low, shared with the battery ADC). The board also carries an
LSM6 IMU (`CS 7 · INT 5`), unused by this firmware. Cell: single-cell Li-Po, 3.7 V nominal, ~500 mAh.

## Installing the firmware

Three steps: **back up** the stock firmware, **flash** the release build, **set up** via the
on-device portal.

> ⚠️ **Never `erase_flash` this board** — a full chip erase of the 32 MB part fails partway and
> leaves the device half-wiped (recovery: just flash again — small region writes work). No erase
> is needed for any step here; to wipe only the saved config, see
> [Factory reset](#factory-reset-settings-only--for-this-custom-firmware).

**Tested configuration: hardware rev v0.0.1, official firmware v1.1.11.** On this unit the
ESP32-C3's security eFuses are **not burned** — no Secure Boot, no Flash Encryption, USB
download mode unlocked — which is what makes the backup, this firmware, and a later stock
restore possible at all. eFuses are one-time-programmable: if the manufacturer ever ships
units (or an update that burns fuses) with these protections enabled, none of this will
work. If unsure, check first — `python -m espefuse --chip esp32c3 -p <PORT> summary` should
show Secure Boot and Flash Encryption disabled before you proceed.

### Step 1 — Back up the stock firmware (required)

**You cannot re-download the stock firmware — the backup is your only way back.** Read the
**lower 16 MB** of the flash with `--no-stub`:

```sh
# backup — run it TWICE, into two files
python -m esptool --chip esp32c3 -p <PORT> -b 921600 --no-stub read_flash 0x0 0x1000000 stock_backup_1.bin
python -m esptool --chip esp32c3 -p <PORT> -b 921600 --no-stub read_flash 0x0 0x1000000 stock_backup_2.bin

# the two hashes must be identical — only then trust the backup
shasum -a 256 stock_backup_1.bin stock_backup_2.bin
```

**Why 16 MB when the chip says 32?** The flash identifies itself as 32 MB, but full 32 MB
dumps (taken twice on real hardware and compared) show the upper half reads back as a
**byte-perfect mirror of the lower half** — it holds no data of its own — and the entire
stock system (bootloader, partition table, both app slots, photo storage) lives below 16 MB.
Restoring a lower-16 MB backup has been verified to boot stock. Reading 32 MB just captures
the same data twice and doubles the wait. Never *write* anything above the 16 MB boundary —
on this chip such writes can wrap around and corrupt the bootloader.

**Why `--no-stub`?** esptool's default *stub* flasher is unreliable for large transfers on
this flash chip — it can report success while the data is actually corrupt — so the backup
must go through the ROM loader instead. The ROM loader moves data in small acknowledged
chunks, so **expect a long wait — roughly 1.5–2 hours per read**; a higher baud rate won't
help (this board's native USB ignores it). Keep the frame plugged in and prevent your
computer from sleeping.

A raw `read_flash` has no built-in error check, so two independent reads matching is what
proves the backup is good. Keep one file and its checksum somewhere safe. The backup is
**per-device**: the stock settings region at `0x9000` holds factory data unique to your unit
(serial number, radio calibration), and the first flash of this firmware overwrites it —
someone else's backup or a shared stock image cannot fully restore your frame.

### Step 2 — Flash the release build

> **Easiest: flash from the browser — <https://picpaktesserae.pages.dev>** (Chrome or Edge).
> Fresh install or a settings-keeping update, plus a read-only serial monitor — no tools to
> install. The esptool commands below do the same from the command line.

Each [release](../../releases) ships an all-in-one image, its four component files, and
`SHA256SUMS`; for the command-line route only esptool is needed (`pip install esptool`):

| File | Flash offset | |
| --- | --- | --- |
| `picpak-tesserae-firmware.bin` | `0x0` | **all-in-one image** (the four files below merged) — simplest for a first install; always wipes saved settings |
| `bootloader.bin` | `0x0` | |
| `partition-table.bin` | `0x8000` | |
| `nvs_blank.bin` | `0x9000` | blank settings — guarantees the setup portal on first boot; **omit when upgrading** to keep saved WiFi/pairing |
| `picpak-tesserae-client.bin` | `0x10000` | |

**Finding the port:** the C3's native USB-Serial-JTAG shows up as `/dev/cu.usbmodem*` on macOS
(`/dev/ttyACM*` on Linux; on Windows as "USB Serial Device (COMx)" under Device Manager →
Ports (COM & LPT) — use `COM<x>`). List it with `ls /dev/cu.usbmodem*`. The number encodes the
USB port/hub position, so it **changes when you replug into a different port** — re-check it
rather than assuming last time's name.

First install — all-in-one image:

```sh
python -m esptool --chip esp32c3 -p <PORT> -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 picpak-tesserae-firmware.bin
```

Same install from the individual files (use this form **when upgrading**, leaving out
`0x9000 nvs_blank.bin` to keep your saved settings — the all-in-one image always wipes them):

```sh
python -m esptool --chip esp32c3 -p <PORT> -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x9000 nvs_blank.bin 0x10000 picpak-tesserae-client.bin
```

If the connection drops or fails to sync, use a different USB-C **data** cable and a direct
port (no hub) — the baud rate doesn't matter on this board's native USB. *"The port is busy or
doesn't exist"* means something else holds the port — usually an open serial monitor; close it
and re-run.

Note: **the partition layout changes** (stock dual-OTA → our single factory app), so the
vendor's update/OTA tools stop recognizing the device until you restore stock — expected.

### Step 3 — First boot: the setup portal

WiFi + server come from an on-device **SoftAP captive portal** (`firmware/main/provisioning.c`) — no
recompiling. Two triggers:

- **No usable creds at boot** (empty NVS + empty `secrets.h`) → portal auto-starts.
- **Hold the button ~20 s at wake** → portal (deliberate re-provision). A **~3 s hold** requests a
  refresh — the server re-renders the current frame with fresh data (when the device is driven by a
  rotation) and it repaints; a quick tap just wakes and checks for a new frame. (REST transport only.)

The AP is **`Tesserae-Setup`** (password `tesserae`, IP `192.168.4.1`); a DNS-hijack pops the captive
sheet on your phone. Fill in:

- **WiFi** network + password.
- **Server URL** — the Tesserae server, either its LAN IP or `<host>.local:8765` (the C3 resolves
  `.local` via mDNS). Use the LAN IP so it keeps working if your internet drops. `https://` also
  works if the server sits behind a reverse proxy with a **publicly-trusted** certificate (e.g.
  Let's Encrypt); self-signed certificates won't validate.
- **Device id** *(optional)* — a custom name (`picpak-1`, validated `^[a-z][a-z0-9_-]{1,31}$`); blank
  auto-derives `picpak-<mac>`. This is the id the device claims and shows in Tesserae.
- **Pairing code** *(optional)* — a 6-digit code from Tesserae's **Pair new device** to self-claim
  without an admin click; blank uses the discovery flow (admin clicks **Register**).

Save, and it reboots into the normal cycle. If the first boot after saving can't join the WiFi,
the portal reopens by itself with an error banner while you're still nearby: a wrong password
("the password looks wrong") or a wrong/nonexistent network name ("couldn't find the WiFi
network") — fix the field and save again. Transient outages don't trigger this; an
already-working frame never drops back to setup on a router blip. The server/broker URL is
deliberately not checked this way: if the Tesserae server happens to be down (e.g. restarting
its container), the frame just keeps retrying on its own and catches up when it returns — and a
genuinely wrong URL is fixed any time via the 20 s button-hold portal.
Credentials precedence is `NVS → secrets.h → empty`.
On the LAN the frame advertises its DHCP hostname as its **device id** (e.g. `picpak-red` — the
router's client list matches Tesserae's device list; `_` becomes `-`). An unnamed frame advertises
`tesserae-picpak-<mac>` (last three MAC bytes) so multiple PicPaks stay distinguishable.

## Going back to stock

### Restoring the stock firmware

Flash your `stock_backup.bin` back at offset `0x0` — but **not** as one monolithic stub-mode
write (it silently fails to persist on this chip). Two ways that work — **both
hardware-verified**:

- **ESP Launchpad** (easiest): <https://espressif.github.io/esp-launchpad/> in Chrome/Edge,
  DIY tab → add the backup at address `0x0` → flash.
- **esptool with `--no-stub`**:

  ```sh
  python -m esptool --chip esp32c3 -p <PORT> --no-stub write_flash --flash_size 16MB 0x0 stock_backup_1.bin
  ```

  Unlike the backup, the restore is **fast — about 5–10 minutes total**. It starts with a
  silent `Erasing flash...` phase of a minute or two with no progress output — **it looks
  stuck; it isn't. Don't unplug.** Then the write runs with a progress counter and ends with
  `Hash of data verified` (esptool confirming the data actually persisted).

  The check that matters is the boot: if the frame comes up running stock, the restore worked.
  If it doesn't boot, or the write fails partway, re-enter bootloader mode (unplug → hold the
  button → replug while holding → release when the port appears) and re-run with the same
  backup — repeating is safe.

### Factory reset (settings only) — for this custom firmware

Only for a device already running this Tesserae custom firmware: wipes its saved
WiFi/server/pairing but keeps the firmware — the device comes back up in the setup portal:

```sh
python -m esptool --chip esp32c3 -p <PORT> erase_region 0x9000 0x6000
```

**Do not run this on a device still running the stock firmware** — there the same flash region
holds the factory per-device data (serial number, radio calibration), and erasing it destroys
that data irrecoverably unless you have your full stock backup.

## How it works — Tesserae integration

### Transport modes

Both transports work; pick one in the captive portal (stored in NVS, switchable any time by
re-provisioning). Frame **bytes** are always fetched over HTTP(S) from a URL — the transport
only carries the signalling and telemetry.

| Mode | Status |
| --- | --- |
| `1` REST (default, recommended) | **working** — device polls the Tesserae REST API each wake; no broker needed |
| `0` MQTT | **working** — device reads a retained frame topic from an MQTT broker each wake; needs a broker (e.g. Mosquitto) reachable by both the server and the frame |

### Frame format

Raw, headerless, exactly **30 000 bytes** (`400 × 300 ÷ 4`), **2 bits per pixel**, 4 pixels per byte,
**MSB-first** (leftmost pixel in bits 7:6). Palette indices: `0`=Black, `1`=White, `2`=Yellow, `3`=Red.
Rows are packed **bottom-to-top** (the panel scans that way; the renderer flips vertically before
packing — otherwise the image paints upside-down).

The heartbeat reports `kind: "picpak_client"` and `panel_w: 400, panel_h: 300`. The matching
renderer and `picpak_client` device kind ship **built into the Tesserae server**.

### Heartbeat schema

Sent once per wake on either transport — REST `POST`s it to `/api/v1/device/<id>/status`, MQTT
publishes it retained (QoS 1) to `tesserae/<id>/status`. Always after the frame fetch, before any
paint (the radio is turned off for the panel refresh, so on repaint wakes the server's "last seen"
precedes the paint by its 13–22 s duration):

```json
{
  "battery_mv": 4164,
  "battery_pct": 96,
  "rssi": -63,
  "ip": "10.0.20.40",
  "fw_version": "0.7.0",
  "kind": "picpak_client",
  "panel_w": 400,
  "panel_h": 300,
  "sleep_interval_s": 900,
  "next_sleep_s": 900,
  "wake_reason": "timer",
  "sleep_until": 1784592900
}
```

`battery_pct` uses a piecewise-linear Li-Po discharge curve (`firmware/main/battpct.h`); `battery_mv`
comes from ADC1 ch2 (20-sample median + `adc_cali` curve-fitting, ×1.45 divider), measured once early
each wake before WiFi/EPD load the rail. `sleep_until` is the absolute epoch the device intends to
wake next (sent only with a plausible clock); the server cross-checks it against `next_sleep_s`.

### REST contract

Every wake hits `/api/v1/device/<id>/...` over HTTP — or HTTPS when the server URL uses it
(validated against ESP-IDF's built-in CA bundle; publicly-trusted certificates only):

| Method + path | Purpose |
| --- | --- |
| `POST /device/discover` | Unauthenticated. Posts identity (`device_id`, `kind`, `panel_w/h`, `fw_version`, `mac`). `device_id` is the portal's custom **Device id**, or `picpak-<mac3>` when left blank. `registered:false` → deep-sleep + retry next wake; `registered:true` → returns `device_token`. Default first-boot path. |
| `POST /device/register` | Opt-in: same body + `X-Pairing-Code` header (from the portal's optional Pairing-code field). Returns `device_token` (server auto-claims — no admin click). `403` clears the code and backs off; the code is single-use and burned on success. |
| `GET /device/<id>/frame` | `Authorization: Bearer <device_token>` + optional `If-None-Match`. `200` → `{url, format, panel_w, panel_h}` + `ETag` (fetch `url`, paint); `304` → skip; `204` → not rendered yet. |
| `POST /device/<id>/status` | `Bearer` auth. Body = the heartbeat JSON. Response `{config, next_poll_s, server_time}` — firmware applies `config.sleep_interval_s` to NVS and uses `next_poll_s` for this cycle's deep sleep. |

The `device_token` is persisted to NVS on first pairing; later wakes go straight to the frame GET. A
`401` or `403` on any authenticated request wipes the token and forces re-pairing. A relative `url`
in the frame response is resolved against the server origin.

### MQTT contract

One broker session per wake. Topics live under `tesserae/<device_id>/` (`device_id` from the
portal, or `picpak-<mac3>` when left blank — same fallback as REST):

| Topic | Direction | Payload |
| --- | --- | --- |
| `frame/bin` | server → device, **retained** | URL of the current frame binary — bare URL or `{"url": …}` JSON, both accepted |
| `config` | server → device, retained | `{"sleep_interval_s": N}` (validated 30 s – 7 d, saved to NVS) |
| `status` | device → broker, retained, QoS 1 | the heartbeat JSON above |

The wake: connect (optional username/password, `mqtts://` supported) → subscribe both topics →
wait ≤ 8 s for the retained frame message → skip if its URL matches the last painted one (the
MQTT equivalent of REST's `304`; the server's URLs are content-addressed) → else download over
HTTP and validate exactly 30 000 bytes → publish the heartbeat → disconnect gracefully → radio
off → paint.

There is no pairing token on MQTT: the retained heartbeat is what makes an unclaimed frame appear
under Tesserae's **Settings → Devices** for the admin to claim. The session carries a
**non-retained** last-will (`{"state":"offline"}`) so an ungraceful drop is visible to live
subscribers without ever clobbering the retained heartbeat. Wakes that find the broker down keep
the last image and retry on the next cycle — the frame never blanks.

Three panel screens (baked 2 bpp blobs, generated by `tools/gen_splash.py`, embedded via CMake):

- **Setup** — logo + `Tesserae-Setup` / `tesserae` — shown when the portal comes up so you can join.
- **Paired** — "Connected — waiting for first frame" — once after you submit, until the first photo.
- **Battery low** — "please charge" — shown by the low-battery gate.

### Power notes

- **Brownout mitigation.** PicPak's power delivery is marginal — at default TX power the WiFi current
  spike browns the board out on radio-on. Fixed with a low-power profile: cap PHY TX power to 10 dBm
  (`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10` — caps the PHY-cal spike), CPU @ 80 MHz, trimmed WiFi buffers,
  AMPDU off, plus a brownout-aware defer (on a power-fault reset, deep-sleep to let the cell recover).
- **Low-battery gate** (`firmware/main/lowbatt*`). Below ~0% — the bottom of the battery-percent curve,
  3300 mV — the frame paints the "charge me" screen and enters a 15-min low-power poll instead of the
  normal cycle, resuming only when the voltage rises (charging inferred — the C3 has no VBUS sense)
  or crosses ~2% (3500 mV). State lives in RTC-RAM; a button-wake always resumes.
  The decision is a pure, host-tested FSM.
- **Battery reading** is taken once early each wake (pre-load) and cached, so the reported value is
  accurate and there's a single ADC read per cycle.
- **Radio off during the panel refresh.** All network I/O (frame fetch + heartbeat, on either
  transport) finishes first, then WiFi stops, then the panel paints — the radio never idles
  (~80 mA) through the 13–22 s refresh, which is also the board's worst-case rail load (brownout
  margin). In MQTT mode the broker session is closed gracefully first, so the last-will never
  fires on a normal wake.
- **Lazy panel init.** The EPD is powered and initialized only when a new frame actually arrived;
  on the common "unchanged" wake (REST `304` / MQTT retained-URL match) the panel is never touched.
- **Single broker session per wake (MQTT).** Fetch, config, and heartbeat share one connection —
  no second connect and no post-paint WiFi reconnect (the reference firmware pays both). A failed
  broker connect tears down in ~0.5 s instead of esp-mqtt's default multi-second reconnect wait.
- **No NTP on the wake path.** The C3 keeps RTC time across deep sleep. REST mode over plain http
  never syncs (the server supplies time context); MQTT mode — and REST with an `https://` server
  URL — syncs only when the clock is implausible, effectively once per battery insertion, as
  required for TLS certificate validity checks.
- **WiFi fast-connect.** After the first association the AP's BSSID + primary channel are cached in
  NVS and reused on the next wake, so the radio skips the ~1–2 s all-channel scan (less radio-on
  time, a smaller brownout-sag window; both transports). A stale hint (AP moved channel / router
  swapped) fails fast and falls back to a full scan, which re-caches the real AP — one slower wake,
  then fast again.

## Project layout

```
picpak-tesserae-client/
├── firmware/
│   ├── CMakeLists.txt · partitions.csv · sdkconfig.defaults
│   └── main/
│       ├── main.c              # wake loop: boot → gesture/provision → gates → wifi → fetch → heartbeat → radio off → paint → sleep
│       ├── board.h · defaults.h # pin map + compile-time tunables (secrets.h optional override)
│       ├── epd_driver.{c,h}     # 400×300 BWRY UC81xx panel driver + init sequence
│       ├── fb2bpp.{c,h}         # 2 bpp framebuffer packer (+ host test)
│       ├── power.{c,h}          # battery ADC, deep sleep, boot-button gesture
│       ├── battpct.h            # pure mV→% Li-Po curve (host-tested)
│       ├── lowbatt*.{c,h}       # low-battery gate (pure FSM + RTC glue)
│       ├── config_store.{c,h}   # NVS config (creds, token, etag, broker, sleep) with secrets.h fallback
│       ├── wifi_manager.{c,h}   # STA connect (+ SNTP helper for MQTT-mode clock sanity)
│       ├── provisioning*.{c,h}  # SoftAP captive portal + pure form parser
│       ├── splash.{c,h}         # embedded setup / paired / low-batt screens
│       ├── image_fetcher.{c,h}  # HTTP frame download
│       ├── framebuf.{c,h}       # 30 KB frame staging buffer shared by both transports
│       ├── rest_handler.{c,h}   # REST: discover/register + frame GET + status POST
│       ├── mqtt_handler.{c,h}   # MQTT: retained frame/config read + heartbeat publish, one session
│       ├── mqtt_parse.{c,h}     # pure payload/URI helpers (+ host test)
│       └── heartbeat.{c,h}      # battery / RSSI / IP / panel JSON
├── tools/
│   ├── gen_splash.py            # generate the 2 bpp splash blobs
└── LICENSE                      # AGPL-3.0
```

## Disclaimer

This is a pure hobby project, provided as-is with no warranty of any kind. While everything works on my own devices, I take no responsibility for any damage resulting from its use — including but not limited to bricked devices, lost photos or other data, or voided warranties. Flash and use this firmware entirely at your own risk.

This is an independent, unofficial project — not affiliated with, endorsed by, or supported by
the PicPak manufacturer. "PicPak" and related names are used only to identify the hardware this
firmware runs on; all trademarks belong to their respective owners. This repository contains
**no code, firmware, or other proprietary material from the manufacturer**: the firmware is
original work (portions modelled on the AGPL-licensed Tesserae reference client), and the
hardware interface details (pin map, panel init sequence, battery calibration) were determined
by good-faith reverse engineering of the author's own device for the sole purpose of
interoperability. The stock firmware is not distributed here — every user backs up their own
device.

## Credits

The wake state machine, REST/MQTT/heartbeat contracts, captive-portal provisioning, and NVS schema
follow Tesserae's reference client
[tesserae-device-photopainter-7.3-bin](https://github.com/dmellok/tesserae-device-photopainter-7.3-bin).
The panel init sequence and 4-colour packing were reverse-engineered for the PicPak's specific 400×300
BWRY hardware.

## License

AGPL-3.0-or-later — © 2026 [varanu5](https://github.com/varanu5). See [LICENSE](https://github.com/varanu5/picpak-tesserae-client/blob/main/LICENSE).