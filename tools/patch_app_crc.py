#!/usr/bin/env python3
"""
patch_app_crc.py — stamp a CRC-16/CCITT-FALSE trailer onto an application
hex file so the DCN-AVR-EA-Bootloader will recognise the image as valid.

The bootloader, on every reset, computes CRC-16/CCITT-FALSE over the byte
range [app-start, crc-addr) and compares it to the 2-byte little-endian
value stored at crc-addr. Match → boot the app. Mismatch → stay in
recovery mode.

This tool reads an Intel HEX file, computes that CRC over the bytes that
fall in the range, writes the result back into the hex at crc-addr, and
writes the patched hex out. Any bytes in the range that aren't covered
by the input are treated as 0xFF (erased flash).

Example:
    python tools/patch_app_crc.py \\
        --in build/SPS-1.hex \\
        --out build/SPS-1.crc.hex \\
        --app-start 0x0800 --crc-addr 0xFFFE

No external dependencies — uses only the Python standard library.
"""

from __future__ import annotations
import argparse
import sys
from pathlib import Path


# ---- Intel HEX (very small subset; enough for AVR < 64 KB images) ----

class HexImage:
    """Sparse byte image. Default value for unspecified bytes is 0xFF."""

    def __init__(self) -> None:
        self.data: dict[int, int] = {}
        self.ext_linear_records: list[tuple[int, int]] = []  # passthroughs we kept

    @classmethod
    def from_file(cls, path: Path) -> "HexImage":
        img = cls()
        ext_addr = 0
        with path.open("r", encoding="ascii") as f:
            for lineno, raw in enumerate(f, 1):
                line = raw.strip()
                if not line or not line.startswith(":"):
                    continue
                body = line[1:]
                bytes_ = bytes.fromhex(body)
                if len(bytes_) < 5:
                    raise ValueError(f"{path}:{lineno}: short record")
                count, addr_hi, addr_lo, rtype = bytes_[0], bytes_[1], bytes_[2], bytes_[3]
                payload = bytes_[4:4 + count]
                checksum = bytes_[4 + count]
                calc = (0x100 - (sum(bytes_[:-1]) & 0xFF)) & 0xFF
                if calc != checksum:
                    raise ValueError(f"{path}:{lineno}: bad checksum")
                addr16 = (addr_hi << 8) | addr_lo
                if rtype == 0x00:  # data
                    base = ext_addr + addr16
                    for i, b in enumerate(payload):
                        img.data[base + i] = b
                elif rtype == 0x01:  # EOF
                    break
                elif rtype == 0x04:  # ext linear addr
                    ext_addr = ((payload[0] << 8) | payload[1]) << 16
                elif rtype == 0x05:  # start linear addr — record but ignore
                    pass
                else:
                    # Unsupported but preserved: type and payload, for re-emit
                    pass
        return img

    def read_byte(self, addr: int) -> int:
        return self.data.get(addr, 0xFF)

    def write_byte(self, addr: int, value: int) -> None:
        self.data[addr] = value & 0xFF

    def to_file(self, path: Path, bytes_per_record: int = 16) -> None:
        """Write Intel HEX. Always emits 16-bit-address data records,
        which is sufficient for any AVR EA in this family (64 KB flash)."""
        if not self.data:
            raise ValueError("nothing to write")
        addrs = sorted(self.data.keys())
        with path.open("w", encoding="ascii", newline="\n") as f:
            i = 0
            while i < len(addrs):
                start = addrs[i]
                # collect contiguous run of up to bytes_per_record
                run: list[int] = []
                while i < len(addrs) and addrs[i] == start + len(run) and len(run) < bytes_per_record:
                    run.append(self.data[addrs[i]])
                    i += 1
                self._emit(f, 0x00, start, bytes(run))
            self._emit(f, 0x01, 0, b"")  # EOF

    @staticmethod
    def _emit(f, rtype: int, addr: int, data: bytes) -> None:
        rec = bytes([len(data), (addr >> 8) & 0xFF, addr & 0xFF, rtype]) + data
        chksum = (0x100 - (sum(rec) & 0xFF)) & 0xFF
        f.write(":" + rec.hex().upper() + f"{chksum:02X}\n")


# ---- CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout) ----

def crc16_ccitt(buf: bytes, crc: int = 0xFFFF) -> int:
    for byte in buf:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


# ---- Main ----

def parse_int(s: str) -> int:
    return int(s, 0)

def main() -> int:
    ap = argparse.ArgumentParser(description="Stamp CRC trailer on an AVR EA app hex.")
    ap.add_argument("--in",  dest="inp",  required=True, type=Path, help="input  hex file")
    ap.add_argument("--out", dest="outp", required=True, type=Path, help="output hex file")
    ap.add_argument("--app-start", type=parse_int, required=True, help="app start byte address (e.g. 0x0800)")
    ap.add_argument("--crc-addr",  type=parse_int, required=True, help="trailer byte address (e.g. 0xFFFE)")
    args = ap.parse_args()

    if args.crc_addr <= args.app_start:
        print("error: --crc-addr must be greater than --app-start", file=sys.stderr)
        return 2

    img = HexImage.from_file(args.inp)

    # Sanity: warn if no app bytes present in the range.
    in_range = [a for a in img.data if args.app_start <= a < args.crc_addr]
    if not in_range:
        print(f"warning: input has no bytes in [{args.app_start:#06x}, {args.crc_addr:#06x})",
              file=sys.stderr)

    # Build the byte stream to checksum: every address in the range, with
    # missing bytes treated as 0xFF (matches what the bootloader sees in
    # erased flash).
    body = bytes(img.read_byte(a) for a in range(args.app_start, args.crc_addr))
    crc = crc16_ccitt(body)

    img.write_byte(args.crc_addr,     crc & 0xFF)
    img.write_byte(args.crc_addr + 1, (crc >> 8) & 0xFF)
    img.to_file(args.outp)

    print(f"CRC-16/CCITT over [{args.app_start:#06x}, {args.crc_addr:#06x}) = 0x{crc:04X}")
    print(f"Wrote trailer to {args.crc_addr:#06x} in {args.outp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
