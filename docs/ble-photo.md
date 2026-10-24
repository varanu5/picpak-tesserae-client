# PicPak manual Bluetooth photos (v1)

This extends Tesserae BLE v2; it is not an OpenDisplay implementation. The scope
is the 400×300 PicPak and the matching Tesserae Companion build. No server change
is required to send a local photo.

## Operation

- Default: Automatic (Wi-Fi); existing REST/MQTT/relay settings remain unchanged.
- Select Manual (Bluetooth) in authenticated Maintenance. Its acknowledgement
  also authorizes this iPhone. Close the sheet; the QR is replaced by a Bluetooth
  mode guide: open Tesserae, press the button once, then tap Send to choose a photo.
  The display restarts into deep sleep, woken by the button or a silent 24-hour
  low-power battery check. No Wi-Fi, network heartbeat, or periodic BLE
  advertisement runs in this mode; the 24-hour wake reads the cell and sleeps
  again without bringing up any radio.
- A short button wake opens a connectable photo advertisement. A tap released
  before the boot gesture reader runs still works via the GPIO wake cause.
- No connection/authenticated command: 90 seconds. After an authenticated command:
  5 minutes idle, with a 10-minute absolute session limit. Advertising interval
  is 100–150 ms during this bounded window. BLE is off between sessions.
- Hold for 10 seconds and release before 20 seconds to enter Maintenance.
  A 5-second hold opens photo reception
  in Manual mode. A successful explicit 20-second AP setup selects Automatic.
- The app must be open to discover and show a compact photo invitation. Tap
  **Send** to connect and open the photo editor. This is an in-app sheet, not a
  global iOS proximity prompt. A sleeping display cannot be remotely
  awakened over BLE.
- Select/crop/Auto Frame a photo, then Send. The phone handles resizing, nominal
  BWRY quantization, Floyd–Steinberg dithering and packing without a server.
  Custom server calibration profiles are not imported in v1.
- Transfer completion verifies SHA-256. The radio is shut down before the existing
  panel driver refreshes, to avoid overlapping radio/display current peaks.
  `photo_received` acknowledges verified reception, **not physical refresh success**.
- Cancel, disconnect, missing chunks or checksum failure never refresh partial
  data. The photo session does not paint a QR, instructions, or a closing splash.
  Maintenance continues to paint its existing QR/closing screens.

Low battery in Manual mode is self-signalling, since there is no server to paint a
charge screen. When the cell reaches the low-battery gate (the same threshold and
2-read debounce as the Wi-Fi path), the panel paints the "battery low — please
charge" splash — on a button press or on the 24-hour check — and never brings up
the BLE radio on a weak cell. It keeps re-checking every 24 hours; once the cell
charges back above the recovery threshold it paints a "ready for photos" screen,
clearing the charge splash so an untouched device recovers on its own. A brownout
reset defers with a short recovery sleep instead of waiting button-only. Pressing
the button after charging shows "ready" only once the cell has passed the recovery
threshold; a marginally-charged cell stays on the charge splash.

Manual mode intentionally stops automatic server content and telemetry. A server
may show this display as stale/offline. To resume server-managed updates, select
Automatic in Maintenance; saved network credentials and transport are retained.
No separate firmware, partition migration, or display waveform change is needed.

## Authorization and mode setting

On the existing maintenance service (`7A5E0001-7B6D-4F8B-9C2E-1D0A5A110001`),
authenticate using the current QR AES-GCM flow or authenticated LE Secure
Connections passkey flow. Diagnostics adds `screen_mode: "wifi" | "bluetooth"`.
Older firmware omits this field, so the new picker stays hidden.

Send `{"op":"set_screen_mode","value":"bluetooth","request":123}`.
After one successful NVS commit, receive:
`{"event":"screen_mode","value":"bluetooth","request":123,"photo_key":"<base64url>"}`.
`request` is an unsigned 32-bit correlation ID. The key contains 32 random bytes.
It is delivered only through the authenticated maintenance channel. Selecting
Bluetooth again returns the existing key, allowing another physically authorized
phone to join. Selecting Wi-Fi or Factory Reset revokes all photo keys; Clear
Wi-Fi preserves photo mode and authorization. A failed commit returns `error`.

Mode and key are a single `state/photo_config` NVS blob (mode byte + 32 key bytes).
The iPhone stores the key under the immutable BLE device ID in Keychain with
`WhenUnlockedThisDeviceOnly` accessibility. Mode acknowledgement and local key
storage are separate outcomes: a Keychain failure is shown and can be retried.

## Discovery and security

Photo service: `7A5E0007-7B6D-4F8B-9C2E-1D0A5A110001`.
Its ten-byte service advertisement retains major **2**, hardware **20**, mode
**0x04**, hardware suffix and random four-byte physical session ID. A distinct
service prevents old apps from mistaking photo reception for setup/diagnostics.
Earlier PicPak releases advertise code **11**, which collides with Seeed EE03.
Current Companion requires code **20**, then checks the `picpak_4_2` model and
exact advertised code in device info. Update both app and firmware; see
[maintenance compatibility](ble-maintenance.md) for the upgrade requirement.

Characteristics retain the info/control/event suffixes 0002/0003/0005. Binary
photo data uses suffix **0006**. The passkey endpoint is unavailable in a photo
session. Only photo operations are allowed; network/config/reset/key retrieval
commands are rejected.

The info characteristic supplies the immutable device ID, advertised session ID,
model `picpak_4_2`, mode `photo`, and a fresh 16-byte nonce per connection. The phone
uses its stored photo key in the existing v2 HKDF-SHA256/AES-256-GCM envelope.
Directional counters and connection nonces remain mandatory. The binary endpoint
shares the authenticated counter stream, but has its own message reassembler.
Plain native events cannot be accepted by an app using the encrypted envelope.

## Transfer contract

Control commands and events are JSON, at most 512 bytes, framed using BLE v2.
The separate binary characteristic also uses v2 encrypted chunk framing around
one binary packet. Control always uses ATT Write With Response. The optional
`write_without_response:true` capability plus characteristic 0006's Write Without
Response property enables ATT Write Commands for photo data. Older apps may keep
using reliable writes. Do not base64-encode an entire image into a control message.

1. `photo_info` -> `photo_info` event with version=1, width=400, height=300,
   bytes=30000, chunk_bytes=192, format=`picpak-bwry2-bottom-up`.
   Optional `max_window:4` matches the four-command queue;
   `write_without_response:true` opts into dynamic data sizing and the recovery
   barrier below. The pixel format, 192-byte maximum, service ID and SHA-256 are
   unchanged, so existing PicPak senders continue working.
   It also returns optional `battery_mv`, `battery_pct`, `low_battery`, `refresh_speed`
   (`5s`/`10s`/`native`) and `screen_mode` (`wifi`/`bluetooth`). Battery voltage
   is the cached reading taken on this wake before radio/display load, not a
   live charging indicator. `battery_pct` uses the same discharge curve as
   Wi-Fi heartbeats (3900 mV is 65%). `low_battery` means a valid reading below the
   existing 3400 mV low-voltage threshold; it does not change the protection
   gate. Implausible readings outside 2500–5000 mV return null for all three battery
   fields. Status is sent over the authenticated connection, without Wi-Fi or
   additional advertising. Older apps ignore these fields; newer apps omit
   unavailable values when connected to older photo firmware. Firmware/model
   remain available in the existing info characteristic.
2. `photo_begin` supplies nonzero UInt32 `id`, `bytes`=30000, that exact `format`,
   and a lowercase 64-character `sha256`. It starts/restarts the bounded buffer
   and returns `photo_ack` with matching `id` and `offset`=0.
3. Binary messages contain `[id:4 big endian][offset:4 big endian][pixels:1..192]`.
   `photo_ack` returns the cumulative next expected byte offset. Without
   `max_window`, stop and wait; otherwise keep at most the advertised number of
   ordered logical packets outstanding. Duplicates succeed only if their bytes
   exactly match already received data. A valid future offset is discarded and
   ACKs the unchanged current offset; overflow, conflicting duplicates and wrong
   IDs/generations are rejected. All retries use fresh encrypted frames.
4. `photo_end` supplies the same `id`. All 30000 bytes and the SHA-256 must match.
   `photo_received` with that ID precedes radio shutdown and panel refresh.
5. `photo_cancel` -> `photo_cancelled`, then shutdown. Closing/disconnecting also
   cancels an unfinished upload. A new connection always starts a new upload.

Pixels are row-major **bottom row first**; four pixels per byte, most significant
bits first. Palette indices: 0 black, 1 white, 2 yellow, 3 red. Therefore B/W/Y/R
packs to `0x1b`. This matches the existing PicPak driver and Tesserae renderer.

### Connection-aware data and loss recovery

For both write modes, choose `min(192, maximumWriteValueLength(.withoutResponse) - 33)`
pixel bytes per packet. The 33 bytes cover photo/fragment headers and AES-GCM
framing. Use the actual CoreBluetooth write limit and readiness callbacks each
connection. If capacity is insufficient, use the existing reliable-write path.
At MTU 256 PicPak still uses its compatible 192-byte maximum; lower capacities
use smaller chunks. A maximum is not a fixed on-air packet size or a speed claim.

A Write Command timeout stops refill and drains the phone's local write queue.
Then send `{"op":"photo_status","id":123,"request":7}` with response. The shared worker
queue places it after preceding binary data; its authenticated reply
`{"event":"photo_status","id":123,"request":7,"offset":192}` is a drain barrier. Validate
that the offset belongs to this transfer and an already sent packet boundary,
discard prior in-flight state, and restart the bounded window there with fresh
counters. Do not refill from interim normal ACKs while awaiting status. Status
requires the active transfer ID and connection generation and never changes the
buffer. Use a fresh nonzero UInt32 `request` for each barrier and accept only the
matching reply, so a delayed status cannot restart a later window. Limit no-progress
recovery to two attempts. Reliable-write clients retain
the oldest-only retry after eight seconds and never replay a whole window.

Photo notifications drain at most four frames per host callback. There is no
fixed 15 ms delay per photo ACK; controller congestion backs off one scheduler
tick with a 100 ms bound, and generation is rechecked before every enqueue.
Maintenance retains its existing pacing. Photo sessions also request 15–30 ms
connection intervals with zero peripheral latency and a 4-second timeout. The
central may refuse without failing the transfer; logs show actual interval,
latency, timeout and MTU. See [Apple QA1931](https://developer.apple.com/library/archive/qa/qa1931/)
and [ESP-IDF's NimBLE connection example](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/ble/get-started/ble-connection.html).

For hardware acceptance, compare the same 30000-byte image with old/new app and
firmware combinations, recording transfer time separately from panel refresh,
write mode, actual packet size/window, retries and serial link parameters. Test
cancel/disconnect and command/ACK loss recovery. Host tests/builds do not measure
PicPak radio throughput, power stability or physical refresh success.

On 2026-10-23, the local throughput build (still reporting 0.9.10) was installed
on one PicPak. Flash verification passed and its saved settings partition was
unchanged byte for byte. The tester confirmed successful photo sending and a
substantial perceived speed improvement. App build, negotiated transport, MTU,
transfer timings and power measurements were not captured, so this is a hardware
smoke test rather than a quantified throughput or battery result.

## Validation boundary

Run `tools/test_host.sh` for the C receiver, cross-platform BLE crypto vector,
NVS mode/key persistence and revocation, and existing maintenance regressions.
Release applications must fit one 4 MiB slot. See the
[installation instructions](../README.md#step-2-flash-the-release-build) for the complete release package.
Companion has `BLEPhotoTests` (packing/orientation/crop/ACK/keychain) and PicPak
UI journeys. Radio UI fixtures exercise the real manager and encoder but replace
the physical peripheral; they do not establish BLE interoperability or power use.

Before a hardware release verify on PicPak: QR and passkey authorization; switch
modes/reboot; single short taps; no Wi-Fi, network, or BLE-advertising wakeups in
Manual mode (only the silent 24-hour battery poll); a real photo's colors/row
order/crop; 5s/10s/Native refresh; phone loss/cancel/checksum failure; recovery
through Maintenance; and resuming every previously configured network transport.
For Manual-mode low battery specifically, verify on a drained cell: the charge
splash paints on a button press and on the 24-hour check; the device re-checks and
stays asleep while low; charging past the recovery threshold paints the "ready for
photos" screen (on the 24-hour check or a button press); and a brownout reset
defers with a short recovery sleep rather than stranding button-only. Each Manual
wake logs `manual mode: battery <mV>, gate=<0 normal/1 arm/2 stay-low>,
paint=<0 none/1 lowbatt/2 ready>, photo=<0/1>` just before the 24-hour sleep — read
it in the serial monitor to confirm the gate verdict and what was painted (it
prints after the USB-Serial-JTAG re-attach, so it survives a deep-sleep wake). Actual
throughput, power draw and panel behavior require a physical device.
