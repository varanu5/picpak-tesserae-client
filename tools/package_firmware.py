#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 varanu5 <https://github.com/varanu5>
"""Package a verified public build as individually addressed flash images."""
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "firmware/build"
OUTPUT = ROOT / "firmware/release"
LEGACY_TABLE_SHA256 = "c73f050a37cc44dc94309f32e6cddfd2290bdf609259c95d26664122f8ad35aa"


def package():
    proof = json.loads((BUILD / "public-build.json").read_text())
    version = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"',
                        (ROOT / "firmware/main/defaults.h").read_text()).group(1)
    specs = [
        ("app", "picpak-tesserae-client.bin", 0x20000, 0x400000),
        ("bootloader", "bootloader/bootloader.bin", 0, 0x8000),
        ("partitions", "partition_table/partition-table.bin", 0x8000, 0x1000),
        ("nvs", None, 0x9000, 0x6000),
        ("inactive", None, 0x420000, 0x1000),
        ("otadata", "ota_data_initial.bin", 0x10000, 0x2000),
    ]
    expected = [
        (1, 2, 0x9000, 0x6000, "nvs"), (1, 1, 0xf000, 0x1000, "phy_init"),
        (1, 0, 0x10000, 0x2000, "otadata"), (0, 0x10, 0x20000, 0x400000, "ota_0"),
        (0, 0x11, 0x420000, 0x400000, "ota_1"), (1, 3, 0x820000, 0x10000, "coredump"),
        (1, 0x40, 0x830000, 0x7d0000, "framestore"),
    ]
    table = (BUILD / "partition_table/partition-table.bin").read_bytes()
    assert len(table) == 0xc00
    for index, row in enumerate(expected):
        magic, kind, subtype, offset, size, label, flags = struct.unpack_from("<HBBII16sI", table, index * 32)
        assert magic == 0x50aa and flags == 0
        assert (kind, subtype, offset, size, label.rstrip(b"\0").decode()) == row
    end = len(expected) * 32
    assert table[end:end + 16] == b"\xeb\xeb" + b"\xff" * 14
    assert table[end + 16:end + 32] == hashlib.md5(table[:end]).digest()
    assert all(b == 0xff for b in table[end + 32:])
    parts = []
    images = {}
    for role, source, address, limit in specs:
        data = (BUILD / source).read_bytes() if source else b"\xff" * limit
        assert 0 < len(data) <= limit and len(data) % 4 == 0, role
        if role in ("nvs", "inactive", "otadata"):
            assert data == b"\xff" * limit, role
        sha = hashlib.sha256(data).hexdigest()
        if source:
            assert proof["sha256"][source] == sha, "Rebuild public artifacts before packaging"
        if role == "app":
            assert data[0] == 0xE9 and int.from_bytes(data[12:14], "little") == 5
            assert data[32:36] == bytes.fromhex("3254cdab")
            assert data[48:80].split(b"\0")[0].decode() == version
            assert data[80:112].split(b"\0")[0] == b"picpak-tesserae-client"
        name = Path(source).name if source else ("nvs_blank.bin" if role == "nvs" else "ota_1_blank.bin")
        images[name] = data
        parts.append(dict(role=role, file=name, address=address, size=len(data),
                          sha256=sha, md5=hashlib.md5(data).hexdigest()))
    manifest = dict(schema=1, version=version, chip="ESP32-C3", layout="picpak-ab-4m-v1",
                    flashSize=0x1000000, legacyTableSha256=LEGACY_TABLE_SHA256, parts=parts)
    OUTPUT.mkdir(exist_ok=True)
    for name, data in images.items():
        (OUTPUT / name).write_bytes(data)
    (OUTPUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (OUTPUT / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in images.items()))
    print(f"Packaged firmware {version}: {OUTPUT}")


if __name__ == "__main__":
    package()
