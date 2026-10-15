#!/usr/bin/env python3
"""Push a firmware image (fip.bin) to a running rv_drone Nano over UART0.

The SD card stays in the Nano: the firmware receives the image, stages it as
/fip.new on the card, verifies its size and CRC-32, replaces /fip.bin and
reboots into it.

The transfer interrupts audio on purpose and runs at a high baud for speed.
Protocol (src/app/firmware_update.c):
  - host sends 'u' at the console baud;
  - Nano prints "fwup: ready baud <N>" (at the console baud), then switches
    UART0 to N;
  - host switches to N as well, then sends "RVFW", u32 size (LE),
    u32 crc32 (LE, zlib), then the image;
  - Nano prints "fwup: done" (then reboots) or "fwup: fail <reason>".

The running firmware must include this command. Older images do not: there the
'u' is echoed as `rx: 'u' ...` and this script times out. Boot such an image
once over SD or USB and update it once the new way.
"""
import argparse
import re
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:                                        # pragma: no cover
    sys.exit("pyserial is required (pip install pyserial)")

MAGIC = b"RVFW"
READY = b"fwup: ready"
DONE = b"fwup: done"
FAIL = b"fwup: fail"
READY_BAUD = re.compile(rb"fwup: ready baud (\d+)")


def log(*a):
    print(*a, file=sys.stderr, flush=True)


class Link:
    def __init__(self, port, baud):
        self.console_baud = baud
        self.ser = serial.Serial(port, baud, timeout=0.05, write_timeout=5)
        self.rx = bytearray()

    def set_baud(self, baud):
        if baud != self.ser.baudrate:
            self.ser.baudrate = baud
            self.ser.reset_input_buffer()

    def pump(self):
        n = self.ser.in_waiting or 1
        data = self.ser.read(n)
        if data:
            self.rx.extend(data)
        return data

    def wait_for(self, token, timeout, fail_on=FAIL):
        """Pump until token (or fail_on) appears. True only if token is present."""
        end = time.time() + timeout
        while time.time() < end:
            self.pump()
            if token in self.rx:
                return True
            if token != fail_on and fail_on in self.rx:
                return False
        return token in self.rx

    def tail(self):
        return bytes(self.rx[-400:]).decode("latin1", "replace").strip()


def send_image(lk, image, chunk=512):
    lk.ser.write(MAGIC)
    lk.ser.write(struct.pack("<II", len(image), zlib.crc32(image) & 0xFFFFFFFF))
    lk.ser.flush()
    # Pause so the Nano can open /fip.new on the card before the body streams;
    # otherwise the first SD write stalls long enough to overrun its RX buffer.
    time.sleep(0.5)
    last = -1
    for i in range(0, len(image), chunk):
        lk.ser.write(image[i:i + chunk])
        lk.ser.flush()
        sent = min(i + chunk, len(image))
        pct = sent * 100 // len(image)
        if pct != last:
            sys.stderr.write(f"\r  {sent}/{len(image)} B ({pct}%)")
            sys.stderr.flush()
            last = pct
    sys.stderr.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("fip", help="fip.bin to upload")
    ap.add_argument("--port", required=True,
                    help="UART0 serial device (e.g. /dev/cu.usbserial-XXXX)")
    ap.add_argument("--baud", type=int, default=115200,
                    help="console baud before the switch (default 115200)")
    ap.add_argument("--timeout", type=float, default=30.0,
                    help="seconds to wait for the final result (default 30)")
    a = ap.parse_args()

    image = open(a.fip, "rb").read()
    if not image:
        sys.exit(f"{a.fip} is empty")

    lk = Link(a.port, a.baud)
    lk.ser.reset_input_buffer()

    log(f"upload: {a.fip} ({len(image)} B, crc {zlib.crc32(image) & 0xFFFFFFFF:08x}) -> {a.port}")
    lk.ser.write(b"u")
    lk.ser.flush()

    if not lk.wait_for(READY, timeout=10):
        if FAIL in lk.rx:
            sys.exit("Nano refused the update: " + lk.tail())
        sys.exit("no 'fwup: ready' from the Nano. Does the running firmware have the "
                 "'u' command? (an echoed 'u' means an older image; boot it once over "
                 "SD/USB and update it the new way)")

    m = READY_BAUD.search(bytes(lk.rx))
    fast = int(m.group(1)) if m else a.baud
    if fast != a.baud:
        log(f"firmware switched to {fast} baud; following.")
        try:
            lk.set_baud(fast)
        except (ValueError, OSError) as e:
            sys.exit(f"this adapter/OS cannot use {fast} baud ({e}). Rebuild the "
                     f"firmware with -DFW_UPLOAD_DIV=2 (781250 baud) and retry.")

    log(f"sending image at {fast} baud...")
    send_image(lk, image)

    # The Nano reports the result at the console baud (the upload baud's return
    # path is not reliable on every adapter), so read it back there.
    lk.set_baud(a.baud)
    if not lk.wait_for(DONE, timeout=a.timeout):
        if FAIL in lk.rx:
            sys.exit("update failed: " + lk.tail())
        sys.exit("timed out waiting for 'fwup: done': " + lk.tail())

    log("flash: /fip.bin replaced; board is rebooting.")
    try:
        lk.ser.close()
    except Exception:
        pass


if __name__ == "__main__":
    main()
