#!/usr/bin/env bash
# Pack build/nano/rv_drone.bin into fip.bin using sophgo/fiptool.
# Required env: FIPTOOL (clone dir), FSBL_BIN, DDR_PARAM_BIN.
# OPENSBI_BIN: set by `make fip` to build/opensbi/fw_dynamic.bin (tools/build-opensbi.sh) unless overridden.
# Optional: RTOS_BIN (default $FIPTOOL/data/cvirtos.bin; fiptool needs one).
set -euo pipefail
cd "$(dirname "$0")/.."

die() { echo "mkfip: error: $*" >&2; exit 1; }
need() { [ -n "${!1:-}" ] || die "$1 is not set ($2)"; [ -f "${!1}" ] || die "$1=${!1} does not exist"; }

[ -n "${FIPTOOL:-}" ] || die "FIPTOOL is not set (path to sophgo/fiptool clone)"
[ -f "$FIPTOOL/fiptool" ] || die "$FIPTOOL/fiptool not found"
need FSBL_BIN "vendor FSBL, e.g. \$FIPTOOL/data/fsbl/cv181x.bin for SG2002"
need DDR_PARAM_BIN "e.g. \$FIPTOOL/data/ddr_param.bin"
need OPENSBI_BIN "OpenSBI fw_dynamic.bin; run 'make opensbi' (default build/opensbi/fw_dynamic.bin)"
RTOS_BIN="${RTOS_BIN:-$FIPTOOL/data/cvirtos.bin}"
[ -f "$RTOS_BIN" ] || die "RTOS_BIN=$RTOS_BIN does not exist"
IMG=build/nano/rv_drone.bin
[ -f "$IMG" ] || die "$IMG missing; run 'make BOARD=nano' first"

OUT=build/nano/fip.bin
python3 "$FIPTOOL/fiptool" --fsbl "$FSBL_BIN" --ddr_param "$DDR_PARAM_BIN" \
    --opensbi "$OPENSBI_BIN" --rtos "$RTOS_BIN" --uboot "$IMG" "$OUT"

cat <<MSG

Wrote $OUT
Copy to the FAT (boot) partition of the SD card, as fip.bin:
  cp $OUT /Volumes/<BOOT>/fip.bin && diskutil eject /Volumes/<BOOT>
Then insert the card and watch UART0 (115200 8N1; A16 TX / A17 RX).
MSG
