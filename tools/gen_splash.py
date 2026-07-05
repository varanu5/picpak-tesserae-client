#!/usr/bin/env python3
"""Generate 400x300 2bpp BWRY splash blobs for the PicPak panel.

Draws flat-colour layouts (no photo dithering) using only the 4 firmware
palette colours, then packs 2bpp MSB-first with the SAME vertical flip as
server-plugin/renderers/esp32_bwry_bin/renderer.py (the panel scans
bottom-to-top). Outputs exactly 30000 bytes each.

Text is baked from the SoftAP constants below — keep them in sync with
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


def mark(d, x, y, s):
    """Black square logo mark (monochrome black-on-white splash)."""
    d.rectangle([x, y, x + s, y + s], fill=PALETTE[BLACK])


def make_setup():
    img = Image.new("RGB", (PANEL_W, PANEL_H), PALETTE[WHITE])
    d = ImageDraw.Draw(img)
    black = PALETTE[BLACK]
    f_brand, f_h, f_lbl, f_foot = load_font(30), load_font(34), load_font(24), load_font(20)
    mark(d, 24, 24, 28)
    d.text((62, 26), "Tesserae", fill=black, font=f_brand)
    d.text((24, 84), "Wi-Fi Setup", fill=black, font=f_h)
    d.line([24, 130, 376, 130], fill=black, width=2)
    d.text((24, 148), "Network", fill=black, font=f_lbl)
    d.text((150, 148), AP_SSID, fill=black, font=f_lbl)
    d.text((24, 184), "Password", fill=black, font=f_lbl)
    d.text((150, 184), AP_PASS, fill=black, font=f_lbl)
    d.text((24, 240), "Join this Wi-Fi, then open", fill=black, font=f_foot)
    d.text((24, 266), "http://192.168.4.1", fill=black, font=f_foot)
    return img


def make_paired():
    img = Image.new("RGB", (PANEL_W, PANEL_H), PALETTE[WHITE])
    d = ImageDraw.Draw(img)
    black = PALETTE[BLACK]
    f_brand, f_h, f_sub = load_font(30), load_font(34), load_font(24)
    mark(d, 150, 78, 28)
    d.text((188, 80), "Tesserae", fill=black, font=f_brand)
    d.text((112, 148), "Connected", fill=black, font=f_h)
    d.text((84, 196), "Waiting for first frame...", fill=black, font=f_sub)
    return img


def make_lowbatt():
    img = Image.new("RGB", (PANEL_W, PANEL_H), PALETTE[WHITE])
    d = ImageDraw.Draw(img)
    black = PALETTE[BLACK]
    f_brand, f_h, f_sub = load_font(30), load_font(34), load_font(22)
    mark(d, 150, 74, 28)
    d.text((188, 76), "Tesserae", fill=black, font=f_brand)
    d.text((120, 144), "Battery low", fill=black, font=f_h)
    d.text((92, 192), "Please connect a charger.", fill=black, font=f_sub)
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
