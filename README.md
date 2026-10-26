# S.T.A.L.K.E.R. L.I.M.I.N.A.L. — World Destruction

**Branch:** `liminal/destruction` (independent of `liminal/audio`, `liminal/movement` and `main`).

## Objective

Make IX-Ray environments respond convincingly to gunfire, explosions and impacts: window panes and frames detach, authored wall sections break into real rigid bodies, cover disappears, debris and dust use native material effects, and room acoustics respond to opened apertures. Preserve the world's unchanged static structural core at **runtime**. Do not introduce Unreal/Chaos, a replacement physics engine, a second acoustic engine or wholesale runtime CFORM rebuilding.

## Verified local architecture (26 October 2026)

The source and game data were inspected read-only in `D:\Development\LIMINAL-IX`.

- `source/pripyat-audio-v1/src/xrEngine/GameMtlLib.h`: friction, damping, bounce, shoot/occlusion factors and `flBreakable` exist. They do **not** encode tensile strength, thickness, anchor durability or pre-fractured geometry.
- `source/pripyat-audio-v1/src/Editors/LevelEditor/Editor/Tools/StaticObject/ESceneObjectToolsExportBreakable.cpp`: Level Editor already extracts faces bearing `flBreakable`, exports `brkbl#*.ogf`, and creates `breakable_object` spawn entries.
- `source/pripyat-audio-v1/src/xrGame/BreakableObject.cpp`: intact static geometry shell switches to a split dynamic physics shell; explosion impulses are already accepted. There are hard-coded random impulses, global static thresholds/cleanup parameters, and no topology changes to the compiled world mesh.
- `source/pripyat-audio-v1/src/xrPhysics/PHFracture.cpp`: native ODE-backed fracture/shell-split infrastructure exists.
- `source/pripyat-audio-v1/src/xrGame/Explosive.cpp`: native blast/occlusion uses `fShootFactor` for ray transmission, sampling 5 rays per object and processing 3 affected objects per frame. Reuse the hit pipeline, but do not create an unbounded number of physical shards.
- `source/pripyat-audio-v1/src/xrSound/New/SoundBus.h`: immutable room/portal sidecar has persistent room IDs and portal IDs; no dynamic opening-control API yet.
- Current `play-pripyat-audio-v1.2/gamedata/levels/pripyat_full` has `level.cform`, `level.snd_env`, `level.snd_rooms`, `level.ai`, but no `level.som`. The shipped acoustic rooms metadata is v3 / `physical=0`; an unrelated v6 geometry-only fixture is **not** installable.
- Audio material completion candidate: 74 persistent IDs, 1,995 pairs; 46 materials have declared approximate spectral models, 28 do not; **zero** have measured physical coefficients; candidate stage specifically states **not installed**. Avoid confusing candidate data with effective mounted VFS.
- Full `pripyat_full` static CFORM is 1,391,532,236 bytes, its renderer level.geom 1,960,473,366 bytes, and level.ai 408,895,531 bytes. Rebuilding runtime static structures globally is out of scope.

## Architectural contract

`GameMtlLib (persistent ID) + MaterialPair (collision/break sounds)` -> `DestructionMaterial (authored strength, density, variants, acoustic assembly)` -> `DestructibleAssembly (asset, panels, anchors, prefractured chunks, bounds)` -> `DestructionEvent` -> consumers `NativePhysics / DynamicCollision / RendererVFX / AcousticPortalOverride / AIObstacle / SaveState`.

**Hard rule:** For a genuinely traversable hole, the compiled static mesh and CFORM must already have the void, covered in the intact state by separately collidable/rendered authored panels; alternatively a localized, separately engineered collision/render mask must be proven. Just hiding a texture or toggling a sound portal leaves an invisible static wall. Visual-only destruction on untouched vanilla static geometry must be explicitly labeled cosmetic.

**Hard rule:** The acoustic material index in CFORM/bones is not the persistent `SGameMtl.ID`; resolve through `GMLib.GetMaterialByIdx` and then the canonical ID. Never use unknown material ID0 as automatic brick/concrete.

**Hard rule:** Audio data is read-only. Destruction emits state events and sparse opening/material overlays, applied on the game thread and published safely to the existing sound mixer; do not rewrite huge `snd_rooms` atlases at runtime, do not duplicate reverbs, and do not use 343 m/s as a physically exact explosive-shock speed.

## Ordered implementation

1. **DATA-01** Snapshot hashes/version of source, native library, CFORM, room atlas and candidate material registry. Classify destructible-worthy authored surfaces, preserve unresolved acoustic surfaces.
2. **ASSET-01** Build three authored prefabs in Blender using blender-xray and IX-Ray SDK: glazed wooden window, simple wooden door, brick wall bay. Each needs intact/prefractured/optional damaged variants, low-cost collider approximations, stable piece IDs and anchors. Validate OGF/SDK export.
3. **NATIVE-01** Wrap or extend `CBreakableObject` with per-instance health/damage, fractured chunk activation, exposure-adjusted native explosion impulse, stable state change notifications and optional low-cost cosmetic emitters. Do not touch actor movement.
4. **MAP-01** Author one window/door/wall void in an isolated level test scene; replace it with an intact destructible panel. Validate renderer + static/dynamic collision + bullet/blast raycast against the same geometry.
5. **EVENT-01** Register versioned `DestructionEvent` with `assembly_id`, `material_id`, `old_state`, `new_state`, affected area/aperture, source impulse and deterministic seed. Apply only once.
6. **AUDIO-01** Resolve assembly to actual stable room/portal IDs from the matching installed metadata. Update a bounded runtime `SoundRoomPortal` override; verify closed/open rays, short reflections, material transfer, no wet-return gain hacks. If a room is uncalibrated, explicitly fall back.
7. **AI-01** For an actual traversable breach, guarantee the baked `level.ai` contains a path and dynamically gate it while intact. Hide/disable destroyed solid cover in line-of-sight and weapon traces. Do not claim a runtime navmesh rebuild.
8. **SAVE-01 / PERF-01** Save persistent assembly states but do not permanently save cosmetic debris. Pool physical pieces, cap concurrent shells, sleep quickly, demote distant/older debris to nonphysical effects. Verify short in-world spawns, frame cost, reload and acoustic closed->open delta.

### First playable acceptance

A grenade can hit one authored window, fracture the glass, detach the frame, send a handful of real pieces outward, visibly reveal the real hole, let bullets/sound pass after the collision state changes, update local acoustic routing, and survive save/reload. Then repeat with a brick wall panel and actual surviving cover.

## Isolation and provenance

The branch stores new LIMINAL specifications and original tooling only. It does not copy engine source, proprietary game assets or externally authored sound banks into public GitHub. Source edits/builds belong in a separate local worktree, never in ongoing audio/movement worktrees. Research references: EA/DICE Frostbite Destruction 2.0, Midway BlackSite cover-based destruction, UE3 2009 fracture-mesh workflows, and PavelBlend/blender-xray.
