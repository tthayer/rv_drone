#!/usr/bin/env bash
# Fetch the vendor files the Nano build needs into third_party/ (gitignored):
#   - sophgo/fiptool: the packer, plus the SG2002 FSBL (cv181x.bin) and ddr_param.bin
#   - cv_dl_magic.bin: the mask-ROM USB download magic (milkv-duo/duo-buildroot-sdk)
# Idempotent.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party/usb_dl

[ -d third_party/fiptool ] || git clone --depth 1 https://github.com/sophgo/fiptool third_party/fiptool

M=third_party/usb_dl/cv_dl_magic.bin
[ -s "$M" ] || curl -sfL -o "$M" \
    https://raw.githubusercontent.com/milkv-duo/duo-buildroot-sdk/develop/build/tools/cv181x/usb_dl/rom_usb_dl/cv_dl_magic.bin

ls -l third_party/fiptool/data/fsbl/cv181x.bin third_party/fiptool/data/ddr_param.bin "$M"
