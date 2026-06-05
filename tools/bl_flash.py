#!/usr/bin/env python3
"""
bl_flash.py — host-side firmware updater for the DCN-AVR-EA-Bootloader.

Drives the BLINFO → BLERASE → BLDATA(*N) → BLVERIFY → BLRESET sequence over
a serial port. Speaks the DCN frame format that rides on the existing
single-wire bus.

Usage:
    python tools/bl_flash.py --port COM5 --hex build/SPS-1.hex
    python tools/bl_flash.py --port /dev/ttyUSB0 --hex app.hex --addr BB --baud 9600

Before running:
    1. Set the target device's BCD address switch to the bootloader-trigger
       position (0xF on SPS-1).
    2. Power-cycle the device.
    3. Run this script.
    4. When it finishes successfully, set the switch back to the device's
       normal operating address and either let the BLRESET take effect or
       power-cycle again.

Requires:  pyserial  (pip install pyserial)
"""

from __future__ import annotations
import argparse
import sys
import time
from pathlib import Path

try:
    import serial   # type: ignore
except ImportError:
    print("error: this tool requires pyserial. Install with: pip install pyserial",
          file=sys.stderr)
    raise SystemExit(2)


# ---- minimal Intel HEX reader (mirrors the one in patch_app_crc.py) ----

def read_hex_sparse(path: Path) -> dict[int, int]:
    out: dict[int, int] = {}
    ext = 0
    with path.open("r", encoding="ascii") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line.startswith(":"):
                continue
            b = bytes.fromhex(line[1:])
            count, ah, al, rtype = b[0], b[1], b[2], b[3]
            payload = b[4:4 + count]
            checksum = b[4 + count]
            if (0x100 - (sum(b[:-1]) & 0xFF)) & 0xFF != checksum:
                raise ValueError(f"{path}:{lineno}: bad checksum")
            addr16 = (ah << 8) | al
            if rtype == 0x00:
                for i, v in enumerate(payload):
                    out[ext + addr16 + i] = v
            elif rtype == 0x01:
                break
            elif rtype == 0x04:
                ext = ((payload[0] << 8) | payload[1]) << 16
    return out


# ---- CRC-16/CCITT-FALSE ----

def crc16(buf: bytes, crc: int = 0xFFFF) -> int:
    for byte in buf:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


# ---- DCN frame I/O ----

class Bootloader:
    def __init__(self, port: serial.Serial, target_addr: int, host_addr: int = 0x00,
                 timeout: float = 2.0, verbose: bool = False) -> None:
        self.s = port
        self.target = target_addr & 0xFF
        self.host = host_addr & 0xFF
        self.s.timeout = timeout
        self.verbose = verbose

    def _send(self, body: str) -> None:
        # body is the bit between the start '/' and the trailing :XX\r
        frame = f"/{self.host:02X}{self.target:02X}:{body}:XX\r"
        if self.verbose:
            print(f"TX: {frame.rstrip()!r}")
        # On the half-duplex bus the device echoes our own bytes back. Flush
        # the input first and we'll drop anything we receive that matches
        # before the response arrives.
        self.s.reset_input_buffer()
        self.s.write(frame.encode("ascii"))
        self.s.flush()

    def _read_frame(self, deadline: float) -> str:
        """Read one frame terminated by \\r, ignoring our own echo."""
        buf = bytearray()
        echo_tail = None
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("no response from bootloader")
            self.s.timeout = max(0.05, remaining)
            chunk = self.s.read(1)
            if not chunk:
                continue
            buf.extend(chunk)
            if chunk == b"\r":
                frame = buf.decode("ascii", errors="replace")
                if self.verbose:
                    print(f"RX: {frame.rstrip()!r}")
                # Discard echoes of our own outbound frame (starts with /HH TT)
                # and only return a frame addressed back to us.
                if frame.startswith(f"/{self.target:02X}{self.host:02X}:"):
                    return frame
                buf.clear()

    def _exchange(self, body: str, timeout: float = 5.0) -> str:
        self._send(body)
        return self._read_frame(time.monotonic() + timeout)

    @staticmethod
    def parse_response(frame: str) -> tuple[str, list[str]]:
        # /TTFF:KEYWORD payload...:XX\r → (keyword, [tokens...])
        body = frame.strip().lstrip("/")
        if ":" not in body:
            raise ValueError(f"malformed frame: {frame!r}")
        addrs, rest = body.split(":", 1)
        if rest.endswith(":XX") or rest[-3:].startswith(":"):
            rest = rest.rsplit(":", 1)[0]
        # rest is now "KEYWORD payload..."
        toks = rest.split(" ")
        return toks[0], toks[1:]

    # ---- protocol methods ----

    def info(self) -> dict[str, str]:
        frame = self._exchange("BLINFO")
        kw, toks = self.parse_response(frame)
        if kw != "BLACK" or not toks or toks[0] != "BLINFO":
            raise RuntimeError(f"unexpected response to BLINFO: {frame!r}")
        fields = toks[1:]
        if len(fields) < 7:
            raise RuntimeError(f"BLINFO response missing fields: {fields}")
        return {
            "device_id":  fields[0],
            "page_size":  int(fields[1], 16),
            "app_start":  int(fields[2], 16),
            "flash_end":  int(fields[3], 16),
            "crc_addr":   int(fields[4], 16),
            "bl_version": int(fields[5], 16),
            "app_valid":  fields[6] == "1",
        }

    def erase(self, start: int, end: int) -> None:
        frame = self._exchange(f"BLERASE {start:04X} {end:04X}", timeout=10.0)
        kw, toks = self.parse_response(frame)
        if kw != "BLACK" or not toks or toks[0] != "BLERASE":
            raise RuntimeError(f"BLERASE failed: {frame!r}")

    def write_page(self, addr: int, seq: int, page_bytes: bytes) -> None:
        if len(page_bytes) == 0:
            return
        hexdata = page_bytes.hex().upper()
        crc = crc16(page_bytes)
        body = f"BLDATA {addr:04X} {seq:02X} {hexdata} {crc:04X}"
        frame = self._exchange(body, timeout=3.0)
        kw, toks = self.parse_response(frame)
        if kw != "BLACK" or len(toks) < 3 or toks[0] != "BLDATA":
            raise RuntimeError(f"BLDATA {addr:#06x} failed: {frame!r}")
        if int(toks[1], 16) != addr or int(toks[2], 16) != (seq & 0xFF):
            raise RuntimeError(f"BLDATA ack mismatch: {frame!r}")

    def verify(self, expected_crc: int) -> None:
        frame = self._exchange(f"BLVERIFY {expected_crc:04X}", timeout=10.0)
        kw, toks = self.parse_response(frame)
        if kw != "BLACK" or not toks or toks[0] != "BLVERIFY":
            raise RuntimeError(f"BLVERIFY failed: {frame!r}")

    def reset(self) -> None:
        # The ACK comes back but the next reset wipes serial state, so any
        # timeout reading further bytes after the ACK is fine.
        self._send("BLRESET")
        try:
            self._read_frame(time.monotonic() + 1.0)
        except TimeoutError:
            pass


# ---- driver logic ----

def main() -> int:
    ap = argparse.ArgumentParser(description="Flash an AVR EA application via DCN-AVR-EA-Bootloader.")
    ap.add_argument("--port", required=True, help="serial port (e.g. COM5, /dev/ttyUSB0)")
    ap.add_argument("--hex",  required=True, type=Path, help="application Intel HEX file")
    ap.add_argument("--addr", default="BB", help="target bootloader address in hex (default BB)")
    ap.add_argument("--host-addr", default="00", help="our (host) address in hex (default 00)")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--no-reset", action="store_true", help="skip BLRESET at the end")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    target = int(args.addr, 16)
    host   = int(args.host_addr, 16)
    hexpath = args.hex
    if not hexpath.exists():
        print(f"hex file not found: {hexpath}", file=sys.stderr)
        return 2

    print(f"Opening {args.port} @ {args.baud} baud…")
    with serial.Serial(args.port, baudrate=args.baud, bytesize=8, parity="N",
                       stopbits=1, timeout=2.0) as port:
        bl = Bootloader(port, target_addr=target, host_addr=host, verbose=args.verbose)

        info = bl.info()
        print(f"Bootloader online:")
        print(f"  device ID  : {info['device_id']}")
        print(f"  page size  : {info['page_size']} bytes")
        print(f"  app region : {info['app_start']:#06x}..{info['flash_end']:#06x}")
        print(f"  CRC addr   : {info['crc_addr']:#06x}")
        print(f"  BL version : {info['bl_version']:#06x}")
        print(f"  app valid  : {info['app_valid']}")

        sparse = read_hex_sparse(hexpath)
        page = info["page_size"]
        app_start = info["app_start"]
        crc_addr  = info["crc_addr"]

        # The user's hex *should* already have a CRC trailer baked in by
        # patch_app_crc.py. Use the trailer present in the hex (or compute
        # if absent) as the expected verify CRC.
        body_bytes = bytes(sparse.get(a, 0xFF) for a in range(app_start, crc_addr))
        expected_crc = crc16(body_bytes)
        baked = (sparse.get(crc_addr, 0xFF) | (sparse.get(crc_addr + 1, 0xFF) << 8))
        if baked != expected_crc:
            print(f"  note: hex trailer {baked:#06x} ≠ recomputed CRC {expected_crc:#06x}.")
            print(f"        Did you run patch_app_crc.py? Using recomputed value.")

        # Determine the page range to erase: smallest page-aligned span
        # covering every data byte in [app_start, crc_addr+2).
        all_addrs = [a for a in sparse if app_start <= a <= crc_addr + 1]
        if not all_addrs:
            print("hex contains no bytes in app region — nothing to do.")
            return 0
        erase_start = (min(all_addrs) // page) * page
        erase_end   = ((max(all_addrs) // page) + 1) * page - 1
        if erase_start < app_start:
            erase_start = app_start
        if erase_end > info["flash_end"]:
            erase_end = info["flash_end"]

        print(f"Erasing {erase_start:#06x}..{erase_end:#06x}…")
        bl.erase(erase_start, erase_end)

        # Walk page-by-page, write any page that contains data.
        seq = 0
        pages_written = 0
        for paddr in range(erase_start, erase_end + 1, page):
            page_bytes = bytes(sparse.get(paddr + i, 0xFF) for i in range(page))
            if all(b == 0xFF for b in page_bytes):
                continue  # erased pages don't need a write
            bl.write_page(paddr, seq, page_bytes)
            seq = (seq + 1) & 0xFF
            pages_written += 1
            if pages_written % 16 == 0:
                print(f"  …{pages_written} pages written")

        print(f"Wrote {pages_written} pages. Verifying…")
        bl.verify(expected_crc)
        print(f"Verify OK (CRC {expected_crc:#06x}). Trailer committed.")

        if not args.no_reset:
            print("Sending BLRESET. Set the switch back to its normal position now.")
            bl.reset()

    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
