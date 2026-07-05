"""picpak_client device contract.

The PicPak custom firmware wakes on its sleep interval, fetches the current
frame over REST (or MQTT), paints the 400x300 4-colour panel, then publishes a
heartbeat before deep-sleeping. This module normalises that heartbeat for the
admin UI and guards the sleep-interval config before it goes on the wire.

Heartbeat payload (see the firmware's heartbeat.c):
    {"battery_mv": int, "battery_pct": int, "rssi": int, "ip": "...",
     "fw_version": "...", "kind": "picpak_client", "panel_w": 400, "panel_h": 300,
     "sleep_interval_s": int, "next_sleep_s": int, "wake_reason": "...",
     "sleep_until": int?}

Config payload (server -> firmware):
    {"sleep_interval_s": int}
"""
from __future__ import annotations

import json
from typing import Any

# Bounds mirror config_schema in device.json (and the firmware's SLEEP_INTERVAL_*).
SLEEP_INTERVAL_MIN_S = 30
SLEEP_INTERVAL_MAX_S = 7 * 24 * 60 * 60


def parse_status(payload: bytes) -> dict[str, Any]:
    """Decode + normalise the heartbeat. Always returns a dict with the
    well-known keys (None when absent); unknown fields pass through."""
    out: dict[str, Any] = {
        "battery_mv": None,
        "battery_pct": None,
        "rssi": None,
        "ip": None,
        # Smart-sync hints: sleep_until (unix secs) preferred over next_sleep_s.
        "sleep_until": None,
        "next_sleep_s": None,
    }
    if not payload:
        return out
    try:
        decoded = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        out["error"] = "payload was not JSON"
        return out
    if not isinstance(decoded, dict):
        out["error"] = f"expected JSON object, got {type(decoded).__name__}"
        return out
    for key, coercer in (
        ("battery_mv", int),
        ("battery_pct", int),
        ("rssi", int),
        ("ip", str),
        ("sleep_until", float),
        ("next_sleep_s", int),
    ):
        if key in decoded:
            try:
                out[key] = coercer(decoded[key])
            except (TypeError, ValueError):
                out[key] = decoded[key]
    for key, value in decoded.items():
        if key not in out:
            out[key] = value
    return out


def validate_config(payload: dict[str, Any]) -> tuple[bool, str | None]:
    """Reject out-of-bounds sleep intervals before they reach the firmware."""
    if "sleep_interval_s" not in payload:
        return False, "missing 'sleep_interval_s'"
    try:
        interval = int(payload["sleep_interval_s"])
    except (TypeError, ValueError):
        return False, "sleep_interval_s must be an integer"
    if interval < SLEEP_INTERVAL_MIN_S:
        return False, f"sleep_interval_s must be >= {SLEEP_INTERVAL_MIN_S} (got {interval})"
    if interval > SLEEP_INTERVAL_MAX_S:
        return False, f"sleep_interval_s must be <= {SLEEP_INTERVAL_MAX_S} (got {interval})"
    return True, None
