# picpak-tesserae-client

**In plain terms:** this replaces the stock firmware on the PicPak photo frame so it shows photos
from *your own* self-hosted [Tesserae](https://github.com/dmellok/tesserae) server instead of the
vendor's cloud, with no vendor account and no subscription. You flash it once, tell the frame your WiFi and
server on a phone setup screen, and it fetches a new picture on its own schedule.

**Under the hood:** it's battery-powered firmware for the frame's **ESP32-C3**. Most of the time the
device is in deep sleep; on each scheduled wake it connects to WiFi, pulls the current frame from
Tesserae (over REST, MQTT, or cloud relay), paints the e-paper panel, reports a heartbeat (battery,
RSSI), and goes back to sleep.

Modelled on Tesserae's battery-native reference client
[tesserae-device-photopainter-7.3-bin](https://github.com/dmellok/tesserae-device-photopainter-7.3-bin),
but retargeted to the PicPak's hardware: a smaller 4-colour panel, an ESP32-C3 (RISC-V) instead of an
S3, no PMIC (battery is read straight off an ADC), and a 2-bits-per-pixel frame format.

> **Status:** working end-to-end over REST, MQTT, and cloud relay on real hardware. `FW_VERSION 0.9.7`.
> See [`CHANGELOG.md`](CHANGELOG.md) for release notes.
> Tested on PicPak **hardware revision v0.0.1**, migrating from **official firmware v1.1.11**
> to this firmware and back (stock restore verified).

## What you need before you start

- A **Tesserae server** already running on your network. *The frame is useless without one; it's the
  thing that sends the photos.* Set one up first: [Tesserae](https://github.com/dmellok/tesserae).
  (Or, for a frame in another location, a **cloud relay**; see
  [Cloud relay](#cloud-relay-remote-panels).)
- A **computer** (macOS, Windows, or Linux) to do the flashing, and roughly **2 hours free** for the
  one-time backup (it runs unattended, so you don't have to sit and watch).
- A **USB-C data cable**. Many cables are charge-only and won't work; if the computer never sees the
  frame, a wrong cable is the most common cause.
- Your **WiFi network name and password** (2.4 GHz, since the ESP32-C3 has no 5 GHz radio).

**Contents:**
[What you need](#what-you-need-before-you-start) ·
[Hardware](#hardware) ·
[Installing the firmware](#installing-the-firmware) (backup → flash → set up) ·
[Going back to stock](#going-back-to-stock) ·
[Building from source](#building-from-source) ·
[How it works: Tesserae integration](#how-it-works-tesserae-integration) ·
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
| Transport | MQTT + REST | **MQTT + REST + cloud relay** (REST is the recommended default; relay is for remote panels) |

**Pin map** (`firmware/main/board.h`): EPD `SCLK 6 · MOSI 3 · MISO 4 · CS 9 · DC 8 · RST 10 · BUSY 20`
(SPI @ 1 MHz); button `GPIO2` (active-low, shared with the battery ADC). The board also carries an
LSM6 IMU (`CS 7 · INT 5`), unused by this firmware. Cell: single-cell Li-Po, 3.7 V nominal, ~500 mAh.

## Installing the firmware

**Quick start (the happy path):**

1. **Back up** the stock firmware from the command line; this is your only way back to factory. *It is the
   one slow, one-time step (roughly 1.5 to 2 hours of unattended reading), and everything after it is quick.*
2. **Flash** this firmware. Easiest from the browser at
   <https://picpaktesserae.pages.dev> (Chrome/Edge), about two minutes.
3. **Set up** WiFi + server on the frame's own **`Tesserae-Setup`** WiFi screen from your phone.

The rest of this section is the detailed version of those three steps, with the command-line
alternatives and every warning worth reading first.

> ⚠️ **Never `erase_flash` this board.** A full chip erase of the 32 MB part fails partway and
> leaves the device half-wiped (recovery: just flash again, since small region writes work). No erase
> is needed for any step here; to wipe only the saved config, see
> [Factory reset](#factory-reset-settings-only-for-this-custom-firmware).

**Tested configuration: hardware rev v0.0.1, official firmware v1.1.11.** On this unit the
ESP32-C3's security eFuses are **not burned** (no Secure Boot, no Flash Encryption, USB
download mode unlocked), which is what makes the backup, this firmware, and a later stock
restore possible at all. eFuses are one-time-programmable: if the manufacturer ever ships
units (or an update that burns fuses) with these protections enabled, none of this will
work. If unsure, check first. `espefuse.py --chip esp32c3 -p <PORT> summary` should
show Secure Boot and Flash Encryption disabled before you proceed.

### Step 1: Back up the stock firmware (required)

> This is the long, one-time step: a couple of hours of the computer reading the chip by itself,
> and you never repeat it. The actual firmware flashing afterwards takes about two minutes.

**You cannot re-download the stock firmware; the backup is your only way back.** This step is
**command line only**: the browser flasher can *write* firmware but cannot *read* a full image,
so it can't make a backup. You need `esptool`.

#### Install esptool: one recommended way per platform

`esptool` is the small tool that reads and writes the frame's memory. Install it once. **Pick the
one line for your computer and ignore the others:**

- **macOS**: run `brew install esptool`
- **Windows**: install [Python](https://python.org/downloads) (**tick "Add python.exe to PATH"**
  during setup), then run `pip install esptool`
- **Linux**: run `pipx install esptool` (or your distro's package, e.g. `apt install esptool`)

After that, **your command is `esptool.py`**, which is exactly how every command in this guide is
written, so you can copy them as-is (just fill in your port). On Windows, if `esptool.py` isn't
found, use `py -m esptool` with the same arguments.

<details>
<summary><b>Didn't work, or want no Python at all? Other install methods & command names</b></summary>

- **Standalone binary (no Python needed)**: grab it from
  [esptool releases](https://github.com/espressif/esptool/releases) (`esptool-*-macos*.zip`,
  `esptool-*-windows-amd64.zip`, or `esptool-*-linux-amd64.zip`), unzip, and run the binary
  directly. The command name is then `./esptool` (macOS/Linux) or `esptool.exe` (Windows), run
  from the unzipped folder.
- **macOS note**: Homebrew bundles its own Python; a plain `pip install esptool` into Homebrew's
  Python fails with `externally-managed-environment`, which is why `brew install esptool` is
  recommended above.
- **Windows note**: Windows Python has no `externally-managed` restriction, so `pip install`
  just works. If `python` isn't found, use `py -m pip …` and `py -m esptool …`.
- **Linux note**: a distro-managed Python also rejects a bare `pip install`, so prefer `pipx`.

All of these are the same tool; `esptool.py`, `python -m esptool`, `./esptool`, and `esptool.exe`
are interchangeable in every command below.

</details>

#### Find your `<PORT>`

- **macOS**: `ls /dev/cu.usbmodem*` (e.g. `/dev/cu.usbmodem1101`).
- **Linux**: `ls /dev/ttyACM*` (e.g. `/dev/ttyACM0`).
- **Windows**: Device Manager → Ports (COM & LPT) → **"USB Serial Device (COMx)"**; use `COM<x>`.

The number changes when you replug into a different USB port, so re-check rather than reusing last
time's name.

#### Run the backup: read the lower 16 MB with `--no-stub`

> ⚠️ **`<PORT>` is a placeholder; don't paste it literally.** Replace it with your actual port from
> the step just above (e.g. `/dev/cu.usbmodem1101` on a Mac, `/dev/ttyACM0` on Linux, `COM5` on
> Windows). If you leave `<PORT>` in, the command fails.

```sh
# backup: two reads chained together, keeping the chip in the ROM loader so the
# frame's firmware never boots between them (replace <PORT> with your port).
# --after no_reset leaves the chip in the loader after the first read;
# --before no_reset tells the second read not to reset it, so both reads see
# the exact same, static flash.
esptool.py --chip esp32c3 -p <PORT> -b 921600 --no-stub --after no_reset read_flash 0x0 0x1000000 stock_backup_1.bin && \
esptool.py --chip esp32c3 -p <PORT> -b 921600 --no-stub --before no_reset read_flash 0x0 0x1000000 stock_backup_2.bin
```

**What a finished command looks like** (Mac, port `/dev/cu.usbmodem1101`, esptool installed via brew):

```sh
esptool.py --chip esp32c3 -p /dev/cu.usbmodem1101 -b 921600 --no-stub --after no_reset read_flash 0x0 0x1000000 stock_backup_1.bin && \
esptool.py --chip esp32c3 -p /dev/cu.usbmodem1101 -b 921600 --no-stub --before no_reset read_flash 0x0 0x1000000 stock_backup_2.bin
```

#### Verify: the two hashes must be identical

A raw `read_flash` has no error check, so two matching reads is the only proof the backup is good.

- **macOS / Linux**: `shasum -a 256 stock_backup_1.bin stock_backup_2.bin`
- **Windows (PowerShell)**: `Get-FileHash stock_backup_1.bin, stock_backup_2.bin -Algorithm SHA256`
- **Windows (Command Prompt)**: `certutil -hashfile stock_backup_1.bin SHA256` (repeat for file 2)

Only trust the backup once both hashes match.

The `--after no_reset` / `--before no_reset` flags above are what make that possible. If esptool
is allowed to reset the chip between the two reads (its default), the frame boots, wakes, and
repaints — which rewrites its photo cache (the `framestore` partition) and can touch saved
settings. Those changed bytes mean the two dumps would never match, even though the flash is fine.
Keeping the chip in the ROM loader across both reads stops the firmware from running in between, so
the bytes stay put.

> **esptool v4 vs v5:** the underscore spelling above (`read_flash`, `no_reset`, `esptool.py`) works
> on **both** v4 and v5. On v5 it just prints a harmless deprecation warning. v5's own native
> spelling uses hyphens (`read-flash`, `--after no-reset`, and the `esptool` command without `.py`),
> but that hyphen form does **not** work on v4, so the underscores shown here are the safe choice if
> you're not sure which version you have.

**Why 16 MB when the chip says 32?** The flash identifies itself as 32 MB, but full 32 MB
dumps (taken twice on real hardware and compared) show the upper half reads back as a
**byte-perfect mirror of the lower half** (it holds no data of its own), and the entire
stock system (bootloader, partition table, both app slots, photo storage) lives below 16 MB.
Restoring a lower-16 MB backup has been verified to boot stock. Reading 32 MB just captures
the same data twice and doubles the wait. Never *write* anything above the 16 MB boundary,
because on this chip such writes can wrap around and corrupt the bootloader.

**Why `--no-stub`?** esptool's default *stub* flasher is unreliable for large transfers on
this flash chip (it can report success while the data is actually corrupt), so the backup
must go through the ROM loader instead. The ROM loader moves data in small acknowledged
chunks, so **expect a long wait of roughly 1.5 to 2 hours per read**; a higher baud rate won't
help (this board's native USB ignores it). Keep the frame plugged in and prevent your
computer from sleeping.

Keep one file and its checksum somewhere safe. The backup is
**per-device**: the stock settings region at `0x9000` holds factory data unique to your unit
(serial number, radio calibration), and the first flash of this firmware overwrites it, so
someone else's backup or a shared stock image cannot fully restore your frame.

### Step 2: Flash the release build

> **Easiest: flash from the browser at <https://picpaktesserae.pages.dev>** (Chrome or Edge).
> Fresh install or a settings-keeping update, plus a read-only serial monitor, with no tools to
> install. (Flashing only; the browser can't make the Step 1 backup.) The esptool commands
> below do the same flashing from the command line.

Each [release](../../releases) ships an all-in-one image, its four component files, and
`SHA256SUMS`; for the command-line route you need esptool, see
[Step 1](#step-1-back-up-the-stock-firmware-required) for the per-platform install:

| File | Flash offset | |
| --- | --- | --- |
| `picpak-tesserae-firmware.bin` | `0x0` | **all-in-one image** (the four files below merged); simplest for a first install; always wipes saved settings |
| `bootloader.bin` | `0x0` | |
| `partition-table.bin` | `0x8000` | |
| `nvs_blank.bin` | `0x9000` | blank settings; guarantees the setup portal on first boot; **omit when upgrading** to keep saved WiFi/pairing |
| `picpak-tesserae-client.bin` | `0x10000` | |

**Finding the port:** the C3's native USB-Serial-JTAG shows up as `/dev/cu.usbmodem*` on macOS
(`/dev/ttyACM*` on Linux; on Windows as "USB Serial Device (COMx)" under Device Manager →
Ports (COM & LPT), use `COM<x>`). List it with `ls /dev/cu.usbmodem*`. The number encodes the
USB port/hub position, so it **changes when you replug into a different port**; re-check it
rather than assuming last time's name.

First install, all-in-one image (**replace `<PORT>`** with your real port, e.g.
`/dev/cu.usbmodem1101`):

```sh
esptool.py --chip esp32c3 -p <PORT> -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 picpak-tesserae-firmware.bin
```

Same install from the individual files (use this form **when upgrading**, leaving out
`0x9000 nvs_blank.bin` to keep your saved settings, since the all-in-one image always wipes them):

```sh
esptool.py --chip esp32c3 -p <PORT> -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x9000 nvs_blank.bin 0x10000 picpak-tesserae-client.bin
```

If the connection drops or fails to sync, use a different USB-C **data** cable and a direct
port (no hub); the baud rate doesn't matter on this board's native USB. *"The port is busy or
doesn't exist"* means something else holds the port, usually an open serial monitor; close it
and re-run.

Note: **the partition layout changes** (stock dual-OTA → our single factory app), so the
vendor's update/OTA tools stop recognizing the device until you restore stock, which is expected.

### Step 3: First boot and the setup portal

WiFi + server come from an on-device **SoftAP captive portal** (`firmware/main/provisioning.c`), with no
recompiling. It opens automatically when there are no usable creds at boot (empty NVS + empty
`secrets.h`), or on demand via the ~20 s button hold below. The portal also lets you pick a
**refresh speed** (5 s (default), 10 s, or the panel's native waveform), a trade-off between how
fast the screen redraws and colour fidelity; the choice is saved and can be changed any time. The
fast waveforms also get a small automatic temperature compensation (from the on-chip sensor); it
needs no setup.

The frame's **single button** is classified by how long you hold it at wake (deck-next and refresh
are **REST transport only**):

- **Quick tap**: a plain press-and-release → wakes now and checks the server for a new photo. This
  is the "just show me the latest" press: nothing is navigated, and a directly-pushed image comes
  through as normal.
- **Brief hold (~0.5 s, anything held under 5 s)** → flips to the **next page of the device's Deck**
  in Tesserae (manual deck navigation; the wake sends `?button=right`). Needs a Deck bound to the
  device; with none it is a harmless no-op that behaves like a quick tap.
- **Hold ~5 s (5–10 s)** → **force refresh**: the server re-renders the current page with fresh data
  and the frame repaints. This is also the deliberate override that resumes a battery-locked device.
- **Hold ~10 s (10–20 s)** → **Bluetooth maintenance** in Tesserae Companion.
  Read diagnostics, repair Wi-Fi, or change refresh speed without a reachable server.
  A QR code and passkey authorize a five-minute session. See [Bluetooth maintenance](docs/ble-maintenance.md).
  Sitting above the refresh window gives the tap a wide gesture window and keeps an over-held
  refresh from ever opening a BLE session by accident.
- **Hold ~20 s** → reopens the setup portal (deliberate re-provision of WiFi/server).
- **Status LED** (`firmware/main/led.{c,h}`, GPIO21). Screen-free status: **one blink on every wake**
  (timer or button); while holding the button the feedback steps off → **steady-on** past the ~5 s
  refresh point → **pulsing** at ~10 s for Bluetooth → **rapid burst** at the ~20 s provisioning
  point, so you can feel the timing without
  watching the panel. The console runs on USB-Serial-JTAG (`/dev/cu.usbmodem*`), so GPIO21, which is
  also the UART0 TX pin, carries no log traffic and is a clean, dedicated LED.

The AP is **`Tesserae-Setup`** (password `tesserae`, IP `192.168.4.1`); a DNS-hijack pops the captive
sheet on your phone. Fill in:

- **WiFi** network + password.
- **Server URL**: the Tesserae server, either its LAN IP or `<host>.local:8765` (the C3 resolves
  `.local` via mDNS). Use the LAN IP so it keeps working if your internet drops. `https://` also
  works if the server sits behind a reverse proxy with a **publicly-trusted** certificate (e.g.
  Let's Encrypt); self-signed certificates won't validate.
- **Device id** *(optional)*: a custom name (`picpak-1`, validated `^[a-z][a-z0-9_-]{1,31}$`); blank
  auto-derives `picpak-<mac>`. This is the id the device claims and shows in Tesserae.
- **Pairing code** *(optional)*: a 6-digit code from Tesserae's **Pair new device** to self-claim
  without an admin click; blank uses the discovery flow (admin clicks **Register**).

Save, and it reboots into the normal cycle. If the first boot after saving can't join the WiFi,
the portal reopens by itself with an error banner while you're still nearby: a wrong password
("the password looks wrong") or a wrong/nonexistent network name ("couldn't find the WiFi
network"). Fix the field and save again. Transient outages don't trigger this; an
already-working frame never drops back to setup on a router blip. The server/broker URL is
deliberately not checked this way: if the Tesserae server happens to be down (e.g. restarting
its container), the frame just keeps retrying on its own and catches up when it returns, and a
genuinely wrong URL is fixed any time via the 20 s button-hold portal.
Credentials precedence is `NVS → secrets.h → empty`.
On the LAN the frame advertises its DHCP hostname as its **device id** (e.g. `picpak-red`; the
router's client list matches Tesserae's device list; `_` becomes `-`). An unnamed frame advertises
`tesserae-picpak-<mac>` (last three MAC bytes) so multiple PicPaks stay distinguishable.

## Going back to stock

### Restoring the stock firmware

Flash your `stock_backup.bin` back at offset `0x0`, but **not** as one monolithic stub-mode
write (it silently fails to persist on this chip). Two ways that work, **both
hardware-verified**:

- **ESP Launchpad** (easiest): <https://espressif.github.io/esp-launchpad/> in Chrome/Edge,
  DIY tab → add the backup at address `0x0` → flash.
- **esptool with `--no-stub`**:

  ```sh
  esptool.py --chip esp32c3 -p <PORT> --no-stub write_flash --flash_size 16MB 0x0 stock_backup_1.bin
  ```

  Unlike the backup, the restore is **fast, about 5 to 10 minutes total**. It starts with a
  silent `Erasing flash...` phase of a minute or two with no progress output. **It looks
  stuck; it isn't. Don't unplug.** Then the write runs with a progress counter and ends with
  `Hash of data verified` (esptool confirming the data actually persisted).

  The check that matters is the boot: if the frame comes up running stock, the restore worked.
  If it doesn't boot, or the write fails partway, re-enter bootloader mode (unplug → hold the
  button → replug while holding → release when the port appears) and re-run with the same
  backup; repeating is safe.

### Factory reset (settings only): for this custom firmware

Only for a device already running this Tesserae custom firmware: wipes its saved
WiFi/server/pairing but keeps the firmware, so the device comes back up in the setup portal:

```sh
esptool.py --chip esp32c3 -p <PORT> erase_region 0x9000 0x6000
```

**Do not run this on a device still running the stock firmware.** There the same flash region
holds the factory per-device data (serial number, radio calibration), and erasing it destroys
that data irrecoverably unless you have your full stock backup.

## How it works: Tesserae integration

### Transport modes

All three transports work; pick one in the captive portal (stored in NVS, switchable any time by
re-provisioning). In **REST** and **MQTT** the frame **bytes** are fetched over HTTP(S) from a URL
and the transport only carries the signalling and telemetry; the **cloud relay** instead carries the
(encrypted) frame itself through its mailbox; see [Cloud relay](#cloud-relay-remote-panels).

| Mode | Status |
| --- | --- |
| `1` REST (default, recommended) | **working**: device polls the Tesserae REST API each wake; no broker needed |
| `0` MQTT | **working**: device reads a retained frame topic from an MQTT broker each wake; needs a broker (e.g. Mosquitto) reachable by both the server and the frame |
| Cloud relay | **new**: for a panel that can't reach your Tesserae server directly (another home, CGNAT, a hotspot). Mutually exclusive with REST/MQTT: when a relay is paired the device talks only to the relay. See below. |

### Cloud relay (remote panels)

A relay panel and your home Tesserae instance both connect **outbound** to a small mailbox Worker;
home seals each rendered frame and `PUT`s it, the panel polls and decrypts it, so the home server is
never exposed to the internet. The relay is **zero-knowledge**: at pairing the two ends exchange
X25519 **public** keys through it and each derive the same AES-256-GCM frame key locally (the key is
never transmitted), so the relay only ever holds ciphertext, two public keys and a token hash. A
failed authentication tag is never painted. Frame crypto is in `firmware/main/relay_crypto.c` (X25519
via vendored Monocypher, HKDF-SHA256 + AES-GCM via mbedTLS), pinned to the relay contract's golden
vectors by `tools/test_relay_crypto.sh`.

**Provisioning:** in the setup portal pick **Cloud relay**, enter the relay URL (defaults to the
hosted `https://relay.tesserae.ink`) and a single-use pairing code from your Tesserae server
(*Settings → Cloud relay → Add a remote panel*). No server URL is needed. Pairing may take a couple of
wakes to complete; after that each wake fetches the current frame, posts telemetry, and adopts the
server-set sleep interval, the same status body the Devices UI shows for a local device.

**Removing or re-pairing:** to retire a relay panel, use the per-device **Remove** button on
*Settings → Cloud relay* (it revokes the relay mailbox and deletes the device). To move a panel to a
fresh pairing (or to recover a deleted/re-added one), reopen setup (20 s button hold) and enter a
new pairing code; the panel drops its old pairing and re-pairs cleanly (no factory reset needed). The
device-id field is hidden on relay, since a relay panel's identity comes from pairing, not that field.

**Button gestures work over relay on Tesserae server/relay v0.240.0+.** The press rides the status
body (`button`+`button_event_id`); a tap navigates the deck (`right`) and a 5 s-hold refreshes.
Because relay delivery is store-and-forward, the panel stays awake ~45 s after a press, polling for
home's rendered response (~30 s for the first press) and painting it. On an older server the button
fields are ignored (harmless). MQTT remains button-less (push-only transport). *(A revoked panel also
auto-recovers on v0.240.0+: two consecutive `401` wakes → the panel drops its pairing, paints an
**"Unpaired"** screen, and re-pairs on a fresh code via the 20 s-hold portal, URL pre-filled.)*

### Frame format

Raw, headerless, exactly **30 000 bytes** (`400 × 300 ÷ 4`), **2 bits per pixel**, 4 pixels per byte,
**MSB-first** (leftmost pixel in bits 7:6). Palette indices: `0`=Black, `1`=White, `2`=Yellow, `3`=Red.
Rows are packed **bottom-to-top** (the panel scans that way; the renderer flips vertically before
packing, otherwise the image paints upside-down).

The heartbeat reports `kind: "picpak_client"` and `panel_w: 400, panel_h: 300`. The matching
renderer and `picpak_client` device kind ship **built into the Tesserae server**.

### Heartbeat schema

Sent once per wake on either transport: REST `POST`s it to `/api/v1/device/<id>/status`, MQTT
publishes it retained (QoS 1) to `tesserae/<id>/status`. Always after the frame fetch, before any
paint (the radio is turned off for the panel refresh, so on repaint wakes the server's "last seen"
precedes the paint by its 13 to 22 s duration):

```json
{
  "battery_mv": 4164,
  "battery_pct": 96,
  "rssi": -63,
  "ip": "10.0.20.40",
  "fw_version": "0.9.7",
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

Every wake hits `/api/v1/device/<id>/...` over HTTP, or HTTPS when the server URL uses it
(validated against ESP-IDF's built-in CA bundle; publicly-trusted certificates only):

| Method + path | Purpose |
| --- | --- |
| `POST /device/discover` | Unauthenticated. Posts identity (`device_id`, `kind`, `panel_w/h`, `fw_version`, `mac`). `device_id` is the portal's custom **Device id**, or `picpak-<mac3>` when left blank. `registered:false` → deep-sleep + retry next wake; `registered:true` → returns `device_token`. Default first-boot path. |
| `POST /device/register` | Opt-in: same body + `X-Pairing-Code` header (from the portal's optional Pairing-code field). Returns `device_token` (server auto-claims, no admin click). `403` clears the code and backs off; the code is single-use and burned on success. |
| `GET /device/<id>/frame` | `Authorization: Bearer <device_token>` + optional `If-None-Match`. `200` → `{url, format, panel_w, panel_h}` + `ETag` (fetch `url`, paint); `304` → skip; `204` → not rendered yet. |
| `POST /device/<id>/status` | `Bearer` auth. Body = the heartbeat JSON. Response `{config, next_poll_s, server_time}`: firmware applies `config.sleep_interval_s` to NVS and uses `next_poll_s` for this cycle's deep sleep. |

The `device_token` is persisted to NVS on first pairing; later wakes go straight to the frame GET. A
`401` or `403` on any authenticated request wipes the token and forces re-pairing. A relative `url`
in the frame response is resolved against the server origin.

### MQTT contract

One broker session per wake. Topics live under `tesserae/<device_id>/` (`device_id` from the
portal, or `picpak-<mac3>` when left blank; same fallback as REST):

| Topic | Direction | Payload |
| --- | --- | --- |
| `frame/bin` | server → device, **retained** | URL of the current frame binary, either a bare URL or `{"url": …}` JSON, both accepted |
| `config` | server → device, retained | `{"sleep_interval_s": N}` (validated 30 s to 7 d, saved to NVS) |
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
the last image and retry on the next cycle; the frame never blanks.

Four panel screens (baked 2 bpp blobs, generated by `tools/gen_splash.py`, embedded via CMake):

- **Setup**: logo + `Tesserae-Setup` / `tesserae`, shown when the portal comes up so you can join.
- **Paired**: "Connected, waiting for first frame", shown once after you submit, until the first photo.
- **Battery low**: "please charge", shown by the low-battery gate.
- **Unpaired**: shown when a relay panel's pairing is revoked (see [Cloud relay](#cloud-relay-remote-panels)), until you re-pair with a fresh code.

### Power notes

- **Brownout mitigation.** PicPak's power delivery is marginal: at default TX power the WiFi current
  spike browns the board out on radio-on. Fixed with a low-power profile: cap PHY TX power to 10 dBm
  (`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10`, which caps the PHY-cal spike), CPU @ 80 MHz, trimmed WiFi buffers,
  AMPDU off, plus a brownout-aware defer (on a power-fault reset, deep-sleep to let the cell recover).
- **Low-battery gate** (`firmware/main/lowbatt*`). At 0% (the bottom of the battery-percent curve,
  3400 mV) the frame paints the "charge me" screen and enters a 24-h low-power poll instead of the
  normal cycle, resuming only when the voltage rises (charging inferred, as the C3 has no VBUS sense)
  or crosses ~5% (3550 mV). State lives in RTC-RAM; a button-wake always resumes, and after plugging
  in a charger, pressing the button is the fast way back rather than waiting for the next daily poll.
  The decision is a pure, host-tested FSM.
- **Battery reading** is taken once early each wake (pre-load) and cached, so the reported value is
  accurate and there's a single ADC read per cycle.
- **Radio off during the panel refresh.** All network I/O (frame fetch + heartbeat, on either
  transport) finishes first, then WiFi stops, then the panel paints, so the radio never idles
  (~80 mA) through the 13 to 22 s refresh, which is also the board's worst-case rail load (brownout
  margin). In MQTT mode the broker session is closed gracefully first, so the last-will never
  fires on a normal wake.
- **Lazy panel init.** The EPD is powered and initialized only when a new frame actually arrived;
  on the common "unchanged" wake (REST `304` / MQTT retained-URL match) the panel is never touched.
- **Panel put fully to sleep after a paint** (`firmware/main/epd_driver.c`). `epd_sleep()` sends the
  Deep Sleep command *with* its required `0xA5` check-code (a bare opcode is ignored by this
  controller, leaving the panel in standby); the next wake's reset brings it back before painting.
  This lowers the panel's own standby draw between wakes.
- **Single broker session per wake (MQTT).** Fetch, config, and heartbeat share one connection:
  no second connect and no post-paint WiFi reconnect (the reference firmware pays both). A failed
  broker connect tears down in ~0.5 s instead of esp-mqtt's default multi-second reconnect wait.
- **No NTP on the wake path.** The C3 keeps RTC time across deep sleep. REST mode over plain http
  never syncs (the server supplies time context); MQTT mode (and REST with an `https://` server
  URL) syncs only when the clock is implausible, effectively once per battery insertion, as
  required for TLS certificate validity checks.
- **WiFi fast-connect.** After the first association the AP's BSSID + primary channel are cached in
  NVS and reused on the next wake, so the radio skips the ~1 to 2 s all-channel scan (less radio-on
  time, a smaller brownout-sag window; both transports). A stale hint (AP moved channel / router
  swapped) fails fast and falls back to a full scan, which re-caches the real AP: one slower wake,
  then fast again.

## Project layout

```
picpak-tesserae-client/
├── firmware/                     # ESP-IDF app for the ESP32-C3
│   ├── CMakeLists.txt · partitions.csv · sdkconfig.defaults
│   └── main/
│       ├── main.c                # wake loop: boot → gesture/provision → gates → wifi → fetch → heartbeat → radio off → paint → sleep
│       ├── board.h · defaults.h  # pin map + compile-time tunables
│       ├── secrets.example.h     # template for baked-in creds/broker
│       ├── epd_driver.{c,h} · epd_init_seq.h   # 400×300 BWRY UC81xx panel driver + init sequence
│       ├── fb2bpp.{c,h}          # 2 bpp framebuffer packer (+ host test)
│       ├── framebuf.{c,h}        # 30 KB frame staging buffer shared by all transports
│       ├── power.{c,h}           # battery ADC, deep sleep, boot-button gesture
│       ├── battpct.h             # pure mV→% Li-Po curve (host-tested)
│       ├── lowbatt.{c,h} · lowbatt_core.h      # low-battery gate (pure FSM + RTC glue)
│       ├── led.{c,h}             # status LED (GPIO21) blink / hold-progression feedback
│       ├── config_store.{c,h}    # NVS config (creds, token, etag, broker, sleep)
│       ├── wifi_manager.{c,h}    # STA connect (+ SNTP helper for MQTT-mode clock sanity)
│       ├── provisioning.{c,h} · provision_form.{c,h}   # SoftAP captive portal + pure form parser
│       ├── splash.{c,h} · assets/splash_*.bin  # embedded setup / paired / low-batt / revoked screens
│       ├── image_fetcher.{c,h}   # HTTP frame download
│       ├── rest_handler.{c,h} · rest_button.h  # REST: discover/register + frame GET + status POST (+ button/deck dispatch)
│       ├── mqtt_handler.{c,h}    # MQTT: retained frame/config read + heartbeat publish, one session
│       ├── mqtt_parse.{c,h}      # pure payload/URI helpers (+ host test)
│       ├── relay.{c,h} · relay_crypto.{c,h} · relay_wire.{c,h}   # cloud-relay transport (X25519 + AES-GCM remote panels)
│       ├── vendor/monocypher.{c,h}             # vendored crypto for the relay transport
│       └── heartbeat.{c,h}       # battery / RSSI / IP / panel JSON
├── tools/                        # gen_splash.py (splash blobs)
├── CHANGELOG.md
├── README.md
└── LICENSE                       # AGPL-3.0
```

## Disclaimer

This is a pure hobby project, provided as-is with no warranty of any kind. While everything works on my own devices, I take no responsibility for any damage resulting from its use, including but not limited to bricked devices, lost photos or other data, or voided warranties. Flash and use this firmware entirely at your own risk.

This is an independent, unofficial project, not affiliated with, endorsed by, or supported by
the PicPak manufacturer. "PicPak" and related names are used only to identify the hardware this
firmware runs on; all trademarks belong to their respective owners. This repository contains
**no code, firmware, or other proprietary material from the manufacturer**: the firmware is
original work (portions modelled on the AGPL-licensed Tesserae reference client), and the
hardware interface details (pin map, panel init sequence, battery calibration) were determined
by good-faith reverse engineering of the author's own device for the sole purpose of
interoperability. The stock firmware is not distributed here; every user backs up their own
device.

## Credits

The wake state machine, REST/MQTT/heartbeat contracts, captive-portal provisioning, and NVS schema
follow Tesserae's reference client
[tesserae-device-photopainter-7.3-bin](https://github.com/dmellok/tesserae-device-photopainter-7.3-bin).
The panel init sequence and 4-colour packing were reverse-engineered for the PicPak's specific 400×300
BWRY hardware.

## License

AGPL-3.0-or-later. © 2026 [varanu5](https://github.com/varanu5). See [LICENSE](https://github.com/varanu5/picpak-tesserae-client/blob/main/LICENSE).
