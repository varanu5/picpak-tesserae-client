# Changelog

## 0.7.0

### Added
- **WiFi fast-connect (both transports).** After the first association the AP's
  BSSID + primary channel are cached in NVS (`config_{get,set,clear}_ap_hint`,
  `wifi` namespace) and targeted directly on the next wake, so the radio skips
  the ~1–2 s all-channel scan — less radio-on time per wake (battery, and a
  smaller brownout-sag window). It runs before the REST/MQTT split, so both
  transports benefit. A stale hint (AP moved channel / router swapped) fails
  fast on one retry (`WIFI_FAST_CONNECT_RETRIES`), clears the hint, and falls
  back to a normal full-scan connect that re-caches the real AP — one slower
  wake, then fast again (self-healing, hardware-verified across a channel
  change). DHCP unchanged. Ported from `tesserae-device-firmware-1.5.1`.
- **Physical-button refresh (REST).** A **~3 s front-button hold** — classified
  on release, so the existing gestures are untouched (quick tap → wake + check,
  3–20 s → refresh, ≥20 s → provisioning portal) — sends
  `GET /frame?button=refresh&button_event_id=<n>` and drops `If-None-Match`, so
  the server re-renders the current frame (fresh data — e.g. a live clock/weather)
  and the panel repaints. An RTC-retained monotonic event id dedups one physical
  press across the `/frame` request and a `/status` fallback field. MQTT logs and
  ignores the gesture (the server dispatches button actions only on its REST
  endpoints). Note: the server re-renders only for a **rotation-bound** device — a
  directly-pushed page has no rotation step to refresh and the press no-ops.

### Changed
- **`sleep_until` reported in `/status`.** The heartbeat now carries the absolute
  epoch the device intends to wake next (`now + interval`, guarded by a sane-clock
  check), matching the reference payload shape. Redundant in effect — the server
  prefers `next_sleep_s`, which we already send, and drops `sleep_until` on
  disagreement — but kept for parity.
- **REST control-response buffers `2 KB → 4 KB`** (`REST_RESP_MAX`), matching the
  reference's headroom against a growing server `config` object (static `.bss`
  only, ~+6 KB RAM; overflow is still logged as a tripwire).
- `FW_VERSION` bumped `0.6.0` → `0.7.0`.

## 0.6.0

### Fixed
- **Revoked-token recovery (REST).** `http_do()` now trusts the HTTP status
  line even when `esp_http_client_perform()` returns an error. A Bearer-token
  API answers a revoked/expired token with a **401 carrying no
  `WWW-Authenticate` header**, which the client's auto-handling reports as
  `ESP_ERR_NOT_SUPPORTED`; the old code gated the status on `err == ESP_OK`
  and so saw the 401 as a transport error (`-1`), never wiping the token —
  the device would loop forever unauthenticated. It now reads the real status
  and only treats a response-less failure as a transport error, so a 401/403
  on `/frame` or `/status` correctly wipes the token and re-pairs next wake.
  Confirmed on hardware (the tell-tale `HTTP_CLIENT: This request requires
  authentication…` log alongside `GET /frame -> 401 … wiping token`).

### Changed
- **Wall clock from the server `Date` header (REST).** Each REST response's
  `Date` header is parsed (RFC 1123 → epoch) and applied with
  `settimeofday()`, keeping the C3 RTC accurate across sleeps without an SNTP
  round-trip. The `main.c` NTP gates for `https`/`mqtts` cert-validity
  bootstrap are untouched (TLS still needs a sane clock before the response's
  `Date` can arrive).
- **`Retry-After` honoured on 429.** A rate-limited discover/register now
  backs off by the server's `Retry-After` seconds when present, instead of
  always using the fixed one-hour fallback (kept for when the header is
  absent; the final value is still clamped to the sane sleep bounds).
- **Control-response buffers hardened.** The discover/frame/status JSON
  response buffers grew to a shared 2 KB (`REST_RESP_MAX`) with explicit
  overflow detection and a truncation warning, replacing the previous silent
  truncation of a larger-than-expected `config` object.

## 0.5.0

### Added
- **HTTPS server URLs.** `https://` server URLs now work end-to-end (REST
  calls and the frame download), validated against ESP-IDF's built-in CA
  bundle — publicly-trusted certificates only (e.g. Let's Encrypt behind a
  reverse proxy); self-signed won't validate. The bundle is attached only on
  `https://` URLs (attaching it on plain http mis-configures the client).
  Before an https REST cycle with an implausible clock, the MQTT-mode SNTP
  sanity sync runs once so the first TLS handshake doesn't fail the
  certificate validity check on a 1970 clock. Portal hint updated to match.
- **Post-setup error feedback (WiFi).** One-shot on the first boot after
  provisioning: a WiFi failure with a wrong-password signature (`AUTH_FAIL`,
  `AUTH_EXPIRE`, handshake timeouts) or a no-such-network signature
  (`NO_AP_FOUND` + security-mismatch variants) reopens the captive portal
  with a matching error banner instead of silently sleep-retrying forever.
  Transient outages match neither signature and keep the silent retry, so a
  provisioned device never drops to AP mode on a router blip. Deliberately
  NOT extended to the server/broker URL: an unreachable backend at first
  boot is usually the user's own server restarting — the frame keeps
  retrying on its own and catches up when it returns (a genuinely wrong URL
  is fixed via the 20 s button-hold portal).
- **MQTT transport (Mode 0).** Single-session wake loop
  (`mqtt_handler.c`): one broker connect per wake — subscribe the retained
  `tesserae/<id>/frame/bin` + `config` topics, HTTP-download the frame,
  publish the heartbeat retained (QoS 1, PUBACK-confirmed), graceful stop
  before the radio goes down, then paint radio-off like the REST path.
  Frame skip by URL match (new NVS key `rest/frame_url`, kept separate from
  the REST ETag so transport switches never false-skip). Non-retained
  `{"state":"offline"}` LWT so an ungraceful drop is visible without
  clobbering the retained heartbeat that feeds Tesserae's discovery UI.
  Validated on hardware: full bench e2e against a local Mosquitto, then
  against a real Tesserae server over an external Mosquitto (discovery →
  claim → frame → sleep-interval change from the UI).
- **Captive portal: MQTT selectable again.** The transport radio is live
  (pre-checked from the stored mode), broker URI/username echoed on
  re-provision, broker URI required in MQTT mode, blank broker password
  keeps the stored one, bare `host:1883` gets `mqtt://` prepended.
- **Clock-sane NTP for MQTT mode.** MQTT carries no `server_time`, so when
  `time(NULL)` is implausible (`CLOCK_SANE_EPOCH`) the wake runs the
  best-effort SNTP sync — normally once per power-on; needed for `mqtts://`
  cert validation. REST stays NTP-free.
- Host test `test_mqtt_parse.c` for the new pure helpers (`mqtt_parse.c`:
  URL/int payload extraction, broker-URI normalization).
- **DHCP hostname = device id.** The frame advertises its provisioned device
  id to the router (e.g. `picpak-red`, `_` mapped to `-`), so the router's
  client list matches Tesserae's device list; an unnamed frame advertises
  `tesserae-picpak-<mac3>` so multiple PicPaks stay distinguishable
  (previously every frame was the fixed `tesserae-picpak`).

### Fixed
- **Discover retry backs off when the server is unreachable.** A freshly
  set-up device whose Tesserae server isn't running yet was waking every
  30 s to hammer it; the connection-failure fallback is now 15 min. The
  actual claiming handshake is unaffected — when the server is up but hasn't
  claimed the device it dictates the fast cadence via `retry_after_s` — and a
  front-button press onboards immediately once the server comes up.
- **Token wiped on `403`, not just `401`.** The server 403s a bearer token
  bound to a renamed/re-canonicalized device id; the device now re-pairs on
  the next wake instead of retrying forever (parity with the reference).
- **Relative frame URLs resolved against the server origin.** A path-only
  `url` from `GET /frame` (possible behind a proxy) previously failed the
  download silently every wake; absolute URLs pass through unchanged.
- **WPA2 required when a WiFi password is stored.** The STA auth threshold
  defaulted to OPEN, so the device would join an unencrypted rogue AP
  broadcasting the provisioned SSID and leak the bearer token in cleartext.
- **Portal idle timeout no longer cuts off an active user.** The 600 s
  window used to be a hard total; it now counts only while no client is
  associated with the setup AP and restarts when the last one leaves
  (reference behaviour).
- **Unreachable-broker wakes no longer burn ~5 s of idle radio.** Bench
  measurement showed `esp_mqtt_client_stop()` waiting out esp-mqtt's
  reconnect state (the WAIT_RECONNECT loop polls its stop flag every
  `reconnect_timeout_ms / 2`; 5 s at the 10 s default). Auto-reconnect is
  now disabled (a failed connect fails the wake — retry is the next wake)
  with a 1 s reconnect poll, bringing failure-path teardown to ≤0.5 s
  (bench-verified).

### Changed
- The 30,000-byte frame staging buffer moved out of `rest_handler.c` into a
  shared `framebuf.{c,h}` used by both transports (only one runs per boot;
  saves a duplicate 30 KB .bss array).
- `FW_VERSION` bumped `0.4.0` → `0.5.0`.

## 0.4.0

### Fixed
- **Firmware-review "smaller items"**
  - Server-URL normalization now trims surrounding whitespace and trailing
    slashes (classic paste errors; a kept trailing slash produced
    `//api/v1/...` request URLs). Six new host-test cases in
    `test_provision_form.c`.
  - Portal form no longer displays a chopped server URL: the HTML-escape
    buffer now fits the worst-case fully-escaped 159-char URL
    (`e_server` 640 → 960, downstream `form_rest` 1600 → 1856).
  - The captive-portal DNS hijack task now shuts down cooperatively (stop
    flag + 250 ms recv timeout) and closes its own socket, instead of being
    `vTaskDelete`d mid-`recvfrom` and leaking the fd.
  - Two items resolved without code: the heartbeat `next_sleep_s` mismatch is
    deferred to the offline-resilience milestone (any device-side fix today
    risks worse smart-sync behaviour), and the AP-SSID MAC suffix is declined
    (single-device household; keeps the setup splash's literal SSID accurate).

### Changed
- **Colour-block splash redesign.** The three boot splash screens now use a
  full-height colour panel on the left with the status icon inside it and the
  text set to its right — yellow for setup, black for paired/connected, red for
  low battery. Regenerated from a rewritten `tools/gen_splash.py`; the compiled
  `firmware/main/assets/splash_{setup,paired,lowbatt}.bin` blobs are updated.
- `FW_VERSION` bumped `0.3.0` → `0.4.0`.

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
  MAC match. Delete it in **Settings → Devices** (tick "also wipe" if offered)
  and let it re-discover under the new id.

## 0.1.0-dev
- Initial bring-up: panel driver, REST transport (discover + friendly claim),
  captive-portal Wi-Fi/server provisioning, battery/low-battery handling.