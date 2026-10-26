#!/usr/bin/env python3
"""Inspect an IXROOMS v6 file without allocating millions of portal objects.

This is a read-only gate for LIMINAL ATLAS-01, not an acoustic renderer.
The geometric test fixture is intentionally NOT an installable physical atlas.
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import zlib
from pathlib import Path

HEADER = struct.Struct("<8s14I")
BLOCK = struct.Struct("<6I")
ROOM = struct.Struct("<II23f")
MAX_STORED = 128 * 1024 * 1024
MAX_LOGICAL = 256 * 1024 * 1024
MAX_BLOCK = 8 * 1024 * 1024


class AtlasError(ValueError):
    """Invalid bounded v6 binary or inconsistent physical evidence."""


def inspect(path: Path) -> dict:
    size = path.stat().st_size
    if size < 64 or size > MAX_STORED:
        raise AtlasError("stored file outside v6 bounds")
    with path.open("rb") as f:
        raw_header = f.read(HEADER.size)
        (magic, version, env_crc, zones, rooms, portals, checksum, groups,
         blocks, dirbytes, logical, flags, logical_version, header_bytes,
         reserved) = HEADER.unpack(raw_header)
        if (magic != b"IXROOMS\0" or version != 6 or logical_version != 4
                or flags != 1 or header_bytes != 64 or reserved
                or not 0 < rooms <= 131072 or not 0 < blocks <= 1024
                or not 0 < groups <= portals or dirbytes != blocks * BLOCK.size):
            raise AtlasError("invalid v6 header/identity")
        metadata_bytes = zones * 4 + rooms * ROOM.size
        expected = metadata_bytes + groups * 32 + portals * 16
        if logical != expected or logical > MAX_LOGICAL:
            raise AtlasError("logical size disagrees with counts")
        directory = [BLOCK.unpack(f.read(BLOCK.size)) for _ in range(blocks)]
        pos_logical = 0
        pos_file = 64 + dirbytes
        digest = 0
        metadata = bytearray(metadata_bytes)
        for offset, produced, stored_at, stored_size, block_crc, block_reserved in directory:
            if (block_reserved or offset != pos_logical or stored_at != pos_file
                    or not 0 < produced <= MAX_BLOCK
                    or not 0 < stored_size <= 9 * 1024 * 1024
                    or offset + produced > logical or stored_at + stored_size > size):
                raise AtlasError("invalid block offset/size")
            f.seek(stored_at)
            packed = f.read(stored_size)
            decoder = zlib.decompressobj()
            unpacked = decoder.decompress(packed, MAX_BLOCK + 1)
            unpacked += decoder.flush()
            if (not decoder.eof or decoder.unused_data or
                    len(unpacked) != produced or
                    zlib.crc32(unpacked) != block_crc):
                raise AtlasError("compressed block invalid or CRC mismatch")
            begin = offset
            end = min(offset + produced, metadata_bytes)
            if begin < end:
                metadata[begin:end] = unpacked[:end - begin]
            digest = zlib.crc32(unpacked, digest)
            pos_logical += produced
            pos_file += stored_size
        if pos_logical != logical or pos_file != size or digest != checksum:
            raise AtlasError("logical CRC/stream coverage mismatch")
    refs = []
    for i in range(rooms):
        # Room record: IDs(8), Min/Max(24), Volume(4), Surface(4), Wet(4)
        wet = struct.unpack_from("<f", metadata, zones * 4 + i * ROOM.size + 40)[0]
        refs.append(wet)
    calibrated = sum(math.isfinite(w) and w > 0.0 for w in refs)
    return {
        "format": "IXROOMS_v6",
        "env_crc32": f"{env_crc:08x}",
        "logical_crc32": f"{checksum:08x}",
        "zones": zones,
        "rooms": rooms,
        "portal_groups": groups,
        "exact_portal_rectangles": portals,
        "logical_bytes": logical,
        "stored_bytes": size,
        "rooms_calibrated": calibrated,
        "rooms_without_valid_wet_reference": rooms - calibrated,
        "physical_installable": calibrated == rooms,
        "stage": "PHYSICAL_READY" if calibrated == rooms else "GEOMETRY_ONLY_DO_NOT_INSTALL",
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("catalog", type=Path)
    p.add_argument("--geometry-ok", action="store_true",
                   help="return 0 for valid geometry even if uncalibrated; never publish it")
    args = p.parse_args()
    try:
        result = inspect(args.catalog)
    except (OSError, AtlasError, struct.error, zlib.error) as exc:
        print(json.dumps({"status": "INVALID", "error": str(exc)}, ensure_ascii=False))
        return 2
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if result["physical_installable"] or args.geometry_ok else 3


if __name__ == "__main__":
    raise SystemExit(main())
