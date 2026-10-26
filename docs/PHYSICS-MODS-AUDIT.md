# LIMINAL Destruction — X-Ray physics and cinematic effects mod audit

**26 October 2026.** Independent branch: liminal/destruction. This document is an evaluation, NOT an installation report. All descriptions below refer to externally published addons or read-only local source inspection; no addon has been installed or performance-tested in this branch.

## Accepted direction

**Cinematic, cheap, material-aware:**
1. Normal bullet impact: authored 2–5 cm shallow *visual* chipped geometry in suitable masonry, angular flakes, short subtle dust; wood splinters along grain; metal dents/sparks. No ODE simulation for every chip.
2. Actual bullet penetration: cheap see-through hole through rendered surface with entry/exit rims, depth and dynamic shadows. Physics for player and props need not change for this tiny perforation. Use native bullet manager penetration result, never just any impact.
3. Grenade/RPG structural breach: preauthored Blender masks with exact contour, world transform, area and exposed edge mesh. Physically convincing but directed chunks. Static CFORM file NOT rebuilt; runtime negative masks for renderer and the needed static ray/ODE queries, including cache handling, only for larger holes where traversal is allowed.
4. Acoustics: every authored breach knows its **exact dimensions and contour**. Emit a single BreachEvent carrying world-space shape, surface material, affected geometry, and open/closed state. Existing LIMINAL audio may query room membership and update local direct/transmitted/reflected routes. Never re-discover guessed dimensions and never rebuild level.snd_rooms. Full physically calibrated audio is still pending; geometric truth alone does not guarantee calibrated reverb.
5. Fire/liquid visuals are chosen event-driven states, not a simulation of chemical burning or fluid dynamics unless separately proven.

**Performance:** preloaded mask activation 0.1 ms is a TARGET for benchmarking, NOT proven frame/GPU/ODE cost. Pool small pieces; use short FX; avoid thousands of ALife entity IDs for debris.

## Verified native code / assets on Windows computer

Root: D:\Development\LIMINAL-IX\source\pripyat-audio-v1.

- xrGame/DestroyablePhysicsObject.cpp and CPHDestroyable implement health/damage/splitting of physobjs.
- gamedata/configs/models/objects/box_wood_01.ltx contains 7 authored fragment names, native particle and sound, damage/immunity/impulse settings. box_metall_01.ltx contains two, box_paper.ltx one.
- xrGame/BreakableObject.cpp and xrPhysics/PHFracture.cpp already switch pre-fractured components into native ODE dynamic shells.
- xrGame/Level_bullet_manager_firetrace.cpp::ObjectHit handles armor piercing, material resistance, ricochet and density-based slowing. In this function fShootFactor approximately zero means full penetrability, in conflict with simple header description; respect executed behavior. Note ObjectHit return value is not a penetrating-hit Boolean; hook the specific resolved branch.
- xrCore/Collision/xrCDB_ray.cpp and xrPhysics/tri-colliderknoopc/dSortTriPrimitive.h read static CFORM triangles; neither contains a ready-made negative breach mask filter.
- R4 pixel and shadow alpha clipping exists in select shaders, but global static geometry clipping integration remains to be implemented.
- gamedata/configs/zones/zone_campfire.ltx provides native fire visuals, area damage and sound, **not** combustible material spread.
- Separate game install play-native-acoustics contains ixray-puddles-v2.1. No true liquid physics is claimed.
- Acoustic material candidate 74 IDs and 1995 pairs; deployed active VFS and physically calibrated atlas remain separate audio branch concerns. Do NOT overwrite gamemtl.xr.

## Verified mod candidates

**IX-Ray effort:** LOW=configs/ready OGF; MEDIUM=adapted models, scripts or event connections; HIGH=C++ renderer/physics and engine compatibility. These are estimates, NOT runtime measurements.

### Objects / crates / destructible world

| Upstream | Capability | Benefit / cost |
|---|---|---|
| Existing native CPHDestroyable + boxes (installed source) | 7-piece wood box; metal/paper parts, ODE, break sounds and particles | P0, LOW; reuse not rewrite |
| [Dynamic Replace](https://www.moddb.com/mods/stalker-anomaly/addons/dynamic-replace) | Replacement models for dynamic props and fragmented crates | P0, LOW–MEDIUM, donor models subject to provenance |
| [Wooden Box Collision](https://www.moddb.com/mods/stalker-anomaly/addons/wooden-box-collision) | Crates break from jumping/collision | P1, LOW configs |
| [Box Breaker](https://www.moddb.com/mods/doctorx-call-of-the-zone/addons/box-breaker) | Knife breaks wood/metal/paper containers | P1, LOW immunities |
| [Dynamic Objects v1.05](https://www.moddb.com/mods/stalker-anomaly/addons/dynamic-objects-v1-05) | Randomly placed movable objects; new game for source addon | P2, MEDIUM, environment clutter/spawns |
| [No Fake Collision](https://www.moddb.com/mods/stalker-anomaly/addons/no-fake-collision) | Removes collision on problematic fake surfaces globally | Reference ONLY. Conflicts with LIMINAL custom gamemtl.xr |
| [Frozen Zone](https://stalker-mods.su/publ/s_t_a_l_k_e_r_quot_zov_pripjati_quot/frozen_zone/2-1-0-178) | Historical X-Ray project with breakable materials and vehicle parts, source and SDK archives | P0 study, HIGH engine archaeology, canceled prototype not drop-in |

### Impact particles / realistic-looking fake physics

| Upstream | Capability | Benefit / cost |
|---|---|---|
| [HollywoodFX 3.1](https://www.moddb.com/mods/stalker-anomaly/addons/hollywoodfx3) | Gun-hit dust, sparks, debris, explosions; MinimalFX/RealisticFX/HollywoodFX; deliberately reduces grenade fragment simulation to avoid frame spikes | **P0**, MEDIUM; copy cinema principles, select small effects |
| [Cinematic VFX 3.7](https://www.moddb.com/mods/stalker-anomaly/addons/cinemavfx3-7) | High-res explosion, fire, impact, muzzle, anomaly particles | P0/P1, MEDIUM; curated alternatives, NOT whole-pack conflict |
| [Particle Enhancement for SoC](https://www.moddb.com/mods/particle-enhancement) | Classic native X-Ray particle improvements (impacts, fires, explosions) | P1, LOW–MEDIUM baseline |
| [Painter of the Zone](https://www.moddb.com/mods/stalker-anomaly/addons/painter-of-the-zone) | Material-aware parallax decals, hit VFX, gore & blood | P1, MEDIUM–HIGH; material wallmarks, not actual geometric hole |
| [Hit Impulse Restore](https://www.moddb.com/mods/stalker-anomaly/addons/hit-impulse-restore) | Restores cinematic hit impulses in ammo configs | P1, LOW. Tune props, avoid unwanted ragdoll knockback |
| [wepl Hit Effects](https://www.moddb.com/addons/wepls-hit-effects) | NPC/player body/armor hit sounds & shake, NOT wall breakage | Combat/audio owner, MEDIUM |
| [Active Ragdolls](https://www.moddb.com/mods/stalker-anomaly/addons/active-ragdolls) | Experimental post-death NPC physics animation, altered hit impulses | Combat owner, MEDIUM |
| [New Ragdoll Preset DUX/FVM](https://www.moddb.com/mods/stalker-anomaly/addons/new-ragdoll-preset-for-duxfvm) | Joint and mass preset for exact GAMMA actor meshes | Combat owner, MEDIUM–HIGH; model-specific |
| [Item Model Drop After Usage](https://www.moddb.com/mods/stalker-anomaly/addons/items-model-drop-after-usage) | Dropped item meshes, physics detail requires archive inspection | P3, MEDIUM |
| [EFT Grenade Collision Sounds](https://www.moddb.com/mods/stalker-anomaly/addons/eft-grenade-collision-sounds) | Material-specific grenade bounce sounds | Audio owner, MEDIUM, modded-exe dependency |

### Fire, water, rain, hazards

| Upstream | Capability | Benefit / cost |
|---|---|---|
| [IX-Ray Puddles 2.1](https://github.com/ixray-community/ixray-puddles) | Native 3D puddles placed in original Call of Pripyat maps | P1, LOW–MEDIUM, already present in a neighboring profile; NOT fluid sim |
| [Screen Space Shaders](https://www.moddb.com/mods/stalker-anomaly/addons/screen-space-shaders) | Rain puddles/wet buildup and drying, water refraction/ripples, interactive grass/wind | P1, HIGH for selective IX-Ray R4 shader port; no Anomaly exe override |
| [IRDVD Dynamic Visor Drops](https://www.moddb.com/mods/stalker-anomaly/addons/irdvd-dynamic-visor-drops) | Visor drops drift with wind/gravity, dry under cover | P3, MEDIUM optical detail; not world liquid |
| [Molotov Cocktails](https://www.moddb.com/mods/stalker-anomaly/addons/152-molotov-cocktails/) | Fire thrown bottle with damaging area | P2, MEDIUM event-based flame patch, NOT flame propagation |
| [RE:DONE Night Campfires](https://www.moddb.com/mods/stalker-anomaly/addons/redone-night-campfires) | Time/rain-controlled campfire lighting | P2, LOW–MEDIUM simple weather exposure rules |
| [Campfires Placeable](https://www.moddb.com/mods/stalker-anomaly/addons/campfires-placeable-01) | Placeable working campfires and firewood | P2, MEDIUM fire anchor concept, skip survival chores |
| [Dynamic Anomalies Overhaul](https://www.moddb.com/mods/stalker-anomaly/addons/dynamic-anomalies-overhaul-dao-read-description-please) | Dynamic electrical, gravity, thermal, chemical and acid-pool-like hazards | P2, MEDIUM–HIGH partial reaction behaviors; not real liquids |
| [Anomalious Grenade](https://www.moddb.com/mods/stalker-anomaly/addons/anomalious-grenade) | Burner, Mincer, Electra, Web grenades | P2, MEDIUM localized fire/gravity/electric impacts |
| [TB Thermal Anomalies](https://www.moddb.com/mods/stalker-anomaly/addons/tbs-fairer-thermal-anomalies-v1-0) | Ember and crackle effects on thermal fields | P2, LOW small fire FX |
| [RE:TUNE Grenade SFX](https://www.moddb.com/mods/stalker-anomaly/addons/retune-grenade-sfx) | Explosion character and additional debris visual effects | MEDIUM, audio coordination required |

### Relevant but excluded from core physical-world module

- [Breakable Devices 2.0](https://www.moddb.com/mods/stalker-anomaly/addons/breakable-devices): electronics failure with hits, electrical anomalies, radiation and repair parts; not breakable crates, conflicts with anti-chore design.
- [Penetrable Walls](https://www.moddb.com/downloads/stalker-forgotten-zone-1-6): only verified as a named addon in a modpack changelog. Actual visible holes or implementation NOT confirmed.
- [xrRazom 1.4](https://www.moddb.com/mods/stalker-anomaly/addons/xrrazom-stalker-anomaly-co-op-slim): multiplayer *sync* of object/barrel/corpse/physics; not an additional simulation.
- [FuzzRecoil](https://github.com/emgComplex/FuzzRecoil): physics-like 3D recoil, belongs to liminal/movement, NOT world destruction.
- [Akagi object-ID overflow fix](https://www.moddb.com/mods/stalker-anomaly/addons/akagis-solution-to-the-65534-ids-overflow): caution on object lifetime. Temporary shards must not each become persistent ALife objects.
- **All S.T.A.L.K.E.R. 2 Unreal Engine .pak physics mods excluded.**

## Explicit unresolved items

No verified X-Ray addon in this audit simulates generic fluid flow with a real fluid solver, automatic burning progression through arbitrary wooden meshes, or edits entire compiled CFORM mesh at runtime with reliable renderer+ODE+nav+HOM synchronization. These need small purpose-built cinematic overlay states if desired, not a major engine rewrite.

## Research/implementation order

1. Native crate physics + Dynamic Replace evaluation; particle comparison HFX Minimal/Realistic and Cinematic VFX, select moderate looks.
2. Single cheap material-scoped hit mask and convincing chips on concrete/wood; own reusable authored geometry, low-particle setup.
3. Visually see-through penetrating small holes with zero new physical bodies; static lightmap caveat.
4. One big authored breach with exact geometry event and targeted native collision and sound recalculation, check save/load and cache invalidation.
5. Weather-dependent fire-zone/standing-puddle cinematic states, reuse native campfire and available IX-Ray puddles.
6. Examine Frozen Zone sources and SDK in a separate sandbox only if baseline works. Never blend its entire old engine into IX-Ray.
7. Perform measured in-game acceptance, local cost budgets, no hard 0.1 ms promises.

## Redistribution and compatibility

This branch stores original LIMINAL planning only, and links to external projects. Reuse requires review of third-party licenses and asset rights, even for a free mod. Do not modify or overwrite local audio/movement source profiles without separate authorized integration.
