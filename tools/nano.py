#!/usr/bin/env python3
"""rv_drone Nano helper: flash firmware over UART0 and read the console.

  tools/nano.py flash   [--port P] [--retries N] [--wait S] FIP   flash in place
  tools/nano.py console [--port P] [--secs S]                     print the console
  tools/nano.py wait    [--port P] [--secs S]                     wait for the board

The port comes from --port, else $UPLOAD_PORT, else $RESET_PORT, else the first
USB-serial device found.

`flash` writes `FIP` over the console 'u' command: the firmware stages it as
/fip.new, verifies size + CRC-32, swaps /fip.bin and reboots (see
src/app/firmware_update.c). The transfer has no flow control, so a slow card
write can drop a byte and abort it — safe (the old /fip.bin is untouched until
the staged copy verifies) — so this retries, and waits out the reboot between
attempts.
"""
import argparse
import glob
import os
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


def resolve_port(p):
    if p:
        return p
    for env in ("UPLOAD_PORT", "RESET_PORT"):
        if os.environ.get(env):
            return os.environ[env]
    for pat in ("/dev/cu.usbserial-*", "/dev/ttyUSB*", "/dev/ttyACM*", "/dev/cu.usbmodem*"):
        g = sorted(glob.glob(pat))
        if g:
            return g[0]
    return None


class Link:
    def __init__(self, port, baud=115200):
        self.ser = serial.Serial(port, baud, timeout=0.05, write_timeout=5)
        self.rx = bytearray()

    def set_baud(self, baud):
        # No buffer flush here: going back to the console baud must not discard
        # the firmware's "fwup: done". The fast switch flushes explicitly.
        if baud != self.ser.baudrate:
            self.ser.baudrate = baud

    def pump(self):
        n = self.ser.in_waiting or 1
        data = self.ser.read(n)
        if data:
            self.rx.extend(data)
        return data

    def wait_for(self, token, timeout, fail_on=FAIL):
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
    # Pause so the Nano can open /fip.new before the body streams; otherwise the
    # first SD write stalls long enough to overrun its RX ring.
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


def attempt(lk, image, args):
    """One upload attempt. ('ok', None), ('retry', why) or ('fatal', why)."""
    lk.rx.clear()
    lk.set_baud(115200)
    lk.ser.reset_input_buffer()
    lk.ser.write(b"u")
    lk.ser.flush()
    if not lk.wait_for(READY, timeout=10):
        if FAIL in lk.rx:
            return "retry", "Nano refused the update: " + lk.tail()
        if b"rx: 'u'" in lk.rx:
            return "fatal", ("the running image has no 'u' command (it echoed the "
                             "byte); boot it once over SD/USB, then update")
        return "retry", "no 'fwup: ready' (board still booting?)"
    m = READY_BAUD.search(bytes(lk.rx))
    fast = int(m.group(1)) if m else 115200
    if fast != 115200:
        lk.ser.reset_input_buffer()            # drop 115200 stragglers, not the result
        try:
            lk.set_baud(fast)
        except (ValueError, OSError) as e:
            return "fatal", f"adapter cannot use {fast} baud ({e})"
    log(f"sending image at {fast} baud...")
    send_image(lk, image)
    lk.set_baud(115200)                        # the result comes back here
    if lk.wait_for(DONE, timeout=args.timeout):
        return "ok", None
    if FAIL in lk.rx:
        return "retry", "update failed: " + lk.tail()
    return "retry", "timed out waiting for 'fwup: done': " + lk.tail()


def wait_alive(lk, secs):
    """Block until the console prints anything (the board runs), or `secs`."""
    lk.rx.clear()
    end = time.time() + secs
    got = 0
    while time.time() < end:
        got += len(lk.pump())
        if got:
            return True
    return False


def cmd_flash(args):
    image = open(args.fip, "rb").read()
    if not image:
        sys.exit(f"{args.fip} is empty")
    lk = Link(args.port)
    log(f"upload: {args.fip} ({len(image)} B, crc {zlib.crc32(image) & 0xFFFFFFFF:08x}) -> {args.port}")
    if not wait_alive(lk, args.wait):
        sys.exit(f"no console output on {args.port} in {args.wait}s; is the Nano "
                 f"powered and booted? (a stuck board needs a power-cycle)")
    for i in range(1, args.retries + 1):
        status, msg = attempt(lk, image, args)
        if status == "ok":
            log("flash: /fip.bin replaced; board is rebooting.")
            if wait_alive(lk, 15):
                log("board is up.")
            return
        if status == "fatal":
            sys.exit(msg)
        log(f"attempt {i}/{args.retries} failed: {msg}")
        if i < args.retries:
            if wait_alive(lk, 15):
                log("board rebooted; retrying.")
    sys.exit(f"upload failed after {args.retries} attempts")


def cmd_console(args):
    lk = Link(args.port)
    end = time.time() + args.secs
    buf = bytearray()
    while time.time() < end:
        buf += lk.pump()
    sys.stdout.write(bytes(buf).decode("latin1", "replace").replace("\r", ""))
    lk.ser.close()


def cmd_wait(args):
    lk = Link(args.port)
    if wait_alive(lk, args.secs):
        sys.stdout.write(bytes(lk.rx).decode("latin1", "replace").replace("\r", ""))
        lk.ser.close()
        return
    lk.ser.close()
    sys.exit(f"no console output on {args.port} in {args.secs}s")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--port", help="UART0 device (default: auto-detect)")

    f = sub.add_parser("flash", help="upload fip.bin in place and reboot")
    common(f)
    f.add_argument("fip", help="fip.bin to upload")
    f.add_argument("--timeout", type=float, default=30.0, help="seconds to wait for the result")
    f.add_argument("--retries", type=int, default=3, help="attempts before giving up")
    f.add_argument("--wait", type=float, default=30.0, help="seconds to wait for the board at start")
    f.set_defaults(func=cmd_flash)

    c = sub.add_parser("console", help="print the console for --secs")
    common(c)
    c.add_argument("--secs", type=float, default=3.0)
    c.set_defaults(func=cmd_console)

    w = sub.add_parser("wait", help="wait until the board prints")
    common(w)
    w.add_argument("--secs", type=float, default=30.0)
    w.set_defaults(func=cmd_wait)

    a = ap.parse_args()
    a.port = resolve_port(a.port)
    if not a.port:
        sys.exit("no serial port found; pass --port or set UPLOAD_PORT")
    a.func(a)


if __name__ == "__main__":
    main()
