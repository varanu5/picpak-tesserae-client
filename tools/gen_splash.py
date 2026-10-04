#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 varanu5 <https://github.com/varanu5>
"""Generate 400x300 2bpp BWRY splash blobs for the PicPak panel.

Draws flat colour-block layouts (spec: docs/superpowers/specs/
2026-07-09-splash-redesign-design.md) using only the 4 firmware palette
colours, then packs 2bpp MSB-first with the SAME vertical flip as
server-plugin/renderers/esp32_bwry_bin/renderer.py (the panel scans
bottom-to-top). Outputs exactly 30000 bytes each.

The base network label is replaced with the device SSID at runtime.
Other text is baked from the SoftAP constants below — keep them in sync with
PROVISION_AP_SSID / PROVISION_AP_PASS in firmware/main/defaults.h and re-run
this script if they change:
    python3 tools/gen_splash.py
"""
import os
from PIL import Image, ImageDraw, ImageFont
import numpy as np

PANEL_W, PANEL_H = 400, 300
# index -> RGB; MUST match board.h / renderer.py: 0=Black 1=White 2=Yellow 3=Red
PALETTE = [(0, 0, 0), (255, 255, 255), (255, 255, 0), (255, 0, 0)]
BLACK, WHITE, YELLOW, RED = 0, 1, 2, 3

AP_SSID = "Tesserae-Setup"   # == PROVISION_AP_SSID
AP_PASS = "tesserae"         # == PROVISION_AP_PASS

_FONT_CANDIDATES = [
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/HelveticaNeue.ttc",
    "/Library/Fonts/Arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]


def load_font(size):
    for p in _FONT_CANDIDATES:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size=size)
            except Exception:
                pass
    return ImageFont.load_default()


def rgb_to_indices(img):
    """Map an RGB image (drawn with palette colours) to nearest palette index.
    int32 (not int16): a 255^2*3 = 195075 squared distance overflows int16 and
    wraps negative, which inverts the nearest-colour pick (white<->black)."""
    arr = np.array(img, dtype=np.int32)
    pal = np.array(PALETTE, dtype=np.int32)
    d = ((arr[:, :, None, :] - pal[None, None, :, :]) ** 2).sum(axis=3)
    return d.argmin(axis=2).astype(np.uint8)   # H x W indices 0..3


def pack_2bpp(idx):
    """Vertical flip + pack 2bpp MSB-first -> 30000 bytes (matches renderer.py)."""
    h, w = idx.shape
    plane = idx[::-1, :]
    groups = plane.reshape(h, w // 4, 4)
    packed = (groups[:, :, 0] << 6) | (groups[:, :, 1] << 4) \
           | (groups[:, :, 2] << 2) | groups[:, :, 3]
    out = packed.astype(np.uint8).tobytes()
    assert len(out) == w * h // 4 == 30000, f"got {len(out)} bytes"
    return out


def brand(d, x, y):
    """Small square mark + wordmark, black on white."""
    d.rectangle([x, y + 2, x + 16, y + 18], fill=PALETTE[BLACK])
    d.text((x + 26, y), "Tesserae", fill=PALETTE[BLACK], font=load_font(20))


def wifi_icon(d, cx, cy, color):
    """Wi-Fi arcs opening upward, dot at (cx, cy)."""
    for r in (16, 30, 44):
        d.arc([cx - r, cy - r, cx + r, cy + r], start=225, end=315,
              fill=color, width=6)
    d.ellipse([cx - 6, cy - 6, cx + 6, cy + 6], fill=color)


def check_icon(d, cx, cy, r, color):
    """Circle outline with a checkmark, single colour."""
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=color, width=5)
    w = int(r * 0.9)
    pts = [(cx - w * 0.55, cy + 0.05 * w), (cx - w * 0.15, cy + 0.45 * w),
           (cx + w * 0.6, cy - 0.4 * w)]
    d.line(pts, fill=color, width=7, joint="curve")


def battery_icon(d, cx, cy, color, w=80, h=44):
    """Battery outline + terminal nub, charge segment at ~22%."""
    x0, y0, x1, y1 = cx - w // 2, cy - h // 2, cx + w // 2, cy + h // 2
    d.rounded_rectangle([x0, y0, x1, y1], radius=8, outline=color, width=5)
    d.rounded_rectangle([x1 + 3, cy - 12, x1 + 12, cy + 12], radius=3, fill=color)
    d.rectangle([x0 + 7, y0 + 7, x0 + 7 + int((w - 14) * 0.22), y1 - 7], fill=color)


def colour_page(panel_color):
    """White page with a full-height state-colour panel at x 0-128 and brand row."""
    img = Image.new("RGB", (PANEL_W, PANEL_H), PALETTE[WHITE])
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 128, PANEL_H], fill=panel_color)
    brand(d, 152, 22)
    return img, d


def make_setup():
    img = Image.new("RGB", (PANEL_W, PANEL_H), PALETTE[WHITE])
    d = ImageDraw.Draw(img)
    d.text((152, 22), "Tesserae", fill=PALETTE[BLACK], font=load_font(20))
    wifi_icon(d, 76, 150, PALETTE[BLACK])
    d.text((152, 58), "Wi-Fi Setup", fill=PALETTE[BLACK], font=load_font(30))
    f_lbl, f_val = load_font(15), load_font(23)
    d.text((152, 116), "NETWORK", fill=PALETTE[RED], font=f_lbl)
    d.text((152, 136), AP_SSID, fill=PALETTE[BLACK], font=f_val)
    d.text((152, 176), "PASSWORD", fill=PALETTE[RED], font=f_lbl)
    d.text((152, 196), AP_PASS, fill=PALETTE[BLACK], font=f_val)
    d.text((152, 232), "Scan QR or join this Wi-Fi.", fill=PALETTE[BLACK], font=load_font(16))
    d.text((152, 252), "If setup doesn’t open, visit", fill=PALETTE[BLACK], font=load_font(16))
    d.text((152, 272), "http://192.168.4.1", fill=PALETTE[BLACK], font=load_font(16))
    return img


def make_paired():
    img, d = colour_page(PALETTE[BLACK])
    check_icon(d, 64, 150, 34, PALETTE[WHITE])
    d.text((152, 112), "Connected", fill=PALETTE[BLACK], font=load_font(32))
    d.rectangle([152, 156, 232, 161], fill=PALETTE[YELLOW])
    d.text((152, 176), "Waiting for", fill=PALETTE[BLACK], font=load_font(20))
    d.text((152, 202), "first frame...", fill=PALETTE[BLACK], font=load_font(20))
    return img


def make_lowbatt():
    img, d = colour_page(PALETTE[RED])
    battery_icon(d, 62, 150, PALETTE[WHITE])  # 62 not 64: optical centre incl. nub
    d.text((152, 104), "Battery low", fill=PALETTE[BLACK], font=load_font(32))
    d.rectangle([152, 148, 232, 153], fill=PALETTE[RED])
    d.text((152, 168), "Please connect", fill=PALETTE[BLACK], font=load_font(20))
    d.text((152, 194), "a charger.", fill=PALETTE[BLACK], font=load_font(20))
    return img


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    outdir = os.path.normpath(os.path.join(here, "..", "firmware", "main", "assets"))
    os.makedirs(outdir, exist_ok=True)
    for name, img in [("splash_setup", make_setup()), ("splash_paired", make_paired()),
                      ("splash_lowbatt", make_lowbatt())]:
        blob = pack_2bpp(rgb_to_indices(img))
        path = os.path.join(outdir, name + ".bin")
        with open(path, "wb") as fh:
            fh.write(blob)
        print(f"wrote {path} ({len(blob)} bytes)")


if __name__ == "__main__":
    main()
