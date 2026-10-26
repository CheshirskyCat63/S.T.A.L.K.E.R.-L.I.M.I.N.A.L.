#!/usr/bin/env python3
"""Stage a *local* playable LIMINAL Audio baseline from an already installed IX-Ray profile.

Only copy material already present on the owner's computer. This does NOT
redistribute third-party game files and does NOT ship unfinished physical v6 data.
The current profile may still use external archive paths from fsgame.ltx.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

NEEDED = (
    "bin/xrEngine.exe", "bin/xrGame.dll", "bin/xrSound.dll",
    "bin/SDL3.dll", "fsgame.ltx",
    "gamedata/levels/pripyat_full/level.snd_env",
    "gamedata/levels/pripyat_full/level.snd_rooms",
)


def hash_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(4 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def stage(source: Path, target: Path, apply: bool) -> dict:
    source = source.resolve()
    target = target.resolve()
    if os.name != "nt":
        raise RuntimeError("Windows/robocopy only; this is a local IX-Ray profile")
    if not (source / "ixr_addons").is_dir() or not (source / "userdata").is_dir():
        raise ValueError("source does not appear to be an installed IX-Ray profile")
    for rel in NEEDED:
        if not (source / rel).is_file():
            raise FileNotFoundError(f"required working profile asset: {rel}")
    if target.exists():
        raise FileExistsError("target already exists; never merge into an existing game profile")
    if source == target or source in target.parents or target in source.parents:
        raise ValueError("target and source must not be nested")
    files = [p for p in source.rglob("*") if p.is_file()]
    total_size = sum(p.stat().st_size for p in files)
    available = shutil.disk_usage(target.parent).free
    result = {
        "schema": "LIMINAL_LOCAL_AUDIO_BASELINE_V1",
        "source": str(source), "target": str(target),
        "file_count": len(files), "source_bytes": total_size,
        "available_bytes": available,
        "checked_core_sha256": {rel: hash_file(source / rel) for rel in NEEDED},
        "source_is_existing_playable_profile": True,
        "acoustic_atlas_complete": False,
        "note": "Local copy only; preserve external fsgame archive dependencies. Physical v6 not shipped.",
    }
    if not apply:
        return result
    if available < int(total_size * 1.05):
        raise RuntimeError("insufficient free space for independent copy")
    target.mkdir(parents=True, exist_ok=False)
    proc = subprocess.run([
        "robocopy", str(source), str(target), "/E", "/COPY:DAT", "/DCOPY:DAT",
        "/XJ", "/R:1", "/W:1", "/MT:8", "/NP", "/NFL", "/NDL"
    ], capture_output=True, text=True, errors="replace", timeout=2400)
    if proc.returncode >= 8:
        raise RuntimeError(f"robocopy failed (code {proc.returncode}); target must not be used")
    conf = target / "fsgame.ltx"
    raw = conf.read_bytes()
    old = str(source).encode("utf-8")
    new = str(target).encode("utf-8")
    if old.lower() not in raw.lower():
        raise ValueError("cannot rebase fsgame.ltx; copied profile must not be launched")
    # Original IX-Ray Windows paths are ASCII; replace both path casings safely.
    text = raw.decode("utf-8-sig")
    import re
    text = re.sub(re.escape(str(source)), lambda _: str(target), text, flags=re.IGNORECASE)
    conf.write_text(text, encoding="utf-8")
    for rel in NEEDED:
        if rel != "fsgame.ltx" and hash_file(target / rel) != result["checked_core_sha256"][rel]:
            raise RuntimeError(f"staged file differs: {rel}")
    result["copied"] = True
    result["staged_fsgame_sha256"] = hash_file(conf)
    result["created_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
    (target / "LIMINAL-LOCAL-AUDIO-BASELINE.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", required=True, type=Path)
    ap.add_argument("--target", required=True, type=Path)
    ap.add_argument("--apply", action="store_true",
                    help="copy profile; without this only inspect/report")
    args = ap.parse_args()
    try:
        result = stage(args.source, args.target, args.apply)
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as err:
        print(json.dumps({"status": "FAILED", "error": str(err)}, ensure_ascii=False))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
