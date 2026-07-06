# Changelog

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
