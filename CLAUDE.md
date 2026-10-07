# rv_drone: working rules for Claude sessions

## Keep the docs and wiring diagrams current
Whenever a change affects them, update the documentation **in the same commit**:

- `docs/ENGINE.md`: engine behaviour, the parameter table (ranges, defaults, CCs), the clock follower, CPU and memory.
- `docs/ARCHITECTURE.md`: pin tables, pinmux, runtime model, protocols, milestone status.
- `README.md`: build, flash and wiring instructions.
- `docs/wiring/system.yml` and `docs/wiring/panel.yml` (WireViz): any change to pins,
  wires, power, components or modules. Re-render with `make wiring`, look at the PNG
  to check it, and commit the `.yml`, `.svg` and `.png` together.

**Always regenerate the WireViz artifacts when the wiring changes.** The repo's
pre-commit hook (`.githooks/pre-commit`, enabled with
`git config core.hooksPath .githooks`) re-renders and stages them whenever a
`docs/wiring/*.yml` is committed. Still look at the rendered PNG before committing.

Triggers include pin or wiring changes, power changes, new or removed parts,
protocol or packet changes, console commands, UI controls, and build or tool steps.
The pin tables in `docs/ARCHITECTURE.md` are the source of truth; the diagrams must agree with them.

## Dev loop
- Nano: `make usbboot RESET_PORT=/dev/cu.usbserial-A4008UMc` (the SD card has no fip.bin,
  so the ROM falls through to USB). Console 115200 on that port.
- Pico A: `make pico-flash-audio PICO_A_SER=0608CFFAD05BCF10`
- Pico B: `make pico-flash-panel PICO_B_SER=53ADB4FD5CB7055B`
- Host tests: `make test-link test-panel test-presets`. Emulator: `make emu` and `make emu-wav`.
