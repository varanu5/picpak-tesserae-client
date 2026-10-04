#!/bin/bash
set -eo pipefail
cd "$(dirname "$0")/.."
. "$HOME/esp/esp-idf/export.sh"
python3 tools/build_public.py
