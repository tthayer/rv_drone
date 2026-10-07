#!/usr/bin/env bash
# Fetch the vendor files the Nano build needs into third_party/ (gitignored):
#   - sophgo/fiptool: the packer, plus the SG2002 FSBL (cv181x.bin) and ddr_param.bin
#   - cv_dl_magic.bin: the mask-ROM USB download magic (milkv-duo/duo-buildroot-sdk)
#   - FatFs R0.15a (ChaN): ff.c/ff.h/diskio.h into third_party/fatfs (our ffconf.h is in src/fs)
# Idempotent.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party/usb_dl

[ -d third_party/fiptool ] || git clone --depth 1 https://github.com/sophgo/fiptool third_party/fiptool

M=third_party/usb_dl/cv_dl_magic.bin
[ -s "$M" ] || curl -sfL -o "$M" \
    https://raw.githubusercontent.com/milkv-duo/duo-buildroot-sdk/develop/build/tools/cv181x/usb_dl/rom_usb_dl/cv_dl_magic.bin

F=third_party/fatfs
if [ ! -s "$F/ff.c" ]; then
    tmp=$(mktemp -d)
    curl -sfL -o "$tmp/ff15a.zip" http://elm-chan.org/fsw/ff/arc/ff15a.zip
    echo "74737b1cafa1a67a3f722dd0d1a44767c5b54d37b6300ad3825a904cbe88fc3c  $tmp/ff15a.zip" | shasum -a 256 -c -
    unzip -q "$tmp/ff15a.zip" -d "$tmp"
    mkdir -p "$F"
    cp "$tmp"/source/{ff.c,ff.h,diskio.h} "$tmp/LICENSE.txt" "$F/"
    rm -rf "$tmp"
fi

ls -l third_party/fiptool/data/fsbl/cv181x.bin third_party/fiptool/data/ddr_param.bin "$M" "$F/ff.c"
