#!/usr/bin/env bash
# Build OpenSBI fw_dynamic.bin for the LicheeRV Nano (SG2002 / C906), idempotent.
#   - mainline OpenSBI at a pinned tag -> third_party/opensbi (gitignored)
#   - PLATFORM=generic, FW_TEXT_START=0x80000000 (matches the vendor FSBL's monitor address)
#   - embeds boards/licheervnano/sg2002-licheervnano.dts via FW_FDT_PATH; the FSBL passes
#     a1=0x80080000 (no DTB there), fw_base.S overrides a1 with the embedded DTB and
#     relocates it to the address the FSBL passed.
# Output: build/opensbi/fw_dynamic.{bin,elf}, build/opensbi/sg2002-licheervnano.dtb
# Env: CROSS_COMPILE (default riscv64-elf-), OPENSBI_TAG (default below), DTC (default dtc)
set -euo pipefail
cd "$(dirname "$0")/.."

OPENSBI_TAG="${OPENSBI_TAG:-v1.8.1}"
OPENSBI_REPO="${OPENSBI_REPO:-https://github.com/riscv-software-src/opensbi}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-elf-}"
DTC="${DTC:-dtc}"
SRC=third_party/opensbi
OUT=build/opensbi
DTS=boards/licheervnano/sg2002-licheervnano.dts

die() { echo "build-opensbi: error: $*" >&2; exit 1; }
command -v "${CROSS_COMPILE}gcc" >/dev/null || die "${CROSS_COMPILE}gcc not found (brew install riscv64-elf-gcc)"
command -v "$DTC" >/dev/null || die "dtc not found (brew install dtc)"

if [ ! -d "$SRC/.git" ]; then
    mkdir -p third_party
    git clone --quiet --depth 1 --branch "$OPENSBI_TAG" "$OPENSBI_REPO" "$SRC"
fi
if [ "$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || true)" != "$OPENSBI_TAG" ]; then
    git -C "$SRC" fetch --quiet --depth 1 origin tag "$OPENSBI_TAG"
    git -C "$SRC" checkout --quiet --force "$OPENSBI_TAG"
fi

mkdir -p "$OUT" "$OUT/obj"   # obj must exist: BSD readlink -f needs an existing path
DTB="$PWD/$OUT/sg2002-licheervnano.dtb"
"$DTC" -I dts -O dtb -@ -o "$DTB" "$DTS" 2>/dev/null || "$DTC" -I dts -O dtb -o "$DTB" "$DTS"

# Bare-metal riscv64-elf binutils (Homebrew) cannot link -pie, which OpenSBI's Makefile
# demands. fw_dynamic is loaded at its link address (0x80000000, FW_TEXT_START), so
# self-relocation is unnecessary (fw_base.S skips it when .rela.dyn is empty). If the
# toolchain lacks -pie, build through a shim dir whose gcc drops -pie and whose other
# tools are symlinks to the real ones. Toolchains that do support -pie are used directly.
TOOLCHAIN_PREFIX="$CROSS_COMPILE"
if ! "${CROSS_COMPILE}gcc" -fPIE -nostdlib -Wl,-pie -x c /dev/null -o /dev/null >/dev/null 2>&1; then
    REAL_GCC="$(command -v "${CROSS_COMPILE}gcc")"
    SHIM="$PWD/$OUT/toolshim"
    rm -rf "$SHIM"; mkdir -p "$SHIM"
    realdir="$(dirname "$REAL_GCC")"; pfx="$(basename "$CROSS_COMPILE")"
    for t in "$realdir/$pfx"*; do
        case "$(basename "$t")" in "${pfx}gcc") ;; *) ln -s "$t" "$SHIM/$(basename "$t")" ;; esac
    done
    cat > "$SHIM/${pfx}gcc" <<SH
#!/bin/sh
for a in "\$@"; do shift; case "\$a" in -pie|-Wl,-pie) ;; *) set -- "\$@" "\$a" ;; esac; done
exec "$REAL_GCC" "\$@"
SH
    chmod +x "$SHIM/${pfx}gcc"
    CROSS_COMPILE="$SHIM/$pfx"
    echo "build-opensbi: toolchain lacks -pie; using shim $SHIM (static link at 0x80000000)"
fi

# FW_FDT_PADDING leaves room for OpenSBI's in-place FDT fixups (reserved-memory, chosen).
# Build dir lives inside $OUT so the OpenSBI tree stays clean; make handles incrementality.
make -C "$SRC" READLINK=readlink O="$PWD/$OUT/obj" PLATFORM=generic CROSS_COMPILE="$CROSS_COMPILE" \
    FW_TEXT_START=0x80000000 FW_FDT_PATH="$DTB" FW_FDT_PADDING=0x1000 \
    PLATFORM_RISCV_XLEN=64 PLATFORM_RISCV_ISA=rv64imafdc_zicsr_zifencei PLATFORM_RISCV_ABI=lp64d \
    FW_JUMP=n FW_PAYLOAD=n FW_DYNAMIC=y

F="$OUT/obj/platform/generic/firmware"
cp "$F/fw_dynamic.bin" "$OUT/fw_dynamic.bin"
cp "$F/fw_dynamic.elf" "$OUT/fw_dynamic.elf"
echo "Wrote $OUT/fw_dynamic.bin ($(wc -c < "$OUT/fw_dynamic.bin" | tr -d ' ') bytes), OpenSBI $OPENSBI_TAG"
