# rv_drone

Bare-metal drone synthesizer for the Sipeed LicheeRV Nano (SG2002, C906 RV64GC).
Runs in S-mode under the vendor OpenSBI. Design and milestones: `docs/ARCHITECTURE.md`.
Current state: M0 (boot, polled UART hello + heartbeat).

## Toolchain

    brew install riscv64-elf-gcc qemu dtc

## Build

    make BOARD=nano     # default; build/nano/rv_drone.{elf,bin,map,lst}
    make BOARD=qemu

## Run on QEMU

    make run-qemu       # Ctrl-A X to quit

## Boot on the Nano

Needs vendor blobs (not in this repo): sophgo/fiptool clone, its `data/fsbl/cv181x.bin`
and `data/ddr_param.bin` (SG2002), and an OpenSBI `fw_dynamic.bin`, which `make opensbi` builds for you (mainline OpenSBI
`v1.8.1` cloned to `third_party/`, with `boards/licheervnano/sg2002-licheervnano.dts` embedded;
output `build/opensbi/fw_dynamic.bin`; needs `git` and `dtc`, no GNU make required).

    export FIPTOOL=~/src/fiptool
    export FSBL_BIN=$FIPTOOL/data/fsbl/cv181x.bin
    export DDR_PARAM_BIN=$FIPTOOL/data/ddr_param.bin
    make fip            # builds OpenSBI if needed -> build/nano/fip.bin
    # optional: export OPENSBI_BIN=/path/to/fw_dynamic.bin to use your own

Copy `fip.bin` to the SD card's FAT partition, insert, and watch UART0 at 115200 8N1.
