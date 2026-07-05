"""esp32_bwry_bin renderer — PicPak 400×300 4-colour BWRY, 2 bits/pixel.

Wire contract (matches the PicPak custom firmware + fb2bpp packing):

* Exactly ``width * height / 4`` bytes = 30000 for 400×300.
* 2 bits/pixel, 4 pixels packed per byte, **MSB-first** (leftmost pixel in
  bits 7:6). Palette indices: 0=Black, 1=White, 2=Yellow, 3=Red.
* Scanline order, no row padding. The firmware streams these bytes straight
  to the panel over SPI (command 0x10), no on-device conversion.

Pipeline: fit -> optional saturation/contrast -> dither to the 4-colour BWRY
palette (reusing Tesserae's dither engine so all modes are available) -> pack 2bpp.
"""
from __future__ import annotations

import io
from typing import Any

import numpy as np
from PIL import Image, ImageEnhance

from app.quantizer import (
    fit_to_panel,
    _PIL_DITHER_MAP,
    _palette_image,
    _error_diffusion,
    _dither_ordered,
    _ATKINSON_WEIGHTS,
    _JJN_WEIGHTS,
    _STUCKI_WEIGHTS,
    _BAYER_8X8,
    _HALFTONE_16,
    _CROSSHATCH_8,
)
from app.state.page_store import Panel

# Index order MUST match the firmware palette (board.h / fb2bpp / engine).
BWRY_PALETTE: tuple[tuple[int, int, int], ...] = (
    (0, 0, 0),        # 0 Black
    (255, 255, 255),  # 1 White
    (255, 255, 0),    # 2 Yellow
    (255, 0, 0),      # 3 Red
)

DEFAULTS: dict[str, Any] = {"dither": "floyd-steinberg", "saturation": 1.0, "contrast": 1.0}


def _native_dims(panel: Panel) -> tuple[int, int]:
    if panel.native_w is not None and panel.native_h is not None:
        return (panel.native_w, panel.native_h)
    return (panel.w, panel.h)


def _setting(settings: dict[str, Any], key: str) -> Any:
    return settings.get(key, DEFAULTS[key])


def _dither_to_indices(rgb: Image.Image, dither: str) -> bytes:
    """Return one palette-index byte per pixel (0..3), using the same dither
    dispatch as app.quantizer.pack_to_panel_bin."""
    pal_arr = np.array(BWRY_PALETTE, dtype=np.float32)
    if dither in _PIL_DITHER_MAP:  # floyd-steinberg, none (Pillow C path)
        return rgb.quantize(palette=_palette_image(BWRY_PALETTE),
                            dither=_PIL_DITHER_MAP[dither]).tobytes()
    if dither == "atkinson":
        return _error_diffusion(rgb, pal_arr, _ATKINSON_WEIGHTS)
    if dither == "jarvis":
        return _error_diffusion(rgb, pal_arr, _JJN_WEIGHTS)
    if dither == "stucki":
        return _error_diffusion(rgb, pal_arr, _STUCKI_WEIGHTS)
    if dither == "bayer-8x8":
        return _dither_ordered(rgb, pal_arr, _BAYER_8X8)
    if dither == "halftone":
        return _dither_ordered(rgb, pal_arr, _HALFTONE_16, strength=128.0)
    if dither == "crosshatch":
        return _dither_ordered(rgb, pal_arr, _CROSSHATCH_8, strength=96.0)
    raise ValueError(f"unknown dither mode: {dither!r}")


def transform(png_bytes: bytes, *, panel: Panel, settings: dict[str, Any]) -> bytes:
    img = Image.open(io.BytesIO(png_bytes)).convert("RGB")
    w, h = _native_dims(panel)
    if img.size != (w, h):
        fit = str(settings.get("image_fit") or "fit")
        img = fit_to_panel(img, target_w=w, target_h=h, scale=fit, bg="white")

    sat = float(_setting(settings, "saturation"))
    con = float(_setting(settings, "contrast"))
    if sat != 1.0:
        img = ImageEnhance.Color(img).enhance(sat)
    if con != 1.0:
        img = ImageEnhance.Contrast(img).enhance(con)

    raw = _dither_to_indices(img, str(_setting(settings, "dither")))
    idx = np.frombuffer(raw, dtype=np.uint8)
    if idx.size != w * h:
        raise ValueError(f"dither produced {idx.size} px, expected {w*h}")
    if w % 4 != 0:
        raise ValueError(f"panel width {w} must be a multiple of 4 for 2bpp packing")

    # The panel scans bottom-to-top: flip rows before packing. Matches PicPak
    # Studio's hardware-verified pipeline (flipIndicesVertical -> packTo2bpp);
    # without it a real image paints upside down.
    plane = idx.reshape(h, w)[::-1, :]
    groups = plane.reshape(h, w // 4, 4)         # 4 pixels per output byte
    packed = (groups[:, :, 0] << 6) | (groups[:, :, 1] << 4) \
           | (groups[:, :, 2] << 2) | groups[:, :, 3]
    out = packed.astype(np.uint8).tobytes()
    expected = w * h // 4
    if len(out) != expected:
        raise ValueError(f"packed {len(out)} bytes, expected {expected}")
    return out


def payload(digest: str, base_url: str, *, settings: dict[str, Any]) -> dict[str, Any]:
    del settings  # not part of the on-the-wire payload
    return {"url": f"{base_url.rstrip('/')}/renders/{digest}.bin"}
