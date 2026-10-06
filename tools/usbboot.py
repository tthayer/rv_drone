#!/usr/bin/env python3
"""Boot fip.bin on an SG2002 over USB (mask-ROM download mode, RAM only).

Protocol (from Sophgo's cv181x_rom_usb_download.py):
  - Each packet is [token u8, len u16 BE, addr 5 bytes BE] + data, where len
    includes the 8-byte header.
  - Data packets are acked with 16 bytes. Bytes [2:4] hold the CRC16-CCITT
    (XModem) of the packet; [8:12] and [12:16] hold the next fip offset and
    size, both BE.
  - ROM stage: send the magic, send fip[0:4K] to address 0, set the "1NGM"
    flag at 0x0E000004, then BREAK. The device re-enumerates.
  - FSBL stage, repeated until the device no longer reappears: send the
    magic; its ack names the fip window to send next. Send that window, then
    the flag, then BREAK.

Differences from the vendor script:
  - Every ack has a timeout, and a lost packet is resent. Packets carry their
    RAM address, so resending is idempotent.
  - The packet size is configurable.
  - It never issues PROGRAM commands, so nothing is written to flash or SD.
"""
import argparse
import binascii
import struct
import sys
import time

import serial
import serial.tools.list_ports

VIDPID = (0x3346, 0x1000)
TOK_DATA, TOK_FLAG, TOK_BREAK, TOK_KEEP_DL = 0, 1, 2, 3
DUMMY_ADDR = 0xFF
FLAG_ADDR = 0x0E000004


def log(*a):
    print(*a, flush=True)


def find_port(timeout):
    end = time.time() + timeout
    while time.time() < end:
        for p in serial.tools.list_ports.comports():
            if (p.vid, p.pid) == VIDPID:
                return p.device
        time.sleep(0.05)
    return None


def open_port(dev):
    time.sleep(0.1)
    s = serial.Serial(dev, 115200, timeout=0.5, write_timeout=1)
    s.reset_input_buffer()
    return s


def packet(token, addr, data=b"", length=None):
    n = 8 + len(data) if length is None else length
    return bytes([token, n >> 8, n & 0xFF]) + addr.to_bytes(5, "big") + data


def send_acked(s, pkt, retries):
    want = binascii.crc_hqx(pkt, 0)
    for attempt in range(retries + 1):
        if attempt:
            s.reset_input_buffer()
        s.write(pkt)
        s.flush()
        ack = s.read(16)
        if len(ack) == 16 and (ack[2] << 8 | ack[3]) == want:
            off, size = struct.unpack(">II", ack[8:16])
            return off, size
    raise RuntimeError(f"no valid ack after {retries + 1} tries (last {ack.hex()})")


def send_data(s, data, addr, chunk, retries):
    step = chunk - 8
    for i in range(0, len(data), step):
        send_acked(s, packet(TOK_DATA, addr + i, data[i:i + step]), retries)
        sys.stdout.write(f"\r  {min(i + step, len(data))}/{len(data)} B")
        sys.stdout.flush()
    sys.stdout.write("\n")


def finish_stage(s):
    s.write(packet(TOK_FLAG, FLAG_ADDR, b"1NGM", length=12))
    s.write(packet(TOK_BREAK, DUMMY_ADDR))
    s.flush()
    s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("fip", help="fip.bin to boot")
    ap.add_argument("--magic", required=True, help="vendor cv_dl_magic.bin (128 B)")
    ap.add_argument("--chunk", type=int, default=256, help="packet size incl. 8 B header")
    ap.add_argument("--retries", type=int, default=5)
    ap.add_argument("--wait", type=float, default=120, help="seconds to wait for the ROM")
    a = ap.parse_args()

    fip = open(a.fip, "rb").read()
    magic = open(a.magic, "rb").read()

    log(f"waiting for {VIDPID[0]:04x}:{VIDPID[1]:04x} (power the board without a bootable SD)...")
    wait, stage, drops = a.wait, 1, 0
    while True:
        dev = find_port(wait)
        if not dev:
            if stage == 1:
                sys.exit("ROM download device not found")
            log("device gone: image is running")
            return
        wait = 3
        time.sleep(1.0)                             # ROM re-enumerates once on attach
        try:
            s = open_port(dev)
            off, size = send_acked(s, packet(TOK_KEEP_DL, DUMMY_ADDR, magic), a.retries)
            if size == 0:                           # ROM: wants the fip header
                off, size = 0, 4096
            log(f"stage {stage}: fip[{off:#x}:+{size:#x}]")
            if off + size > len(fip):
                sys.exit(f"bad window {off:#x}+{size:#x} for {len(fip)} B fip")
            send_data(s, fip[off:off + size], 0, a.chunk, a.retries)
            finish_stage(s)
            stage += 1
        except (serial.SerialException, OSError, RuntimeError) as e:
            drops += 1
            log(f"\nstage {stage}: {e}; reconnecting ({drops})")
            if drops > 10:
                sys.exit("too many USB drops")
        time.sleep(0.3)                             # old port must vanish first


if __name__ == "__main__":
    main()
