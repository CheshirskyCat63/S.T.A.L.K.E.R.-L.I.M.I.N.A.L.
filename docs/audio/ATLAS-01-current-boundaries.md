# Audio ATLAS-01 — implementation boundary and measured blockers
**26 October 2026. Scope: `liminal/audio` only. GPU, bodycam, animation and movement stay untouched.**

## Installed baseline, branches and release gate
- Public `main` and sibling `liminal/movement` not modified by audio work.
- Existing source branch `liminal/pripyat-audio-v1.2-physical-mix` at `45a6cbdd1` contains extensive **uncommitted and untracked** acoustic changes. Never reset, replace or cherry-pick it blindly.
- Original playable-profile files were locally staged without changing their original source under `D:\Development\LIMINAL-IX\play-liminal-audio-rc0` (16,890 files, 22,061,228,520 bytes, startup binaries and environment files SHA-256 verified).
- New profile uses external archives already installed by the game owner. Not a self-contained distributable.
- The native executable did start from RC0, load `LIMINAL Acoustic Tract v2`, initialize SDL/WASAPI/Resonance and reach the menu; the attempted minimal 90s automated **world** smoke ended `FAIL` / `0/120 frames`, with just menu music rendering. Do **not** label this a gameplay PASS. Correct the QA start fixture instead of running a large test suite.
- Original profile's acoustic catalog stays v3 `physical=0`. The full-size verified v6 file `UNINSTALLABLE-geometry-audit.snd_rooms` is **geometry-only** and remains out of the game.

## Verified compiled geometry (nine existing axes, never recompute)
| Quantity | Count |
|---|---:|
| Logical rooms | 42,652 |
| Space cells / zones | 1,064,008 |
| Portal groups | 921,443 |
| Exact finite opening rectangles | 6,964,899 |
| v6 stored geometry fixture | 56,535,521 bytes |
| v6 logical payload | 149,445,792 bytes |
| Rooms with calibrated material and native room-return reference in separate P3 subset | 2,903 |
| Rooms not yet eligible for a fully physical atlas | 39,749 |

`v6_probe.py` checks the complete stored stream CRC and block bounds, room wet-return calibration, material-boundary completeness and zone identities; it independently accepted the real geometry fixture, rejected it as an installable physical atlas. Note: **`WetReference` is native room reverb return calibration; it has no relation to material moisture.**

The P3 recovery contract is at `...\work\roadmap-v2\p3\P3_CONTINUATION.md`.
- Remaining boundary sampling issues: 9,324,953 unresolved quadrature samples, about 231,256.006 m² across 39,749 rooms; includes ~180,309 m² no proven native material and ~50,947 m² explicit native ID0.
- Known cause: many air-voxel faces do not coincide with physical CFORM walls. A normal-ray miss is **not proof** of open sky or window.
- Index known coordinates deterministically with `indexed_material_continuation.py`; recheck only unresolved source samples, preserve `source_sha256:row:ix:iy`, do not rerun nine axes and do not double-apply 17,816 recovered samples.
- For actual CFORM native ID0, preserve unknown until a source or explicitly authored material assignment proves its acoustic identity. Renderer texture color/THM is not sufficient.

## All 74 native materials — every material identity is accounted for
Read-only registry compiled on Windows from original `material_table74_current.json`:
- **74** distinct persistent material IDs accounted for, registry kept local until content licensing review.
- **46** IDs have known nine-band boundary loss/absorption, from published or authored approximate material models; **28** have no full static boundary spectra. Many of the 28 are creatures, water, vegetation, fake/helper collisions and objects not intended as solid room boundaries. Require explicit `acoustic_role`, do not fill those with concrete or block all air.
- **6 groups** of native IDs currently share dry material model curves. Sharing is not physically distinct coefficients; record an explicit per-material assembly/profile before claiming "every material has unique real characteristics".
- **0** independently evidenced wet-material transfer profiles. Engine has no native wood-moisture response: it must be authored from actual evidence and passed as state-dependent coefficients to the existing material filters/room model. No moisture-to-gain heuristic or artificial room gain.
- **Native ID81 `materials\\cloth` is physically inconsistent in some bands:** its current heavy-curtain absorption plus squared old GameMtlLib transmission (>1 combined incident energy) is capped to 1.0 by current `BoundaryLoss`. This is **not energy-conserving**. Review/replace with a self-consistent assembly transmission curve, with provenance; do not silently renormalize.
- A unique acoustics registry is not an audio bank: collision/step sounds are chosen by existing native material pairs; their identity and file provenance remain separately auditable.

### Maximum usable properties, without inventing IX-Ray APIs
Per solid **material**: 9 octave absorption energies; 3 transmission energies (and 9 interpolated coefficients for room integration); derived two summary reflection bands; reflectance/refractive flags; native `SGameMtl::fSndOcclusionFactor`; named provenance. Existing `GameMtlLib` physics exposes a `fDensityFactor` but not measured acoustic SI density. Per contact **pair**: impact/step/breaking audio mappings. Per room: 9-band mean closed surface loss, volume, union area, opening area, native calibrated `WetReference`, native Resonance reverb parameters. There is no global per-material native scattering coefficient, geometry thickness, humidity or timber wetness equation. These **extra authored data** should be stored in a versioned atlas companion registry and compiled to the already existing sound filter coefficients.

A solid construction (`AcousticAssembly`) is a different type from `Material`. A confirmed CFORM entry/exit mesh pair or authored panel description may provide thickness and layering; a lone triangle cannot reveal the thickness of an actual wall. Mark unknown, not fabricated. Scattering can be authored in future for reflection distribution only after a native consumer exists; storing a number now does not make the effect audible.

Moisture belongs to *state*, not material identity: a material can declare `dry` plus explicitly evidenced `wet` or continuous calibrated interpolation; the weather/exposure system supplies an actual moisture/exposure parameter and updates materials infrequently. An unchanged room/facade is still geometry-static when wet; only its coefficients change. If no wet curves are available, leave state dry and record the missing evidence rather than manufacturing it.

## Multi-building spatial route: native integration tasks
Use existing `SoundRoomCompactCatalog`, `SoundRoomPath`, `SoundPhysicalRoomEntry`, `SoundRoomFieldGeometry`, `SoundGeometryFar`, Resonance room buses. Do not add a second audio engine.

**Target trace:**
1. NPC firearm/primary PCM at point **S** in upper-floor room **A** excites A's *local diffuse field* without multiplying its listener-level direct attenuation.
2. The field reaches verified finite window **W1**. Use the precomputed room-to-window geometry, frequency transmission and opening solid-angle coupling, not the entire room's global wet value played at W1 unscaled.
3. The already transmitted component propagates across **outdoor air room 0** as a genuine directional virtual acoustic path. At outdoor facade **F** add only a verified specular reflection with per-frequency loss and extra propagation delay.
4. Both direct aperture and facade path can enter window **W2** of room **B**. They carry different times/directions and are not blindly summed as independent full-energy sources.
5. At listener head point **L** preserve ear-local HRTF directions and separate direct impulse, finite reflections, B room local field, and A field transmitted through W1/W2.
6. Render late A-field as **existing room bus output** with finite aperture propagation. Forbid unrestricted recursive return-to-return sends that would create artificial feedback or double count the same sound energy.
7. Validate an outdoor listener near W1 and an indoor listener behind W2, closed/open glass, plus movement between locations. Use the existing `SoundRecordedEchoPolicy` to avoid duplicating authored weapon tails against physical returns only when physical participation has actually been proven.

**Current blockers at exact source functions:**
- `SoundMixer.cpp::Snd_UpdateRoomReturns` works with a room pool of 12 returns and `SoundRoomPortal Candidates[4]`; `SoundRoomFieldGeometry::ValidatedCoupling` requires all three candidate supports, each with two clear ray legs. Thus valid partial openings can be dropped by conservative whole-opening acceptance.
- A source room-return field is not represented as a persistent outdoor traveling emitter that independently continues to a second window and facade; existing direct-shot `SoundGeometryFar` is not equivalent to transporting the **diffuse** field of another building.
- `SoundRoomPath::HopCapacity=4`, `SearchCapacity=48`, `EdgeCapacity=8`, `CandidateCapacity=4`; must track truncation separately from blocked geometry.
- Room weights/source field and outdoor spectrum need **a single energy-owner** across finite/reflected/recorded and late components; avoid clamping an oversized coherent sum after it has already fabricated energy.
- Current installed complete physical room coverage=0, making a full game-side acceptance impossible until material identities/room calibration are complete.

### Practical effect, not a fake horn/air blast
An open window does not generate energy. It acts as a finite aperture/radiator from the room to the exterior, with a strong directional cue and far higher energy than the wall-transmitted path. Outdoor reflected energy may make the shot/tail seem much fuller at a particular listener position. A genuine blast/shock-pressure waveform, however, is not automatically generated by linear reverb or opening gain. Any intentionally cinematic extra punch must be clearly separated as a bounded **source character** enhancement, not encoded as falsified absorption/transmission physics. Do not add a global window +dB knob.

## Ordered short acceptance
1. Preserve current source/level hashes; feed only immutable P3 fragments into the new workspace.
2. Verify complete geometry counts and strong 0 wet-reference refusal (`v6_probe.py`).
3. Classify only unresolved real physical boundaries/ID0 with traceable origin, preserving former known results.
4. Validate consistent energy for all actual static surfaces; resolve ID81 cloth case. Fill material wetness only where evidential curves exist.
5. Native-calibrate 42,652 rooms when the full material input is ready. No dummy wet references.
6. Install a matched v6 `snd_env` + `snd_rooms` + CFORM + GameMtlLib into the *separate* local RC staging folder and verify `physical=1` in real runtime log.
7. One controlled audio event with dry/send/return PCM across window W1, facade F, and window W2; demonstrate nonzero energy and correct delay, then one open/closed variant.
8. Publish a clearly identified release candidate, do not claim `atlas finished` from a geometry-only checksum.

Minimal quick tests: `python tests/test_atlas.py` (10 currently pass); the real v6 checker passed on the saved 56,535,521-byte candidate. Do not run large gameplay regression by default.

## Legal/public repository boundary
The root Apache-2.0 license does not relicense upstream IX-Ray New Sound Engine code, S.T.A.L.K.E.R. original assets, GAMMA/STCoP/Paradox samples or game binaries. This public branch contains newly authored tools, documentation and references. Native build/install staging is performed on the owner's PC using already present files; redistribution requires independent permission and licensing review.
