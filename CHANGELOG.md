# Changelog

## 0.3.0

### Fixed
- **DNS hijack stack overflow** (`provisioning.c`): a DNS packet longer than
  496 bytes overflowed the task stack when the 16-byte answer was appended.
  Oversized packets are now dropped. (Inherited from the reference firmware —
  report upstream.)
- **ADC-failure false low-battery lockout**: `power_read_mv()` returned `0` on
  ADC failure, which read as a flat cell and could lock a healthy device into
  the 15-minute low-power poll after two failed reads. Failures now return `-1`,
  and `lowbatt_decide` treats anything below 2500 mV
  (`LOWBATT_MIN_PLAUSIBLE_MV`) as implausible — never gating on garbage.
- **Paired-splash ran before the power-fault and low-battery gates** (`main.c`):
  the heaviest EPD operation could run on a brownout or flat-cell boot, losing
  the one-shot flag if it browned out mid-paint. The splash now runs only on a
  healthy boot; the flag survives deferred boots.
- **Portal form truncation** (`h_save`): a long fully percent-encoded submission
  could overflow the 1536-byte body buffer and silently persist a chopped
  `server_url`. Buffer is now a 3072-byte static (off the httpd stack), and
  oversized submissions are rejected with a visible form error.
- **`image_fetch` oversized/error bodies**: non-2xx responses are rejected
  before reading, and a body larger than the frame buffer now fails (one-extra-
  byte probe, catches chunked responses) instead of painting garbage.
- **Malformed percent-escapes in the portal** (`provform_url_decode`): `%zz` /
  truncated escapes embedded a string-truncating NUL via `strtol`; they now pass
  through literally.
- **Token revoked between `/frame` and `/status`**: a 401 on the `/status` POST
  now wipes the device token too (parity with the reference), re-pairing next
  wake instead of one cycle later.

### Changed
- **Radio off during panel refresh.** The wake cycle is now GET `/frame` → POST
  `/status` → WiFi off → paint → sleep, so the radio never idles (~80 mA)
  through the 13–22 s EPD refresh — the board's worst-case rail load. The
  transport buffers a validated frame (`rest_pending_frame`) and `main` paints
  it radio-off; the ETag is persisted only after a successful paint. Visible
  side effect: on repaint wakes the heartbeat lands pre-paint, so the server's
  "last seen" precedes the repaint by the paint duration (verified safe against
  the 0.71.5 smart-sync scheduler at any poll interval).
- **Lazy EPD init.** The panel is initialized only when a new frame actually
  arrives; 304/204/error wakes never power it (matches the reference).
  `epd_init` failure now keeps the last image and retries next wake instead of
  aborting.
- **NTP removed from the wake path.** Nothing in this build consumed the time,
  and the C3 keeps RTC across deep sleep (the reference's every-wake SNTP works
  around PhotoPainter's AXP2101 RTC corruption — hardware we don't have). Saves
  up to 8 s of radio-on per wake. `wifi_sync_ntp` stays compiled as the planned
  MQTT cold-boot clock source; REST will seed from `server_time` when
  `sleep_until` lands.
- `FW_VERSION` bumped `0.2.0-dev` → `0.3.0` (dropped the `-dev` suffix).

## 0.2.0-dev

### Added
- **Captive portal "Device id" now works.** The field was previously rendered
  but silently discarded on save. `h_save` now parses, validates, and persists
  it; the form pre-fills the current id (user-set, or the server's canonical id
  once paired). Blank keeps the auto-derived `picpak-<mac>` default.
- **Device-id validation** (`provform_device_id_valid`): `^[a-z][a-z0-9_-]{1,31}$`,
  matching the form's HTML pattern and the Tesserae server's rules. Invalid
  input gets an inline error instead of a silent server-side 400 loop.
- **Pairing-code flow is live.** When a code is provisioned the firmware now
  POSTs `/api/v1/device/register` with the `X-Pairing-Code` header (server
  auto-claims the device — no admin click). With no code it stays on the
  friendly `/api/v1/device/discover` path. Handles `403` (bad/expired code →
  clears it + backs off `REST_PAIR_REJECT_RETRY_S` = 1 h), `429` (rate-limited →
  backs off), and treats both `200` and `201` as success. The code is single-use
  and cleared once consumed.
- `config_get_pairing_code()` (only a setter existed before).
- **DHCP hostname** set to `tesserae-picpak` (`WIFI_HOSTNAME`) on the STA netif
  before DHCP runs, so the frame shows up identifiably in the router's client
  list instead of the ESP-IDF default `espressif`.

### Changed
  The captive portal's MQTT radio is now
  rendered `disabled` (grayed, "Coming soon — not yet selectable") and REST is
  always pre-checked, so it can't be selected by mistake. `h_save` also forces
  REST server-side (`use_rest = true`), so a stale NVS flag or a hand-crafted
  POST can no longer store `transport=MQTT` and strand the device on a transport
  with no client. The submitted transport is still parsed and logged (`req=…`).
  Both spots carry a `TODO` with the exact lines to restore when the MQTT
  client lands.
- `ensure_paired()` sends the **configured** device_id, falling back to a
  MAC-derived `picpak-<mac>` only when none is set. The server's canonical
  device_id in the response still wins and is persisted.
- `FW_VERSION` bumped `0.1.0-dev` → `0.2.0-dev`.

### Notes
- Renaming a device that has **already paired** (e.g. it auto-registered as
  `picpak-xxxx` before you set a custom id) isn't done from the portal: the
  existing token short-circuits pairing, and the server re-adopts the old id by
  MAC match. Delete it in **Settings → Devices**

## 0.1.0-dev
- Initial bring-up: panel driver, REST transport (discover + friendly claim),
  captive-portal Wi-Fi/server provisioning, battery/low-battery handling.
