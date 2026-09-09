# Changelog

## 0.9.5

### Added
- Bluetooth photo connections now report battery voltage, low-voltage status,
  refresh speed and screen mode to Companion without starting Wi-Fi.
- **Manual Bluetooth photo mode.** Choose Automatic (Wi-Fi) or Manual (Bluetooth)
  in Companion's PicPak maintenance screen. Manual mode pauses scheduled network
  updates and heartbeats, sleeps until a button press, and receives photos directly
  from an authorized iPhone. Wi-Fi/server settings and refresh speed are retained.
- Photo authorization is established through authenticated maintenance and stored
  on the display. Returning to Automatic mode or performing Factory Reset revokes
  photo authorization. Complete uploads are verified before refreshing; cancelled
  or incomplete photo sessions leave the existing picture intact.

### Fixed
- Manual (Bluetooth) mode now gives its own low-battery feedback and recovers on
  its own. When the cell reaches the low-battery gate it paints the "battery low —
  please charge" splash (on a button press or a silent 24-hour battery check) and
  keeps the radio off; previously it slept button-only with no on-screen warning and
  no scheduled re-check. It re-checks every 24 hours and, once charged past the
  recovery threshold, paints the "Bluetooth photos" ready screen so an untouched
  device recovers unattended. A brownout reset now defers with a short recovery
  sleep instead of waiting only for a button. Diagnostic log line added
  (`manual mode: battery … mV, gate=…`) for hardware verification.
- Leaving Bluetooth maintenance in Manual mode now shows how to send a photo
  with one button press, replacing the misleading "Bluetooth closed" Wi-Fi
  message. A consistent text size, numbered steps and aligned spacing make
  the guide easier to follow. The expired QR code is still cleared before sleeping.
- Maintenance entry guidance follows the upstream 10-second hold in Manual
  Bluetooth mode as well as Automatic mode.

## 0.9.4

### Added
- Exit Bluetooth maintenance without a phone by briefly pressing and releasing
  the wake button after the QR screen is ready. Saved settings are retained,
  and the expired QR is cleared before normal operation resumes.
- **Bluetooth maintenance from Tesserae Companion.** Hold the wake button for
  about 10 seconds (release between 10 and 20 seconds) to open a five-minute
  local session with a QR code and six-digit passkey. Read diagnostics, repair
  Wi-Fi while preserving REST/MQTT/relay configuration, restart, or clear
  settings without requiring a reachable Tesserae server. The 5-second refresh
  and 20-second AP setup gestures remain available.
- **Refresh speed over Bluetooth.** Choose 5 s, 10 s, or Native in Companion.
  The setting is shared with the AP portal, saved on the display, and takes
  effect on the next refresh. Companion confirms a change only after it is saved.

### Changed
- **Bluetooth maintenance now enters at ~10 s instead of ~3 s.** The button
  gesture map is reordered so maintenance sits above refresh: tap (deck next)
  is now anything under 5 s, refresh is 5–10 s, maintenance is 10–20 s, and AP
  setup stays at 20 s. This widens the deck-next tap window and stops a
  slightly over-held refresh (or deck-next) from accidentally opening a BLE
  session. The hold LED cue follows suit: steady-on at 5 s (refresh armed),
  pulsing at 10 s (release for Bluetooth), burst at 20 s (AP setup).

### Fixed
- The "still holding" boot-hold log now fires on elapsed time rather than an
  exact `waited % 1000` match that a timer-derived counter almost never hit.

## 0.9.3

### Added
- **Temperature-compensated refresh.** On the 5 s / 10 s waveforms the panel drive is now nudged
  slightly based on the on-chip temperature sensor. The effect is automatic and subtle (no setting,
  no visible change in normal use); the native waveform is unchanged.
  
## 0.9.2

### Added
- **Selectable refresh speed.** The setup portal now offers three e-paper refresh waveforms — **5 s**
  (fastest), **10 s** (balanced), and the panel's **native** waveform (no ghosting, slowest).
  It trades redraw speed against colour fidelity, is saved in NVS, and can be changed any time via the
  20 s button-hold portal. The default compile-time choice is 5 s.

## 0.9.1

### Fixed
- **Splashes stranded on MQTT and cloud relay** (`firmware/main/config_store.c`, `config_store.h`,
  `main.c`). The low-battery charge splash and the post-provisioning "paired" splash are painted
  outside the normal frame path, so firmware clears the frame dedup key afterwards to force a repaint
  of the live photo. That clear (`config_set_etag("")`) only touched the **REST** ETag — on MQTT the
  retained frame URL still matched (`frame url unchanged; skipping download`) and on relay the stored
  ETag still returned a `304`, so on both transports the panel stayed stuck on the splash until the
  server published a genuinely different frame. A recovered low-battery device therefore did not come
  back on MQTT or relay, and a re-provisioned MQTT device sat on the "paired" splash. New
  transport-agnostic `config_clear_frame_ref()` clears all three dedup keys (REST ETag, MQTT frame
  URL, relay ETag); both call sites (`main.c` lock-recovery and `paired_pending`) now use it.
- **`mqtts://` TLS had no trust anchor** (`firmware/main/mqtt_handler.c`). The setup portal offers
  `mqtts://` (and `wss://`) for TLS and the URI normaliser accepts it, but the MQTT client config was
  built with no CA source, so the handshake always failed with a generic `broker connect failed`. Now
  attaches the same public-CA bundle the REST and relay paths use, conditional on the secure scheme
  (`mqtts://` / `wss://`); plain `mqtt://` / `ws://` are untouched. Publicly-trusted broker certs
  (e.g. HiveMQ/EMQX cloud) validate; a self-signed LAN broker still won't (private-CA / PSK out of
  scope).
- **Button event id could start from RTC garbage** (`firmware/main/main.c`). `s_button_event_seq`
  (`RTC_NOINIT_ATTR`, never initialised by startup) was only zeroed on `ESP_RST_POWERON`, but a
  freshly flashed device's first boot usually reports `ESP_RST_USB` (esptool / web flasher),
  `ESP_RST_SW` (the `esp_restart()` after provisioning), or `ESP_RST_UNKNOWN` — leaving the counter at
  a random value. It still increments monotonically so ordinary use is fine, but a start value that
  collides with an id the server already recorded silently drops that button press once. Guard
  inverted to zero on every reset reason except a genuine deep-sleep wake (`reason != ESP_RST_DEEPSLEEP`).
  Added a note at the two `RTC_DATA_ATTR` counters (`relay.c`, `lowbatt.c`) — zero-initialised on cold
  boot and correct as-is — so a future "consistency" edit doesn't break them.
- **Truncated heartbeat JSON could be sent unterminated** (`firmware/main/heartbeat.c`). The status
  object is built with its closing brace omitted so optional fields (`sleep_until`, the button pair)
  can be appended, and each append guarded truncation with a bare `return` — leaving the object
  without its closing `}`. The caller then POSTed the fragment, which the server rejects wholesale,
  losing the telemetry in it. Not reachable at the current buffer size, but a trap as fields
  accumulate. Truncation now emits a minimal valid object (`{"fw_version":"…"}`) and logs an
  `ESP_LOGE`; the final guard reserves room for the `}`.

## 0.9.0

### Added
- **Cloud relay transport** (`firmware/main/relay.c`, `relay_crypto.c`, `relay_wire.c`,
  `vendor/monocypher.c`; `config_store.c`, `provisioning.c`, `main.c`). A mutually-exclusive third
  transport for a panel that can't reach the Tesserae server directly (another home, CGNAT, a
  hotspot): both ends connect **outbound** to a mailbox Worker, home seals each frame and the panel
  polls + decrypts it. **Zero-knowledge** — X25519 public-key exchange at pairing derives an
  AES-256-GCM frame key on each side (never transmitted); the relay holds only ciphertext. A failed
  GCM tag is never painted, and the decrypted length is validated against the panel's 30 000-byte
  frame. Pair it from the setup portal's new **Cloud relay** option (relay URL + a single-use pairing
  code from *Settings → Cloud relay*); no server URL needed. When paired the device runs the relay
  cycle (frame fetch → status POST → config sync, adopting `sleep_interval_s`) and skips REST/MQTT
  entirely; unpaired devices are unchanged. Enables `CONFIG_MBEDTLS_HKDF_C`. Crypto is pinned to the
  relay contract's golden vectors by `tools/test_relay_crypto.sh` (127 checks). **Hardware-confirmed**
  end-to-end against the hosted `relay.tesserae.ink` + a NUC home instance: pairing, frame decrypt +
  paint, telemetry, config adoption, and ETag/304 dedup.
- **Buttons over relay** (`relay_wire.c` `relay_build_status_body`, `relay.c`, `main.c`; needs
  **server/relay v0.240.0+**). A relay panel's frame GET terminates at the relay, so the press rides
  the **status body** instead (`button` + `button_event_id`, emitted together-or-not; id required, home
  dedups on it). A **tap → deck-next**, a **5 s-hold → refresh** — the same names REST sends, so the
  server's Button map dispatches them. Delivery is store-and-forward (~30 s first press), so after a
  press the panel stays awake a fixed window (`RELAY_BUTTON_WINDOW_S`, 45 s) polling the mailbox and
  painting home's rendered response when it arrives. **The manual-deck-over-relay limitation is
  lifted.** On an older server the button fields are ignored (harmless). Hardware-confirmed. The
  status-body button contract is host-tested (`tools/test_relay_crypto.sh`, now 147 checks).

### Fixed
- **Relay frame/config repainted every wake** (`firmware/main/relay.c`). `relay_get_sealed` read the
  response ETag with `esp_http_client_get_header()`, which returns only *request* headers — so the
  ETag was never captured (`etag (none)`), no `If-None-Match` was ever sent, and every wake got a
  `200` + full ~20 s repaint of the same frame (and re-applied config). Capture the ETag via the
  `HTTP_EVENT_ON_HEADER` event instead (matching `rest_handler.c`); unchanged frames now `304`.
  Hardware-confirmed.
- **Relay device delete / re-add + auto revoke-recovery** (`provisioning.c`, `relay.c`, `main.c`,
  `config_store.c`). Re-provisioning a relay device with a new pairing code clears the old pairing
  first, so it re-pairs cleanly — a stale frame key previously blocked re-pairing (a factory reset was
  the only recourse). And on **server/relay v0.240.0+**, a revoke deletes the device token, so a `401`
  from any device-token route is unambiguous: **two consecutive `401` wakes** (`note_auth` +
  `s_auth_fail_streak` in RTC) trip `relay_pairing_revoked()` → the panel forgets the pairing (keeps
  the relay URL via `config_forget_relay_pairing`), paints a dedicated **"Unpaired" splash**
  (`splash_show_revoked`), and re-pairs on a fresh code via the 20 s-hold portal (URL pre-filled). Two
  wakes, not one, so a captive-portal/middlebox `401` can't unpair a working panel; a `204` (nothing
  published yet) never triggers it. Superseded the earlier single-401 immediate wipe. Hardware-confirmed.

### Changed
- **Relay setup portal hides the device-id field** (`provisioning.c`). The device id is ignored on
  relay (identity comes from pairing / the server's *Add a remote panel* slot), so the Device card
  is hidden when the transport is **CLOUD RELAY**; REST/MQTT unchanged.
- **Portal polish** (`provisioning.c`): a **blank Relay URL now defaults** to the hosted
  `https://relay.tesserae.ink` (matching the field's hint — previously an empty URL was rejected);
  the transport option reads **CLOUD RELAY** (all caps, like REST API / MQTT); and REST API's
  **(recommended)** is now bold.
- `FW_VERSION` bumped `0.8.2` → `0.9.0`.

## 0.8.2

### Added
- **Button-tap deck navigation** (`firmware/main/rest_button.h`, `power.c`, `rest_handler.c`,
  `main.c`). A **brief button hold at wake (~0.5 s — anything under the refresh threshold)** now
  advances the device to the **next page of its bound Deck** in Tesserae: the wake sends
  `GET /frame?button=right`, which the server turns into a deck next-page navigation and serves the
  pre-warmed frame. A **quick tap** (press-and-release) is unchanged — it just wakes and checks for
  a new photo, so a directly-pushed image still comes through. Unlike the refresh gesture, a deck-next
  keeps its `If-None-Match` ETag, so it stays a superset of a plain wake (a no-op `304` when there is
  no Deck bound). REST transport only; on MQTT the gesture is logged and ignored.

### Changed
- **Force-refresh hold raised 3 s → 5 s** (`firmware/main/defaults.h`, `BTN_REFRESH_HOLD_MS`). Holding
  the button now requests a refresh at **~5 s** (was ~3 s); the status-LED "refresh armed" steady-on
  point and the low-battery force-resume override move with it (both key off the same refresh gesture).
  This widens the sub-5 s window used by the new deck-next tap. The ~20 s provisioning hold is
  unchanged.
- `FW_VERSION` bumped `0.8.1` → `0.8.2`.

## 0.8.1

### Fixed
- **Captive-portal setup form rejected with HTTP 431** (`firmware/sdkconfig.defaults`). The
  provisioning HTTP server used the ESP-IDF default request-header cap of 512 bytes. A normal
  browser carrying cookies for `192.168.4.1` overruns that, so submitting the setup form failed
  with `431 "Header fields are too long"` and the device never joined WiFi or registered with the
  server (incognito mode, having no cookies, sent smaller headers and worked). `CONFIG_HTTPD_MAX_REQ_HDR_LEN`
  is raised `512 → 2048` (and `CONFIG_HTTPD_MAX_URI_LEN` `512 → 1024`) so a cookie-laden setup
  submission is accepted from any normal browser. The extra RAM is used only while the setup AP is up.
- `FW_VERSION` bumped `0.8.0` → `0.8.1`.

## 0.8.0

### Fixed
- **Panel deep-sleep command completed** (`firmware/main/epd_driver.c`). `epd_sleep()`
  issued a bare Deep Sleep opcode (`0x07`) with no parameter; this controller requires the
  `0xA5` check-code byte to enter deep sleep (a built-in guard against an accidental sleep),
  so a bare `0x07` is ignored and the panel stays in a higher-power standby between wakes. It
  now sends `0x07, 0xA5` (and the `0x00` parameter with Power OFF `0x02`), matching the
  controller's required framing, so the panel enters its proper low-power deep-sleep state
  between wakes.

### Added
- **Status-LED feedback** (`firmware/main/led.c`, `led.h`, `power.c`, `main.c`). The on-board
  LED (GPIO21) now gives screen-free status: **one blink on every wake** (timer or button) as a
  "the frame woke and is working" pulse, and while you hold the button at wake a gesture
  progression — off (`<3 s`) → **steady-on** (refresh armed at ~3 s) → **rapid burst**
  (provisioning armed at ~20 s) — so you can feel how long you have held. LED is active-low.

### Changed
- **Console moved to USB-Serial-JTAG** (`firmware/sdkconfig.defaults`). GPIO21 is both the status
  LED and the UART0 TX pin; with the console on UART0 the pin flickered with log traffic. The
  console is now solely the native USB-Serial-JTAG — where the logs were already read (the
  `/dev/cu.usbmodem*` port), so serial monitoring is unchanged — and UART0 no longer drives
  GPIO21, leaving it a clean, dedicated LED.
- `FW_VERSION` bumped `0.7.1` → `0.8.0`.

## 0.7.1

### Changed
- **Low-battery gate retuned** (`firmware/main/lowbatt.h`). ARM raised **3300 → 3400 mV**
  and CLEAR **3500 → 3550 mV**; the low-power poll goes **900 s → 86400 s** (daily).
  ARM now sits exactly on the `battpct.h` floor, so the reported percentage hits 0
  when the charge screen appears instead of after it, and the frame stops attempting
  WiFi ~100 mV earlier — that band bought almost no runtime and was spent entirely in
  the zone where a TX burst sags the rail into a brownout. The daily poll removes the
  polling component of gated drain almost entirely (~0.1–0.5 mAh/day → negligible),
  but **total** gated drain only falls by something like 1.5–2×, because what remains
  is the deep-sleep floor, which the permanent battery-sense divider may well dominate.
  The honest justification is that there is nothing useful to do more often than daily
  once gated, not a dramatic power rescue. Hardware-observed firing on a real low cell.
- **Physical button made battery-safe in the low-battery gate** (`lowbatt_core.h`,
  `lowbatt.c`, `main.c`). Previously *any* button wake — including a quick tap —
  force-resumed the gate to NORMAL, driving a WiFi fetch + repaint (the heaviest rail
  load) on a nearly-dead cell, the exact brownout the gate exists to prevent. Now,
  while the gate is **locked**:
    - a **quick tap** only re-measures the battery — no radio — and resumes just if the
      cell has genuinely recovered; otherwise it drops straight back to the low-power
      poll (the persistent e-paper charge splash is already on screen, so nothing
      repaints);
    - a **~3 s hold** is a deliberate override that force-resumes and refreshes,
      behaving identically to the good-battery button-refresh (pulls fresh content /
      rotation + repaints) — the manual escape hatch if a reading is ever wrong;
    - a still-low override **re-arms on the very next reading** (the debounce is
      preloaded), so a force-resume on a truly flat cell costs exactly one fetch instead
      of several. The `force_resume` semantic replaces `button_wake` in the FSM.
  A tap is now the fast recovery path after plugging in, so the daily poll stays a rare
  fallback.
- **Provisioning refused on a flat cell.** The **20 s** re-provision hold is ignored
  while the gate is locked (charge splash instead of the captive portal): AP-mode radio
  for a whole setup session is sustained heavy load, and a brownout mid-setup would lose
  the credentials. Guarded on the gesture, so first-time setup (no saved creds) is
  unaffected — a fresh device is never in the locked state.
- **Live photo repaints over the charge splash on recovery.** The charge splash is
  painted outside the ETag machinery, so an automatic recovery (a tap that charged, or
  the daily poll) could re-fetch, receive a `304 Not Modified`, skip the paint, and
  leave the splash stranded on screen. The device now clears the stored ETag on an
  automatic recovery to force a `200` repaint (REST). A 3 s hold already forces this via
  its refresh path.
- `FW_VERSION` bumped `0.7.0` → `0.7.1`.

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
