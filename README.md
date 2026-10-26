# S.T.A.L.K.E.R. L.I.M.I.N.A.L. — Audio

This is the **audio-only** branch (`liminal/audio`). The sibling `liminal/movement` branch owns movement, body, camera and animations. Neither branch directly edits `main`.

## Current checked baseline — 26 October 2026

- Native game profile: `D:\Development\LIMINAL-IX\play-pripyat-audio-v1.2`
- Acoustic source tree: `D:\Development\LIMINAL-IX\source\pripyat-audio-v1` (contains **uncommitted and untracked changes**; do not blindly reset it)
- Verified room geometry: 42,652 rooms, 1,064,008 zones, 921,443 opening groups, 6,964,899 exact opening rectangles.
- Verified v6 geometry-only fixture: 56,535,521 bytes. All 42,652 room `WetReference` calibration values are zero. **This is NOT a physical catalog and MUST NOT be installed.**
- Material blocker: 39,749 rooms retain unresolved real-surface/voxel-boundary identities. 2,903 rooms have complete native acoustic calibration.
- Existing running profile uses legacy v3 (`physical=0`). Do not claim full v4/v6 room acoustics in shipped builds.

### Deliverables already in this branch

- `tools/acoustic_atlas/v6_probe.py`: bounded read-only binary verification and physical-calibration gate.
- `tools/acoustic_atlas/material_state.py`: energy-conserving 9-band absorption, 3-band transmission, authored wet-state interpolation **only if data exists**.
- `tools/acoustic_atlas/route_reference.py`: small route-contract fixture for room → exterior → facade → next building. **Not a production sound engine**.
- `tests/test_atlas.py`: fast, deterministic tests.

### Quick checks on Windows

```powershell
python tests/test_atlas.py
python tools/acoustic_atlas/v6_probe.py "C:\Users\CheCat\Documents\Codex\2026-10-26\https-chatgpt-com-share-6adf4b20-cdd8\work\roadmap-v2\p3\UNINSTALLABLE-geometry-audit.snd_rooms" --geometry-ok
```

Without `--geometry-ok`, an uncalibrated atlas deliberately produces a nonzero exit status.

## Strict delivery gates

1. Preserve all nine finished geometry axis receipts and the actual dirty native source baseline.
2. Reconcile unknown voxel boundaries with CFORM and GameMtlLib; **never infer material from texture alone**.
3. Cover native ID0 surfaces with explicit, traceable authorship; never turn an unknown into automatic concrete.
4. Give **every persistent material ID** its own registry entry, dry spectral data, provenance, and an explicit wet-response status. `WetReference` means room reverb calibration, not wet wood.
5. Compile fully materialized IXROOMS v6 together with the matched level CFORM, environment and material library.
6. Verify source-room excitation → outdoor aperture radiation → facade reflections → listener window ingress → HRTF / final PCM without multiplying the original acoustic energy.
7. Make short game checks with the installed binary and profile. Never publish an offline geometry fixture as an accepted game build.

## Repository and redistribution boundaries

The GitHub repository is **public** and carries Apache-2.0 at its root. That license does **not** automatically relicense existing IX-Ray engine components, STCoP/GAMMA/Paradox audio samples, assets or the original game. This branch contains original LIMINAL tooling and contracts only until third-party source/binary redistribution rights are confirmed. A playable local install can be staged from the owner's already installed and appropriately licensed original files; do not upload an unlicensed full game build to a public branch.

*Current status: two branches created, original tools and basic runtime-independent tests committed; no full native physical catalog release or new playable installer claimed.*
