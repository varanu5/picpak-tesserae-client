# picpak-tesserae-client

**In plain terms:** this replaces the stock firmware on the PicPak photo frame so it shows photos
from *your own* self-hosted [Tesserae](https://github.com/dmellok/tesserae) server instead of the
vendor's cloud, with no vendor account and no subscription. You flash it once, tell the frame your WiFi and
server on a phone setup screen, and it fetches a new picture on its own schedule.

**Under the hood:** it's battery-powered firmware for the frame's **ESP32-C3**. Most of the time the
device is in deep sleep; on each scheduled wake it connects to WiFi, pulls the current frame from
Tesserae (over REST, MQTT, or cloud relay), reports a heartbeat (battery and RSSI), turns off WiFi,
paints any new frame, and goes back to sleep. An optional Manual (Bluetooth) mode receives
photos locally from a compatible Tesserae Companion app without a server.

Modelled on Tesserae's battery-native reference client
[tesserae-device-photopainter-7.3-bin](https://github.com/dmellok/tesserae-device-photopainter-7.3-bin),
but retargeted to the PicPak's hardware: a smaller 4-colour panel, an ESP32-C3 (RISC-V) instead of an
S3, no PMIC (battery is read straight off an ADC), and a 2-bits-per-pixel frame format.

> **Status:** REST, MQTT, and cloud relay work on real hardware. `FW_VERSION 0.9.10`.
> See [`CHANGELOG.md`](CHANGELOG.md) for release notes.
> Tested on PicPak **hardware revision v0.0.1**, migrating from **official firmware v1.1.11**
> to this firmware and back (stock restore verified).

## What you need before you start

- For automatic updates, a **Tesserae server** already running on your network.
  Set one up first: [Tesserae](https://github.com/dmellok/tesserae).
  (Or, for a frame in another location, a **cloud relay**; see
  [Cloud relay](#cloud-relay-remote-panels).)
- A **computer** (macOS, Windows, or Linux) to do the flashing, and **several hours** for the
  one-time verified backup (two unattended reads, roughly 1.5 to 2 hours each on the tested unit).
- A **USB-C data cable**. Many cables are charge-only and won't work; if the computer never sees the
  frame, a wrong cable is the most common cause.
- Your **WiFi network name and password** (2.4 GHz, since the ESP32-C3 has no 5 GHz radio).

**Contents:**
[What you need](#what-you-need-before-you-start) ·
[Hardware](#hardware) ·
[Installing the firmware](#installing-the-firmware) (backup → flash → set up) ·
[Going back to stock](#going-back-to-stock) ·
[How it works: Tesserae integration](#how-it-works-tesserae-integration) ·
[Disclaimer](#disclaimer) ·
[License](#license)

## Hardware

| Component | PicPak 4.2" |
| --- | --- |
| SoC | ESP32-C3 (RISC-V), 16 MiB usable flash |
| Panel | 400×300, four colours: black, white, yellow and red |
| Frame size | 30 000 bytes, 2 bits per pixel |
| Application storage | Two 4 MiB slots, reserved for future OTA updates |
| Photo storage | 7.8125 MiB reserved, offline gallery not implemented |
| Panel power | Direct, no PMIC |
| Battery sense | ADC1 channel 2 (GPIO2), ×1.45 divider |
| User button | GPIO2, shared with the battery ADC |
| Network transports | REST, MQTT and cloud relay |
| Local photos | Manual Bluetooth mode through compatible Tesserae Companion |

**Pin map** (`firmware/main/board.h`): EPD `SCLK 6 · MOSI 3 · MISO 4 · CS 9 · DC 8 · RST 10 · BUSY 20`
(SPI @ 1 MHz); button `GPIO2` (active-low, shared with the battery ADC). The board also carries an
LSM6 IMU (`CS 7 · INT 5`), unused by this firmware. Cell: single-cell Li-Po, 3.7 V nominal, ~500 mAh.

## Installing the firmware

**Quick start (the happy path):**

1. **Back up** the stock firmware from the command line; this is your only way back to factory. *It is the
   slow, one-time step: allow several hours for the two reads and compare their checksums.*
2. **Flash** this firmware. Easiest from the browser at
   <https://picpaktesserae.pages.dev> (Chrome/Edge), about two minutes.
3. **Set up** WiFi + server on the frame's own **`Tesserae-Setup-XXXX`** WiFi screen from your phone.

The rest of this section is the detailed version of those three steps, with the command-line
alternatives and every warning worth reading first.

> ⚠️ **Never `erase_flash` this board.** A full chip erase of the 32 MB part fails partway and
> leaves the device half-wiped (recovery: just flash again, since small region writes work). No separate full erase
> is needed. Flashing erases only the regions being written. To clear saved settings, see
> [Factory reset](#factory-reset-settings-only-for-this-custom-firmware).

**Tested configuration: hardware rev v0.0.1, official firmware v1.1.11.** On this unit the
ESP32-C3's security eFuses are **not burned** (no Secure Boot, no Flash Encryption, USB
download mode unlocked), which is what makes the backup, this firmware, and a later stock
restore possible at all. eFuses are one-time-programmable: if the manufacturer ever ships
units (or an update that burns fuses) with these protections enabled, none of this will
work. If unsure, check first. `espefuse.py --chip esp32c3 -p <PORT> summary` should
show Secure Boot and Flash Encryption disabled before you proceed.

### Step 1: Back up the stock firmware (required)

> This is the long, one-time step: two reads of your own device, followed by a checksum
> comparison. The actual firmware flashing afterwards takes about two minutes.

**You cannot re-download the stock firmware; the backup is your only way back.** This step is
**command line only**: this browser flasher does not provide a full stock-backup export. You need `esptool`.

#### Install esptool: one recommended way per platform

`esptool` is the small tool that reads and writes the frame's memory. Install it once. **Pick the
one line for your computer and ignore the others:**

- **macOS**: run `brew install esptool`
- **Windows**: install [Python](https://python.org/downloads) (**tick "Add python.exe to PATH"**
  during setup), then run `pip install esptool`
- **Linux**: run `pipx install esptool` (or your distro's package, e.g. `apt install esptool`)

The examples below use the esptool v4 spelling, `esptool.py`. Esptool v5 still accepts
these commands with deprecation warnings. If your installation provides `esptool` instead,
replace `esptool.py` with `esptool` in each command. On Windows you can also use
`py -m esptool`. See Espressif's [v5 migration guide](https://docs.espressif.com/projects/esptool/en/latest/esp32/migration-guide.html).

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

Do not unplug or reset the frame between reads. The first command leaves it in the ROM loader,
and the second reads without starting the installed firmware.

**macOS / Linux:** run this chained command. The second read starts automatically only if
the first succeeds. Replace both occurrences of `<PORT>` with the same port.

```sh
esptool.py --chip esp32c3 -p <PORT> -b 921600 --no-stub --after no_reset read_flash 0x0 0x1000000 stock_backup_1.bin && \
esptool.py --chip esp32c3 -p <PORT> -b 921600 --no-stub --before no_reset read_flash 0x0 0x1000000 stock_backup_2.bin
```

**Windows (PowerShell or Command Prompt):** run these commands **one at a time**.
Continue to the second only after the first succeeds. Replace both occurrences of `<PORT>`
with the same port, for example `COM5`.

```sh
esptool.py --chip esp32c3 -p <PORT> -b 921600 --no-stub --after no_reset read_flash 0x0 0x1000000 stock_backup_1.bin
esptool.py --chip esp32c3 -p <PORT> -b 921600 --no-stub --before no_reset read_flash 0x0 0x1000000 stock_backup_2.bin
```

#### Verify: the two hashes must be identical

Compare both files to check that the reads produced identical data.

- **macOS**: `shasum -a 256 stock_backup_1.bin stock_backup_2.bin`
- **Linux**: `sha256sum stock_backup_1.bin stock_backup_2.bin`
- **Windows (PowerShell)**: `Get-FileHash stock_backup_1.bin, stock_backup_2.bin -Algorithm SHA256`
- **Windows (Command Prompt)**: `certutil -hashfile stock_backup_1.bin SHA256` (repeat for file 2)

Only trust the backup once both hashes match.

The `--after no_reset` / `--before no_reset` flags above are what make that possible. If esptool
is allowed to reset the chip between the two reads (its default), the frame boots, wakes, and
repaints — which rewrites its photo cache (the `framestore` partition) and can touch saved
settings. Those changed bytes can make the dumps differ even when both reads succeeded.
Keeping the chip in the ROM loader across both reads stops the firmware from running in between, so
the bytes stay put.

> **esptool v4 vs v5:** the underscore spelling above (`read_flash`, `no_reset`, `esptool.py`) works
> on **both** v4 and v5. On v5 it just prints a harmless deprecation warning. v5's own native
> spelling uses hyphens (`read-flash`, `--after no-reset`, and the `esptool` command without `.py`),
> but that hyphen form does **not** work on v4, so the underscores shown here are the safe choice if
> you're not sure which version you have.

**Why 16 MB when the chip says 32?** On the tested unit, the flash identifies itself as 32 MB,
but two full dumps showed the upper half reading back as a **mirror of the lower half**.
Its stock partitions all sit below 16 MiB, and restoring the lower-16 MiB backup was verified
to boot stock. This firmware and its installer use only that lower 16 MiB. Never *write* anything above the 16 MB boundary,
because on this chip such writes can wrap around and corrupt the bootloader.

**Why `--no-stub`?** esptool's default *stub* flasher is unreliable for large transfers on
this flash chip (it can report success while the data is actually corrupt), so the backup
must go through the ROM loader instead. The ROM loader moves data in small acknowledged
chunks, so **expect a long wait of roughly 1.5 to 2 hours per read**; a higher baud rate won't
help (this board's native USB ignores it). Keep the frame plugged in and prevent your
computer from sleeping.

Keep one file and its checksum somewhere safe. The backup is
**per-device**: the stock settings region at `0x9000` holds factory data unique to your unit
(including the stock device identity), and a fresh install clears it, so
someone else's backup or a shared stock image cannot fully restore your frame.

### Step 2: Flash the release build

> **Easiest: flash from the browser at <https://picpaktesserae.pages.dev>** (Chrome or Edge).
> Fresh install or a settings-keeping update, plus a read-only serial monitor, with no tools to
> install. (Flashing only; the browser can't make the Step 1 backup.) The esptool commands
> below do the same flashing from the command line.

Version 0.9.9 and later use the layout below. Use a browser installer that supports this layout
to migrate an older device. Update preserves settings and pairing. Fresh install
clears settings and opens setup. The firmware package contains individual images,
`manifest.json` and `SHA256SUMS`. Use all files from the **same release**, and run the commands
from the extracted package folder. Do not use an older combined image for this migration.
The browser may offer a different release from the source checkout, so check its displayed version.

Before a manual flash, compare the downloaded files with the release's `SHA256SUMS`.
On macOS run `shasum -a 256 -c SHA256SUMS`, or on Linux run `sha256sum -c SHA256SUMS`.
On Windows, use `Get-FileHash *.bin -Algorithm SHA256` in PowerShell and compare each hash
with its entry in `SHA256SUMS`. Stop if any file is missing or a checksum differs.

| File | Flash offset | Purpose |
| --- | --- | --- |
| `bootloader.bin` | `0x0` | Bootloader with future OTA rollback support |
| `partition-table.bin` | `0x8000` | New layout |
| `nvs_blank.bin` | `0x9000` | Fresh install only. Omit when updating |
| `ota_data_initial.bin` | `0x10000` | Select slot A after installation |
| `picpak-tesserae-client.bin` | `0x20000` | Application in slot A |
| `ota_1_blank.bin` | `0x420000` | Clear the inactive slot header so stale firmware cannot boot |

The layout reserves two 4 MiB application slots, 64 KiB for future crash diagnostics,
and 7.8125 MiB for future photo storage. NVS remains at `0x9000`, size `0x6000`.
OTA downloads and offline photo storage are not implemented yet. Firmware images must fit
within one 4 MiB slot. All partitions remain below 16 MiB.

The migration needs the bootloader, partition table, application and boot selection files
together. An application written at the old `0x10000` address will not work with this layout.
The browser installer writes and verifies the application first, then updates the boot layout,
clears the inactive slot header and resets boot selection. The command-line examples below
write the same files, but do not provide that application-first ordering. Let the complete
command finish before disconnecting. If interrupted, repeat the full command.

After a successful installation, these release files select slot A, including when a device
previously ran from slot B. The reserved photo area is not written by these commands, but its
location changed in 0.9.9. Storage from an older layout is not migrated.

**Finding the port:** the C3's native USB-Serial-JTAG shows up as `/dev/cu.usbmodem*` on macOS
(`/dev/ttyACM*` on Linux; on Windows as "USB Serial Device (COMx)" under Device Manager →
Ports (COM & LPT), use `COM<x>`). List it with `ls /dev/cu.usbmodem*`. The number encodes the
USB port/hub position, so it **changes when you replug into a different port**; re-check it
rather than assuming last time's name.

Update an existing PicPak custom firmware installation, keeping saved settings
(replace `<PORT>` with the actual serial port):

```sh
esptool.py --chip esp32c3 -p <PORT> -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 ota_data_initial.bin 0x20000 picpak-tesserae-client.bin 0x420000 ota_1_blank.bin
```

For a **fresh install**, including the first installation from stock, use the command below.
It clears saved settings and pairing. Back up the stock firmware first.

```sh
esptool.py --chip esp32c3 -p <PORT> -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x9000 nvs_blank.bin 0x10000 ota_data_initial.bin 0x20000 picpak-tesserae-client.bin 0x420000 ota_1_blank.bin
```

Neither command requires a full chip erase. **Downgrading to a release before 0.9.9** needs
that release's complete matching bootloader, partition table and application at its original
addresses. Use its installation instructions, not the commands above. Flashing only an old
application at `0x10000` corrupts the current layout. Saved settings may also be incompatible
with an older release, so preserving NVS does not guarantee a working downgrade.

If the connection drops or fails to sync, use a different USB-C **data** cable and a direct
port (no hub); the baud rate doesn't matter on this board's native USB. *"The port is busy or
doesn't exist"* means something else holds the port, usually an open serial monitor; close it
and re-run.

The manufacturer's update tools do not support this custom firmware. Restore your own stock
backup to return to the manufacturer's firmware and partition layout.

### Step 3: First boot and the setup portal

WiFi + server come from an on-device **SoftAP captive portal** (`firmware/main/provisioning.c`), with no
recompiling. Public builds contain no preset WiFi, REST or MQTT credentials and are built
without `secrets.h`. Setup opens automatically when no WiFi network is saved, or on demand
via the ~20 s button hold below. The portal also lets you pick a
**refresh speed**: **Native (default, recommended)**, 5 s, or 10 s. A fresh installation with
no saved refresh setting uses Native for the first setup screen and preselects Native in the
portal. You can choose 5 s or 10 s for faster updates; colour and refresh compatibility vary by
panel. The choice is saved and can be changed any time. Firmware updates that preserve settings
keep the previously selected refresh speed.

In Automatic mode, the frame reads its **single button** during boot after waking.
Deck navigation and refresh requests work over REST and cloud relay. MQTT does not dispatch
these server actions. Gestures do not interrupt an ongoing paint or network request:

- **Quick tap, released before the boot gesture check** → wakes and checks for the current image.
- **Brief hold, released before the 5 s LED cue** → sends a next-page request (`right`).
  Hold for roughly a second to make it deliberate. There is no fixed 0.5 s cutoff:
  if the button is still held when the firmware checks it, even a short press can navigate.
  Tesserae decides the action from the device's deck or lineup configuration.
- **Hold until the ~5 s steady LED, release before ~10 s** → requests a refresh of the current page.
  In Automatic mode this also overrides the low-battery lock. Server behavior and, for relay,
  delivery timing determine when the refreshed image becomes available.
- **Hold ~10 s (10–20 s)** → **Bluetooth maintenance** in Tesserae Companion.
  Read diagnostics, repair Wi-Fi, or change refresh speed without a reachable server.
  A QR code and passkey authorize a five-minute session. See [Bluetooth maintenance](docs/ble-maintenance.md).
- **Hold ~20 s** → reopens the setup portal (deliberate re-provision of WiFi/server).
- **Status LED** (`firmware/main/led.{c,h}`, GPIO21). Screen-free status: **one blink on every wake**
  (timer or button); while holding the button the feedback steps off → **steady-on** past the ~5 s
  refresh point → **pulsing** at ~10 s for Bluetooth → **rapid burst** at the ~20 s provisioning
  point, so you can feel the timing without
  watching the panel. The console runs on USB-Serial-JTAG (`/dev/cu.usbmodem*`), so GPIO21, which is
  also the UART0 TX pin, carries no log traffic and is a clean, dedicated LED.

Each frame shows its setup network name on the display. `XXXX` is the last four
hexadecimal characters of its WiFi station MAC address, for example `27CC`.
The name stays the same after restarting or clearing settings. The suffix distinguishes most
nearby frames, but two devices can still share the same last four MAC characters. Scan the QR code on
the left with your phone to join this network, or enter the displayed name and
password manually. If the setup page does not open, visit `http://192.168.4.1`
after joining. The QR contains only the setup network details.

The AP is **`Tesserae-Setup-XXXX`** (password `tesserae`, IP `192.168.4.1`); a DNS-hijack pops the captive
sheet on your phone. Fill in:

- **WiFi** network + password. A blank password keeps the saved password only when the
  network name is unchanged. For a different open network, leave the password blank.
- **Server URL**: the Tesserae server, either its LAN IP or `<host>.local:8765` (the C3 resolves
  `.local` via mDNS on the same network). For HTTPS, use the hostname covered by the
  certificate, with DNS that resolves from the frame's network. A private LAN address behind
  that hostname works, and the server does not need to be publicly reachable. The certificate
  must chain to the firmware's trusted CA bundle, such as Let's Encrypt. Self-signed
  certificates are not supported.
- **Device id** *(optional)*: a custom name (`picpak-1`, validated `^[a-z][a-z0-9_-]{1,31}$`); blank
  auto-derives `picpak-<mac>`. This is the id the device claims and shows in Tesserae.
- **Pairing code** *(optional)*: a 6-digit code from Tesserae's **Pair new device** to self-claim
  without an admin click; blank uses the discovery flow (admin clicks **Register**).

Save, and the frame reboots and checks its connection once. If WiFi fails, or the selected
Tesserae server, MQTT broker or cloud relay connection cannot be confirmed, the setup screen
and portal reopen with a message explaining what to check. A waiting splash can appear while
the connection attempt runs. A device waiting to be claimed, relay pairing still pending,
or a connection with no image yet does not count as a failure. REST token renewal
continues on the next wake without reopening setup. Cloud relay keeps its existing
revocation check across two wakes.

This automatic check runs only after saving setup. A service outage during that initial
attempt can also reopen the portal. Later WiFi or service outages keep the saved settings
and use the normal sleep and retry cycle. If the portal times out without another save,
the frame also returns to normal retries. Hold the button for about 20 seconds to reopen
setup manually. MQTT checks broker acceptance, and cloud relay checks the relay connection.
Neither can confirm that the Tesserae server behind it is online.

The portal shuts down after **10 minutes with no phone connected**. This idle timer pauses
while a phone remains associated. After a timeout the frame sleeps for 15 minutes. If no
WiFi network is saved, the next wake opens setup again. If settings are already saved, the
next wake retries them instead. A 20 second hold opens setup manually. The e-paper setup
screen remains visible while the setup WiFi is off. With saved settings, the next
successful network cycle restores the server image even if that image has not changed.

For HTTPS behind NGINX, Caddy or another reverse proxy, check Tesserae's **Public URL**
if downloads fail after a successful connection. Set it to the HTTPS address reachable by
the frame, including a nonstandard port if needed, without a trailing slash. The image URL
must return the image directly. Image downloads do not follow redirects. Configure
the final HTTP or HTTPS server address: API redirects are accepted only within the
same scheme, hostname and port, so authentication cannot be forwarded elsewhere.
Normal certificate renewal does not require re-pairing, provided the hostname and trusted
certificate chain remain valid.

On the LAN the frame advertises its DHCP hostname as its **device id** (e.g. `picpak-red`; the
router's client list matches Tesserae's device list; `_` becomes `-`). An unnamed frame advertises
`tesserae-picpak-<mac>` (last three MAC bytes) so multiple PicPaks stay distinguishable.

## Going back to stock

### Restoring the stock firmware

Flash your `stock_backup_1.bin` back at offset `0x0`, but **not** as one monolithic stub-mode
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
holds stock settings and per-device identity, and erasing it destroys
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
| Cloud relay | **working**: for a panel that can't reach your Tesserae server directly (another home, CGNAT, a hotspot). Mutually exclusive with REST/MQTT: when a relay is paired the device talks only to the relay. See below. |

The initial sleep interval is **15 minutes**. Tesserae can change it after connection,
with supported relative intervals from 30 seconds to 7 days. This schedule is separate from
the panel's Native, 5 s or 10 s refresh speed.

Connection failures are counted once per wake across WiFi and the selected transport.
The first two failed wakes keep the normal retry interval. From the third consecutive
failure, retries sleep for at least 15 minutes, preserving a longer saved interval.
A successful service connection clears the count and resumes normal scheduling.
An unchanged image or an empty mailbox is still a successful connection. Button wake
remains available during the longer sleep. The counter lives in retained RAM, does not
write to flash, and resets on a restart or power loss.

This detects loss of WiFi, the REST service, the MQTT broker or the cloud relay.
A reachable broker or relay cannot tell the frame whether Tesserae itself is offline.
Initial setup validation, pairing recovery and low battery protection keep their existing
behaviour. Connection attempts take additional time before the sleep interval starts.

### Cloud relay (remote panels)

A relay panel and your home Tesserae instance both connect **outbound** to a small mailbox Worker;
home seals each rendered frame and `PUT`s it, the panel polls and decrypts it, so the home server is
never exposed to the internet. The relay is **zero-knowledge**: at pairing the two ends exchange
X25519 **public** keys through it and each derive the same AES-256-GCM frame key locally (the key is
never transmitted). Frame content is encrypted, while the relay still sees routing information
and request timing. A frame with a failed authentication tag is never painted. Frame crypto is in `firmware/main/relay_crypto.c` (X25519
via vendored Monocypher, HKDF-SHA256 + AES-GCM via mbedTLS), covered by the relay contract's golden
vectors in `firmware/test/test_relay_crypto.c`.

**Provisioning:** in the setup portal pick **Cloud relay**, enter the relay URL (defaults to the
hosted `https://relay.tesserae.ink`) and a single-use pairing code from your Tesserae server
(*Settings → Cloud relay → Add a remote panel*). No server URL is needed. Pairing may take a couple of
wakes to complete. While pending, the frame checks once per wake using its saved sleep interval
(15 minutes if unset). A quick button wake checks sooner. After pairing, each wake fetches the
current frame, posts telemetry and reads the server-set sleep interval. A `204` response means
there is no frame in the mailbox yet.

**Removing or re-pairing:** to retire a relay panel, use the per-device **Remove** button on
*Settings → Cloud relay* (it revokes the relay mailbox and deletes the device). To move a panel to a
fresh pairing (or to recover a deleted/re-added one), reopen setup (20 s button hold) and enter a
new pairing code; the panel drops its old pairing and re-pairs cleanly (no factory reset needed). The
device-id field is hidden on relay, since a relay panel's identity comes from pairing, not that field.

**Button gestures work over relay on Tesserae server/relay v0.240.0+.** The press rides the status
body (`button` + `button_event_id`). A brief hold sends `right`, and a 5 s hold sends `refresh`.
After a press, while pairing is valid, the panel opens a 45 second polling window,
even if the initial fetch downloaded a refreshed copy of the current page. It waits
5 seconds before each check and stops when a different frame arrives. A frame already
downloaded remains available for painting if later polls fail or the window expires. Request time adds to the spacing, and the final request can finish
after the window ends. WiFi then turns off before painting. On an older server the button
fields are ignored (harmless). MQTT does not dispatch button actions. *(A revoked panel also
auto-recovers on v0.240.0+: two consecutive `401` wakes → the panel drops its pairing, paints an
**"Unpaired"** screen, and re-pairs on a fresh code via the 20 s-hold portal, URL pre-filled.)*

Relay requests reuse one connection during the wake, including button polling, when
the relay permits it. A stale connection can retry a GET once. Status and pairing
POSTs are not automatically replayed within a request. Interrupted pairing checks for
completion on the next attempt, then resubmits the same saved key if still pending.
The connection closes before WiFi turns off.
Certificate verification and encrypted frame authentication remain enabled.

### Frame format

Raw, headerless, exactly **30 000 bytes** (`400 × 300 ÷ 4`), **2 bits per pixel**, 4 pixels per byte,
**MSB-first** (leftmost pixel in bits 7:6). Palette indices: `0`=Black, `1`=White, `2`=Yellow, `3`=Red.
Rows are packed **bottom-to-top** (the panel scans that way; the renderer flips vertically before
packing, otherwise the image paints upside-down).

The heartbeat reports `kind: "picpak_client"` and `panel_w: 400, panel_h: 300`. The matching
renderer and `picpak_client` device kind ship **built into the Tesserae server**.

### Heartbeat schema

Sent during a connected wake on REST and MQTT: REST `POST`s it to `/api/v1/device/<id>/status`, MQTT
publishes it retained (QoS 1) to `tesserae/<id>/status`. After the frame fetch attempt, before any
paint (the radio is turned off for the panel refresh, so on repaint wakes the server's "last seen"
precedes the paint by the selected refresh duration):

```json
{
  "battery_mv": 4164,
  "battery_pct": 96,
  "rssi": -63,
  "ip": "10.0.20.40",
  "fw_version": "0.9.10",
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
wake next, estimated before the status response and paint. It is sent only with a plausible clock.
The server cross checks it against `next_sleep_s` and replaces that estimate when issuing `wake_at`.

### REST contract

REST uses the following API paths over HTTP, or HTTPS when the server URL uses it
(validated against ESP-IDF's built-in CA bundle; publicly-trusted certificates only):

| Method + path | Purpose |
| --- | --- |
| `POST /api/v1/device/discover` | Unauthenticated. Posts identity (`device_id`, `kind`, `panel_w/h`, `fw_version`, `mac`). `device_id` is the portal's custom **Device id**, or `picpak-<mac3>` when left blank. `registered:false` → deep-sleep + retry next wake; `registered:true` → returns `device_token`. Default first-boot path. |
| `POST /api/v1/device/register` | Opt-in: same body + `X-Pairing-Code` header (from the portal's optional Pairing-code field). Returns `device_token` (server auto-claims, no admin click). `403` clears the code and backs off; the code is single-use and burned on success. |
| `GET /api/v1/device/<id>/frame` | `Authorization: Bearer <device_token>` + optional `If-None-Match`. `200` → `{url, format, panel_w, panel_h}` + `ETag` (fetch `url`, paint); `304` → skip; `204` → not rendered yet. |
| `POST /api/v1/device/<id>/status` | `Bearer` auth. Body = the heartbeat JSON. Response `{config, next_poll_s, server_time, wake_at?}`. Firmware saves `config.sleep_interval_s`, updates the clock, and uses a valid `wake_at` for this sleep. `next_poll_s` remains the relative fallback. |
| `POST /api/v1/device/<id>/log` | `Bearer` auth. Uploads a bounded plain text log when the status response contains `logs: {upload: true}`. |

The `device_token` is persisted to NVS on first pairing; later wakes go straight to the frame GET. A
`401` or `403` on a frame or status request wipes the token and forces re-pairing. A relative `url`
in the frame response is resolved against the server origin.

REST reuses connections within a wake when the scheme, host and port match and the
server permits persistent connections. Image requests do not carry the device token
or pairing headers. A closed connection can retry a GET once, while POST requests
are not automatically replayed. All connections close before painting and sleeping.

### Device logs over REST

On servers with device log collection, the device page offers **Logs** after the next REST
status check. Choose 1, 5 or 20 wakes to collect logs. Collection works over HTTP and HTTPS
and starts at the next normal wake or button wake. It does not wake a sleeping frame remotely.
MQTT and cloud relay do not upload logs in this version.

The firmware keeps a 3 KiB tail of application logs in retained RAM, with no flash writes.
An upload can include the unsent tail of the previous wake and the current wake. This helps
capture painting and sleep messages, which happen after the network connection closes.
Old entries are overwritten as the buffer fills, and removing power clears the buffer.
These are recent diagnostic messages, not a complete serial recording or crash backtrace.

REST status adds `logs: {schema: 1, ring_bytes: 3072}` and, when needed, a `diag` report for
display failures, panics, watchdog resets or brownouts. Reports remain pending until a status
request succeeds. A painting failure is normally reported on the next connected wake.
Tesserae can request logs automatically after a failure when that server setting is enabled.
Servers without this feature continue using the normal frame and status exchange.

Each requested upload adds one POST with a 10 second network timeout and no automatic retry
or redirects. A failed upload preserves pairing, leaves retained log bytes eligible for a
later request, and keeps the normal sleep schedule. Collection temporarily adds network time
and memory use, so stop collection when troubleshooting is finished.

Captured messages redact bearer tokens, known credential fields, URL credentials and URL
queries. Network names, addresses and other diagnostic details can remain. Review downloaded
logs before sharing them. Tesserae stores accepted uploads on the server and controls retention.

### REST wake timing

When Tesserae sends `wake_at`, the frame calculates the remaining time just before sleep,
after painting and waiting for button release. For example, a target 900 seconds away followed
by a 20 second paint leaves about 880 seconds to sleep. The target applies to this wake only.

Enable **Synchronized wake** on the device page in Tesserae to use clock alignment. The server
also supplies absolute targets when sleeping through quiet hours. Without a target the frame
uses `next_poll_s`, or the saved interval when no valid status response arrives. Missing,
invalid, expired, or implausibly late targets use that same fallback. Targets less than
30 seconds away also use the fallback to preserve the minimum sleep interval.

REST responses supply the clock through HTTP `Date` and JSON `server_time`. Successive normal
timer wakes estimate clock drift, smooth it, and adjust the sleep timer by at most 6 percent.
Drift correction can still apply to relative sleep when Synchronized wake is off.
The estimate stays in RTC memory during deep sleep and clears after a reset. Button wakes do
not train it. Very short samples and large clock jumps are excluded. No extra requests or
flash writes are needed. Network latency and timestamps rounded to seconds still limit precision.

This affects normal REST sleep only. MQTT, cloud relay, manual Bluetooth photos, low battery
checks, brownout recovery and setup retries keep their existing sleep behavior. Alignment targets
the wake time. Connecting and painting still take time after waking, and Tesserae may request an
earlier wake to account for that.

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
HTTP(S) and validate exactly 30 000 bytes → publish the heartbeat → disconnect gracefully → radio
off → paint.

There is no pairing token on MQTT: the retained heartbeat is what makes an unclaimed frame appear
under Tesserae's **Settings → Devices** for the admin to claim. The session carries a
**non-retained** last-will (`{"state":"offline"}`) so an ungraceful drop is visible to live
subscribers without ever clobbering the retained heartbeat. Wakes that find the broker down keep
the last image and retry on the next cycle; the frame never blanks.

### Manual Bluetooth photos

In compatible Tesserae Companion, enter Bluetooth maintenance and select **Manual (Bluetooth)**
as the Screen Mode. This authorizes the phone to send photos locally. Close maintenance,
then press the frame's button with Companion open to send a photo. No Tesserae server or
WiFi connection is needed for this mode.

Bluetooth is active only during a requested session. Between sessions the frame sleeps,
with a silent daily battery check. Return to **Automatic (Wi-Fi)** in maintenance to resume
server updates using the saved connection settings. See [Bluetooth maintenance](docs/ble-maintenance.md)
and [manual photo operation](docs/ble-photo.md) for session limits and app compatibility.

### Panel screens

Four splash backgrounds are embedded as 2 bpp assets generated by `tools/gen_splash.py`.
The setup screen adds its device-specific SSID and WiFi QR at runtime on a white background.
The QR is not a separate prebuilt binary.

- **Setup**: WiFi join QR + `Tesserae-Setup-XXXX` / `tesserae`, shown when the portal comes up so you can join.
- **Paired**: "Connected, waiting for first frame", shown once after you submit, until the first photo.
- **Battery low**: "please charge", shown by the low-battery gate.
- **Unpaired**: shown when a relay panel's pairing is revoked (see [Cloud relay](#cloud-relay-remote-panels)), until you re-pair with a fresh code.

### Power notes

- **Brownout mitigation.** PicPak's power delivery is marginal: at default TX power the WiFi current
  spike browns the board out on radio-on. Fixed with a low-power profile: cap PHY TX power to 10 dBm
  (`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10`, which caps the PHY-cal spike), CPU @ 80 MHz, trimmed WiFi buffers,
  AMPDU off, plus a brownout-aware defer (on a power-fault reset, deep-sleep to let the cell recover).
- **Low-battery gate** (`firmware/main/lowbatt*`). Two consecutive readings below 3400 mV
  trigger the charge screen and a 24-hour battery check. Normal operation resumes at 3550 mV
  or after a sufficient voltage rise indicates charging. The lock lives in RTC RAM and stays
  active if a battery reading fails. A small saved flag remembers the charge screen across
  restarts so recovery restores the server picture or the Bluetooth ready screen. Failed
  charge or ready screen updates are retried on a later wake, once per wake. Successful
  charge screens are not repainted on ordinary battery checks. In Automatic mode a deliberate
  5 second refresh hold overrides the lock. An ordinary tap only checks whether recovery
  conditions are met. Manual Bluetooth mode keeps its low battery protection. Connecting a
  charger does not itself trigger a wake. A button press can check recovery before the daily timer.
- **Battery reading** uses a batch of ADC samples early in the wake, before WiFi or the panel
  loads the supply. Its median is cached for telemetry. The percentage is an estimate from
  voltage, not a fuel-gauge measurement.
- **Radio off during the panel refresh.** All network I/O (frame fetch and status, on all three
  transports) finishes first, then WiFi stops, then the panel paints, so the radio never idles
  throughout the selected refresh or overlaps its load with the panel. In MQTT mode the broker
  session closes gracefully first, so normal shutdown does not trigger its last-will.
- **Lazy panel init.** The EPD is powered and initialized only when a new frame actually arrived;
  on the common "unchanged" wake (REST or relay `304`, or an MQTT retained-URL match) the panel is never touched.
- **Panel put fully to sleep after a paint** (`firmware/main/epd_driver.c`). `epd_sleep()` sends the
  Deep Sleep command *with* its required `0xA5` check-code (a bare opcode is ignored by this
  controller, leaving the panel in standby); the next wake's reset brings it back before painting.
  This lowers the panel's own standby draw between wakes.
- **Single broker session per wake (MQTT).** Fetch, config, and heartbeat share one connection:
  no second connect and no post-paint WiFi reconnect. A failed
  broker connect tears down in ~0.5 s instead of esp-mqtt's default multi-second reconnect wait.
- **Clock sync only when needed.** The C3 keeps RTC time across deep sleep. Plain HTTP REST
  gets time from server responses. MQTT, HTTPS REST and HTTPS relay attempt NTP when the clock
  is implausible, normally after power loss. A failed sync can be retried on a later wake.
- **Connection reuse.** REST and relay reuse compatible HTTP(S) connections within a wake
  when the server allows it, reducing repeated TLS handshakes. Firmware uses performance
  optimization (`-O2`) while keeping the CPU at 80 MHz.
- **WiFi fast-connect.** After the first association the AP's BSSID + primary channel are cached in
  NVS and reused on the next wake, so the radio skips the ~1 to 2 s all-channel scan (less radio-on
  time, a smaller brownout-sag window across all three transports). A stale hint (AP moved channel / router
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
│       ├── epd_driver.{c,h} · epd_init_seq.h   # 400×300 BWRY UC81xx panel driver + init sequence
│       ├── fb2bpp.{c,h}          # 2 bpp framebuffer packer (+ host test)
│       ├── framebuf.{c,h}        # 30 KB frame staging buffer shared by all transports
│       ├── power.{c,h}           # battery ADC, deep sleep, boot-button gesture
│       ├── wake_align.{c,h}      # REST absolute wake targets and RTC drift correction
│       ├── battpct.h             # pure mV→% Li-Po curve (host-tested)
│       ├── lowbatt.{c,h} · lowbatt_core.h      # low-battery gate (pure FSM + RTC glue)
│       ├── led.{c,h}             # status LED (GPIO21) blink / hold-progression feedback
│       ├── config_store.{c,h}    # NVS config (creds, token, etag, broker, sleep)
│       ├── wifi_manager.{c,h}    # STA connect, fast reconnect and clock sync
│       ├── provisioning.{c,h} · provision_form.{c,h}   # SoftAP captive portal + pure form parser
│       ├── splash.{c,h} · assets/splash_*.bin  # embedded setup / paired / low-batt / revoked screens
│       ├── setup_identity.{c,h} · setup_screen.{c,h} · setup_font.h  # setup SSID, QR and text
│       ├── ble_setup.{c,h} · maintenance_screen.{c,h}  # Bluetooth maintenance
│       ├── ble_photo.{c,h}       # local photo sessions
│       ├── image_fetcher.{c,h}   # MQTT image download over HTTP(S)
│       ├── rest_handler.{c,h} · rest_button.h  # REST: discover/register + frame GET + status POST (+ button/deck dispatch)
│       ├── mqtt_handler.{c,h}    # MQTT: retained frame/config read + heartbeat publish, one session
│       ├── mqtt_parse.{c,h}      # pure payload/URI helpers (+ host test)
│       ├── relay.{c,h} · relay_crypto.{c,h} · relay_wire.{c,h}   # cloud-relay transport (X25519 + AES-GCM remote panels)
│       ├── vendor/              # crypto, QR generator and font with their notices
│       └── heartbeat.{c,h}       # battery / RSSI / IP / panel JSON
├── docs/                         # Bluetooth operation and protocol details
├── tools/                        # release packaging, asset generation and host checks
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
The log ring and failure-report helpers include AGPL-licensed components from
[Tesserae device firmware](https://github.com/dmellok/tesserae-device-firmware).
The panel init sequence and 4-colour packing were reverse-engineered for the PicPak's specific 400×300
BWRY hardware.

## License

AGPL-3.0-or-later. © 2026 [varanu5](https://github.com/varanu5). See [LICENSE](https://github.com/varanu5/picpak-tesserae-client/blob/main/LICENSE).
