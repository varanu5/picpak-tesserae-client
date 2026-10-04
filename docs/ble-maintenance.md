# PicPak Bluetooth maintenance

Development implementation, compatible with Tesserae Companion BLE protocol v2.
The GATT transport and QR crypto are adapted from
[Tesserae device firmware](https://github.com/dmellok/tesserae-device-firmware),
under AGPL-3.0-or-later. The vendored QR generator retains its MIT license;
the bitmap font is public domain.

## Entering maintenance

In Automatic mode, press and hold the wake button while the display is sleeping.
At about five seconds the LED lights steady for refresh. At about ten seconds it
pulses. Release between ten and twenty seconds for Bluetooth maintenance.
A press still held when the boot gesture check runs and released before five
seconds requests the next page. A tap released before that check only wakes the
device. Release between five and ten seconds for refresh, or keep holding to
twenty seconds for AP setup. Shorter actions are selected on release.
Gestures are read at boot or wake and do not interrupt a running refresh or portal.
A fresh device uses the AP for initial server setup. Manual mode has different
short-press actions, described under [Screen mode](#screen-mode).

The display paints a QR code and passkey before advertising. Open Bluetooth
Maintenance in Companion, select the nearby PicPak, then scan the QR code or use
the six-digit passkey. The five-minute deadline starts after screen rendering.
Closing the sheet disconnects the phone. In Automatic mode, an unchanged
maintenance session can be rejoined until its deadline. After changing Screen Mode,
or after authenticated maintenance in Manual mode, disconnecting ends maintenance
and applies the selected mode. Deep sleep never advertises. Low-battery protection still
applies to entry.

To leave without a phone, briefly press and release the wake button after the
QR screen is ready. The entry hold must first be released; a fresh press shorter
than three seconds exits on release. Long holds have no action inside maintenance.
The radio stops, the expired QR is cleared, and the display restarts its normal
operation (network cycle or button-only sleep). Saved settings, including refresh speed, are retained; pending
commands and staged but unsaved credentials are discarded. The five-minute
deadline remains the fallback, even if a button is held or bouncing.

Basic diagnostics and refresh-speed changes do not start Wi-Fi. Network scanning
and repair are explicit actions. Wi-Fi repair tests association and DHCP, then
saves only Wi-Fi credentials; it does not contact Tesserae or replace REST,
MQTT, relay, or server-registration settings. Clearing Wi-Fi opens one recovery
session after restarting, then falls back to AP setup if it times out. Factory
Reset clears stored application settings, including photo authorization, and returns to AP setup. Neither reset
operation reflashes the firmware.

The normal image deduplication references are cleared when painting the QR so
the next successful network cycle restores the photo on every transport.
An expired session clears its QR from the screen before normal operation resumes.

## Refresh speed

The Display section exposes `Refresh Speed`: `5 s`, `10 s`, `Native`. These map
to the existing NVS `state/waveform` values 0, 1, 2 without changing the panel's
drivers or waveforms. They control redraw duration, not the schedule between
updates. Faster modes trade colour margin for speed; Native uses the panel's
built-in waveform. Approximate duration depends on the panel and conditions.
Saving does not force an extra refresh; it applies at the next `epd_init()`.

## Screen mode

Updated Companion shows **Screen Mode** when diagnostics reports `screen_mode`.
Choose **Automatic (Wi-Fi)** for normal server updates or **Manual (Bluetooth)**
for button-triggered local photo reception. Manual mode works without a saved
SSID or reachable server. Close Maintenance, wait for the closing screen to
finish, then press the button once with Companion open to send a photo.

The existing refresh waveform is used for photos. Hold for 10 seconds and release
before 20 seconds to open maintenance; a 5-second hold in Manual mode also opens
photo reception. Completing explicit 20-second AP provisioning returns the display
to Automatic mode.
See [BLE photo protocol and validation](ble-photo.md) for authorization, timing,
compatibility, and the complete wire contract.

## Wire contract

- Protocol major: **2**; hardware code: **11**; model: **`picpak_4_2`**.
- Service UUID: `7A5E0001-7B6D-4F8B-9C2E-1D0A5A110001`.
- Info / QR control / passkey control / events use UUID suffixes 0002–0005.
- Advertisement is the existing v2 ten-byte service payload: version, mode
  (`0x02` maintenance), hardware, last three MAC bytes, four-byte session ID.
- Device ID is `picpak-` plus the Wi-Fi MAC, independent of server registration.
- QR: `tesserae://setup?v=2&id=<id>&sid=<8-hex>&key=<base64url-32-byte-secret>`.
- Each connection supplies a new 16-byte `connection_nonce` in the info JSON.
  QR frames use HKDF-SHA256 and AES-256-GCM with the existing v2 domain, nonce,
  directional counters, and chunk headers. Native passkey writes require an
  authenticated encrypted LE Secure Connections link; legacy pairing is disabled.
- Messages are limited to 512 bytes. Events and commands preserve FIFO order.
  Both queues are scoped to the originating connection generation; native
  event notifications and reads require the authenticated encrypted link.

Authenticated commands:

| Command | Result |
| --- | --- |
| `{"op":"diagnostics"}` | Existing diagnostics event plus `"refresh_speed":"5s"` (or `10s`, `native`) |
| `{"op":"set_refresh_speed","value":"10s"}` | `{"event":"refresh_speed","value":"10s"}` after successful NVS commit |
| `{"op":"scan"}` | `scan_started`, zero or more `network` events, `scan_complete` |
| `{"op":"stage","ssid":"…","password":"…","preserve_server":true}` | `staged`; credentials stay in RAM |
| `{"op":"apply"}` | `testing_wifi`, `wifi_connected`, then `configured` and restart |
| `{"op":"reboot"}` | `rebooting` and restart |
| `{"op":"clear_wifi"}` | `clearing_wifi` and restart into BLE recovery |
| `{"op":"factory_reset"}` | `factory_resetting` and restart into AP setup |

Unknown speeds and failed writes return `error` with a message, never a success
acknowledgement. Firmware serializes writes and following diagnostic readbacks.
Companion shows the setting only for PicPak when diagnostics includes a known
speed, retains the confirmed value while saving, and reads back after timeout.
Stage accepts omitted or empty `server_url`/`pairing_code` for compatibility;
nonempty values are rejected. Changing servers remains in AP setup.

## Building and checking

Firmware 0.9.9 and later use two 4 MiB application slots within the lower 16 MiB of flash.
Migrating from an older layout requires the complete release package, including its bootloader
and partition table. An application-only flash at the old address is not supported.
See the [installation instructions](../README.md#step-2-flash-the-release-build).

Run `tools/test_host.sh` for gesture boundaries, settings persistence/error paths,
protocol golden vectors, QR framebuffer bounds, and existing pure firmware tests.
`IDF_PATH` may point to ESP-IDF for the host mbedTLS dependencies;
`PICPAK_TEST_SCREEN` optionally exports the exact test QR framebuffer.

Before distributing: validate QR and passkey connections, reconnects, all three
speed settings after power cycling, Wi-Fi failure with credentials preserved,
low-battery entry, short-press exit (including while scanning or joining Wi-Fi),
five-minute timeout and sleep current on a physical PicPak.
Host and simulator tests do not establish those hardware results.
