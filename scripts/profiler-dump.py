#!/usr/bin/env python3
"""
profiler-dump.py — reference parser for OpenRCT2mini .orctprof snapshots.

Reads a .orctprof file (binary container produced by P9's SnapshotWriter)
and prints its INFO metadata, schema, and frame/slow-poll/profiling
chunks as TSV. Doubles as the format's golden-source documentation: if
this script reads a file, the format contract was honoured.

See profiler-plan.md "Snapshot save format" for the on-disk layout.

Usage:
    profiler-dump.py snapshot.orctprof              # full dump
    profiler-dump.py --info snapshot.orctprof       # INFO only
    profiler-dump.py --frames snapshot.orctprof     # FRMS only as TSV
    profiler-dump.py --slow snapshot.orctprof       # SLWS only as TSV
    profiler-dump.py --prof snapshot.orctprof       # PROF only

No third-party dependencies required, except `zstandard` for decompression.
On Debian/Ubuntu: `pip3 install zstandard` or `apt install python3-zstandard`.
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from dataclasses import dataclass, field
from typing import BinaryIO


# ---------------- Top-level container ---------------------------------

MAGIC = b"ORCP"
HEADER_SIZE = 16
FLAG_ZSTD = 0x0001


@dataclass
class FileHeader:
    format_version: int
    flags: int
    body_length: int
    body_checksum: int

    @property
    def zstd_compressed(self) -> bool:
        return (self.flags & FLAG_ZSTD) != 0


def read_header(f: BinaryIO) -> FileHeader:
    raw = f.read(HEADER_SIZE)
    if len(raw) < HEADER_SIZE:
        raise ValueError("file too short for header")
    magic = raw[0:4]
    if magic != MAGIC:
        raise ValueError(f"bad magic: {magic!r} (expected {MAGIC!r})")
    fmt_ver, flags, body_len, body_crc = struct.unpack("<HHII", raw[4:16])
    return FileHeader(fmt_ver, flags, body_len, body_crc)


def read_body(f: BinaryIO, hdr: FileHeader) -> bytes:
    body = f.read(hdr.body_length)
    if len(body) < hdr.body_length:
        raise ValueError(
            f"file truncated: expected {hdr.body_length} body bytes, got {len(body)}"
        )
    crc_actual = zlib.crc32(body) & 0xFFFFFFFF
    if crc_actual != hdr.body_checksum:
        raise ValueError(
            f"body checksum mismatch: file says {hdr.body_checksum:08x}, computed {crc_actual:08x}"
        )

    if hdr.zstd_compressed:
        try:
            import zstandard as zstd
        except ImportError as e:
            raise SystemExit(
                "zstandard module not available — install with `pip3 install zstandard`"
            ) from e
        # The C++ writer streams via ZSTD_compressStream, so the frame header
        # has no decompressed-size field. Use the streaming decompress API
        # (or fall back to a generous max_output_size) instead of the simple
        # one-shot path which requires the size be in the header.
        body = zstd.ZstdDecompressor().decompress(body, max_output_size=64 * 1024 * 1024)
    return body


# ---------------- Chunk walking ---------------------------------------

@dataclass
class Chunk:
    fourcc: str
    data: bytes


def chunks(body: bytes):
    pos = 0
    while pos < len(body):
        if pos + 8 > len(body):
            raise ValueError("truncated chunk header")
        cc_raw, size = struct.unpack_from("<II", body, pos)
        pos += 8
        # FourCC stored little-endian: first byte = LSB.
        fourcc = bytes(
            [
                cc_raw & 0xFF,
                (cc_raw >> 8) & 0xFF,
                (cc_raw >> 16) & 0xFF,
                (cc_raw >> 24) & 0xFF,
            ]
        ).decode("ascii", errors="replace")
        if pos + size > len(body):
            raise ValueError(f"chunk {fourcc} truncated: claims {size} bytes, only {len(body) - pos} left")
        yield Chunk(fourcc, body[pos : pos + size])
        pos += size
        if fourcc == "ENDF":
            return


# ---------------- INFO ------------------------------------------------

def parse_info(payload: bytes) -> dict:
    out = {}
    pos = 0
    while pos < len(payload):
        if pos + 2 > len(payload):
            break
        (klen,) = struct.unpack_from("<H", payload, pos)
        pos += 2
        key = payload[pos : pos + klen].decode("utf-8", errors="replace")
        pos += klen
        if pos + 2 > len(payload):
            break
        (vlen,) = struct.unpack_from("<H", payload, pos)
        pos += 2
        val = payload[pos : pos + vlen].decode("utf-8", errors="replace")
        pos += vlen
        out[key] = val
    return out


# ---------------- SCHM ------------------------------------------------

@dataclass
class FieldDesc:
    name: str
    offset: int
    width: int
    type_code: str  # 'u' | 's' | 'f' | 'b'


@dataclass
class StructDesc:
    name: str
    total_size: int
    fields: list = field(default_factory=list)


def parse_schema(payload: bytes) -> list:
    structs = []
    pos = 0
    while pos < len(payload):
        if pos + 2 > len(payload):
            break
        (slen,) = struct.unpack_from("<H", payload, pos)
        pos += 2
        sname = payload[pos : pos + slen].decode("utf-8")
        pos += slen
        total_size, field_count = struct.unpack_from("<HH", payload, pos)
        pos += 4
        s = StructDesc(sname, total_size)
        for _ in range(field_count):
            (flen,) = struct.unpack_from("<H", payload, pos)
            pos += 2
            fname = payload[pos : pos + flen].decode("utf-8")
            pos += flen
            offset, width = struct.unpack_from("<HB", payload, pos)
            pos += 3
            type_code = payload[pos : pos + 1].decode("ascii")
            pos += 1
            s.fields.append(FieldDesc(fname, offset, width, type_code))
        structs.append(s)
    return structs


def unpack_record(record: bytes, fields: list) -> dict:
    """Unpack one struct's worth of bytes using the SCHM field list."""
    out = {}
    for f in fields:
        chunk = record[f.offset : f.offset + f.width]
        if f.type_code == "u":
            fmt = {1: "<B", 2: "<H", 4: "<I", 8: "<Q"}[f.width]
        elif f.type_code == "s":
            fmt = {1: "<b", 2: "<h", 4: "<i", 8: "<q"}[f.width]
        elif f.type_code == "f":
            fmt = {4: "<f", 8: "<d"}[f.width]
        elif f.type_code == "b":
            fmt = "<?"
        else:
            raise ValueError(f"unknown type code {f.type_code!r} for {f.name}")
        (val,) = struct.unpack(fmt, chunk)
        out[f.name] = val
    return out


# ---------------- FRMS / SLWS ----------------------------------------

def parse_ring(payload: bytes, fields: list) -> list:
    snapshot_size, count, start_index = struct.unpack_from("<III", payload, 0)
    rows = []
    pos = 12
    for i in range(count):
        rec = payload[pos : pos + snapshot_size]
        rows.append(unpack_record(rec, fields))
        pos += snapshot_size
    return rows


# ---------------- PROF -----------------------------------------------

@dataclass
class ProfFunction:
    id: int
    name: str
    call_count: int
    total_us: int
    min_us: int
    max_us: int


def parse_prof(payload: bytes) -> list:
    (count,) = struct.unpack_from("<I", payload, 0)
    pos = 4
    out = []
    for _ in range(count):
        (fid, nlen) = struct.unpack_from("<IH", payload, pos)
        pos += 6
        name = payload[pos : pos + nlen].decode("utf-8", errors="replace")
        pos += nlen
        call_count, total_us, min_us, max_us = struct.unpack_from("<QQQQ", payload, pos)
        pos += 32
        out.append(ProfFunction(fid, name, call_count, total_us, min_us, max_us))
    return out


# ---------------- Reporting -------------------------------------------

def print_tsv(rows: list, fields: list):
    print("\t".join(f.name for f in fields))
    for r in rows:
        print("\t".join(str(r[f.name]) for f in fields))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("path", help=".orctprof file")
    ap.add_argument("--info", action="store_true", help="dump INFO chunk only")
    ap.add_argument("--frames", action="store_true", help="dump FRMS as TSV")
    ap.add_argument("--slow", action="store_true", help="dump SLWS as TSV")
    ap.add_argument("--prof", action="store_true", help="dump PROF aggregates")
    args = ap.parse_args()

    with open(args.path, "rb") as f:
        hdr = read_header(f)
        body = read_body(f, hdr)

    sel_any = args.info or args.frames or args.slow or args.prof

    info = {}
    schema = []
    frames = []
    slow = []
    prof = []
    note = ""

    for c in chunks(body):
        if c.fourcc == "INFO":
            info = parse_info(c.data)
        elif c.fourcc == "SCHM":
            schema = parse_schema(c.data)
        elif c.fourcc == "FRMS":
            # SCHM must come first.
            fields = next((s.fields for s in schema if s.name == "FrameSnapshot"), [])
            frames = parse_ring(c.data, fields) if fields else []
        elif c.fourcc == "SLWS":
            fields = next((s.fields for s in schema if s.name == "SlowPoll"), [])
            slow = parse_ring(c.data, fields) if fields else []
        elif c.fourcc == "PROF":
            prof = parse_prof(c.data)
        elif c.fourcc == "NOTE":
            note = c.data.decode("utf-8", errors="replace")
        elif c.fourcc == "ENDF":
            pass

    if not sel_any:
        # Default: a one-page summary.
        print(f"format_version: {hdr.format_version}")
        print(f"compressed:     {hdr.zstd_compressed}")
        print(f"body_length:    {hdr.body_length}")
        print()
        print("[INFO]")
        for k, v in info.items():
            print(f"  {k}: {v}")
        if note:
            print()
            print("[NOTE]")
            print(f"  {note}")
        print()
        print(f"[SCHM] structs: {[s.name for s in schema]}")
        print(f"[FRMS] count: {len(frames)}")
        print(f"[SLWS] count: {len(slow)}")
        print(f"[PROF] count: {len(prof)}")
        return

    if args.info:
        for k, v in info.items():
            print(f"{k}\t{v}")
    if args.frames:
        fields = next((s.fields for s in schema if s.name == "FrameSnapshot"), [])
        print_tsv(frames, fields)
    if args.slow:
        fields = next((s.fields for s in schema if s.name == "SlowPoll"), [])
        print_tsv(slow, fields)
    if args.prof:
        # Sort by total_us descending — most expensive first.
        for p in sorted(prof, key=lambda x: x.total_us, reverse=True):
            print(
                f"{p.total_us:>14d}us  count={p.call_count:>9d}  "
                f"avg={(p.total_us / p.call_count) if p.call_count else 0:>10.2f}us  "
                f"{p.name}"
            )


if __name__ == "__main__":
    main()
