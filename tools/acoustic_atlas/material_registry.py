#!/usr/bin/env python3
"""Build a per-persistent-ID acoustic register from the existing P3 material export.

Every material gets its own entry, including nonstructural/unknown materials.
Does not fabricate wet data, thickness, scattering, density or sound banks.
Output stays local unless its original material-data distribution is licensed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path


def compile_registry(rows: list[dict], source_sha: str) -> dict:
    if not rows:
        raise ValueError("empty material table")
    names: set[str] = set()
    ids: set[int] = set()
    items = []
    for row in rows:
        mid = row["id"]
        name = row["name"].replace("\\", "/").lower()
        if mid in ids or name in names or not isinstance(mid, int) or mid < 0:
            raise ValueError(f"duplicate/invalid native material {mid}: {name}")
        ids.add(mid)
        names.add(name)
        spectra = [row.get("absorption9"), row.get("transmission_energy9"),
                   row.get("boundary_loss9")]
        known = bool(row.get("boundary_model_available")) and all(
            isinstance(v, list) and len(v) == 9 and all(
                isinstance(x, (float, int)) and math.isfinite(x) and 0 <= x <= 1
                for x in v) for v in spectra
        )
        violations = []
        if known:
            a, t, loss = spectra
            for band, (absorption, transmission, recorded_loss) in enumerate(zip(a, t, loss)):
                if absorption + transmission > 1.0 + 1e-5:
                    violations.append({"band": band, "kind": "ENERGY_OVER_UNITY",
                                       "excess": absorption + transmission - 1.0})
                if abs(min(1.0, absorption + transmission) - recorded_loss) > 1e-4:
                    violations.append({"band": band, "kind": "BOUNDARY_LOSS_MISMATCH"})
            reflection9 = [max(0.0, 1.0 - z) for z in loss] if not violations else None
        else:
            reflection9 = None
        items.append({
            "native_id": mid, "runtime_index": row.get("runtime_index"),
            "native_name": name,
            "native_flags": row.get("native_flags"),
            "class": row.get("family"),
            "acoustic_role": "surface" if known else "unresolved_or_nonstructural",
            "dry_absorption_energy9": spectra[0] if known else None,
            "dry_transmission_energy9": spectra[1] if known else None,
            "dry_reflection_energy9": reflection9,
            "physics_review_required": bool(violations),
            "physics_review_reasons": violations,
            "wet_absorption_energy9": None, "wet_transmission_energy9": None,
            "wet_profile_status": "NOT_RECORDED",
            "wet_coverage_status": "UNDETERMINED_BY_ATLAS",
            "scattering": None,
            "density_kg_m3": None,
            "thickness_m": None,
            "structure_identity": None,
            "assembly": None,
            "sound_contact_bank": None,
            "provenance": row.get("provenance"),
            "source_model": row.get("model"),
            "source_assignment": row.get("assignment"),
            "runtime_legacy_pressure": row.get("legacy_pressure"),
            "native_material_origin": row.get("native_material_origin"),
            "physical_coefficients_measured": row.get("physical_coefficients_measured", False),
        })
    items.sort(key=lambda r: r["native_id"])
    shared = {}
    for r in items:
        if r["dry_absorption_energy9"] is not None:
            key = json.dumps([r["dry_absorption_energy9"], r["dry_transmission_energy9"]])
            shared.setdefault(key, []).append(r["native_id"])
    return {
        "schema": "LIMINAL_ACOUSTIC_MATERIAL_REGISTRY_V1",
        "input_sha256": source_sha,
        "total_materials": len(items),
        "materials_with_nine_band_boundary_model":
            sum(x["dry_absorption_energy9"] is not None for x in items),
        "materials_without_spectral_boundary_model":
            sum(x["dry_absorption_energy9"] is None for x in items),
        "shared_dry_spectral_profiles": [ids for ids in shared.values() if len(ids) > 1],
        "fully_evidenced_wet_profiles": 0,
        "materials_needing_physics_review": [
            r["native_id"] for r in items if r["physics_review_required"]],
        "note": "Unique native identities do not imply unique measured coefficients.",
        "materials": items,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    if args.input.resolve() == args.output.resolve():
        ap.error("source and output must not be the same file")
    raw = args.input.read_bytes()
    rows = json.loads(raw)
    if not isinstance(rows, list):
        ap.error("expected a list of native materials")
    registry = compile_registry(rows, hashlib.sha256(raw).hexdigest())
    if args.output.exists():
        ap.error("refusing to overwrite existing authored material registry")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(registry, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in registry.items()
                      if k not in ("materials", "shared_dry_spectral_profiles")},
                     indent=2, ensure_ascii=False))
    print(f"shared dry spectral profiles: {len(registry['shared_dry_spectral_profiles'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
