#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 varanu5 <https://github.com/varanu5>
"""Build without private defaults and record which artifacts were checked."""
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"
BUILD = FIRMWARE / "build"
HEADER = FIRMWARE / "main/secrets.h"


def run():
    if not shutil.which("idf.py"):
        raise SystemExit("Activate ESP-IDF before running this tool")
    with tempfile.TemporaryDirectory(prefix="picpak-private-") as private:
        saved = Path(private) / "secrets.h"
        original = hashlib.sha256(HEADER.read_bytes()).digest() if HEADER.exists() else None
        try:
            if original is not None:
                shutil.move(HEADER, saved)
            assert not HEADER.exists()
            subprocess.run(["idf.py", "fullclean", "build"], cwd=FIRMWARE, check=True)
            deps = subprocess.check_output(["ninja", "-C", str(BUILD), "-t", "deps"], text=True)
            assert not any(Path(line.strip()).name == "secrets.h" for line in deps.splitlines())
            entry = next(e for e in json.loads((BUILD / "compile_commands.json").read_text())
                         if e["file"].endswith("/config_store.c"))
            args = iter(shlex.split(entry["command"]))
            filtered = []
            for arg in args:
                if arg in ("-o", "-MF", "-MT", "-MQ"):
                    next(args)
                elif arg not in ("-c", "-MD", "-MMD"):
                    filtered.append(arg)
            macros = subprocess.check_output(filtered + ["-E", "-dM"],
                                             cwd=entry["directory"], text=True)
            values = {}
            for line in macros.splitlines():
                parts = line.split(None, 2)
                if len(parts) == 3 and parts[0] == "#define":
                    values[parts[1]] = parts[2]
            for key in ("WIFI_DEFAULT_SSID", "WIFI_DEFAULT_PASS", "REST_DEFAULT_SERVER_URL",
                        "MQTT_DEFAULT_URI", "MQTT_DEFAULT_USER", "MQTT_DEFAULT_PASS"):
                assert values[key] == '""', key + " must be empty in public builds"
            files = ("bootloader/bootloader.bin", "partition_table/partition-table.bin",
                     "picpak-tesserae-client.bin", "ota_data_initial.bin")
            proof = {"sha256": {name: hashlib.sha256((BUILD / name).read_bytes()).hexdigest()
                                 for name in files}}
            (BUILD / "public-build.json").write_text(json.dumps(proof, indent=2) + "\n")
            print("Public build checked: no private header and empty connection defaults.")
        finally:
            if original is not None:
                assert not HEADER.exists(), "Private header changed during compilation"
                shutil.move(saved, HEADER)
                assert hashlib.sha256(HEADER.read_bytes()).digest() == original
                print("Private header restored unchanged.")
    subprocess.run(["python3", str(ROOT / "tools/package_firmware.py")], check=True)


if __name__ == "__main__":
    run()
