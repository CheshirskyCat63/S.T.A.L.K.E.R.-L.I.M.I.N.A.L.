# STRATUM — six non-negotiable integration rules

Architecture contract, first version: 2026-10-27. These rules are binding targets; native game-side implementation is not claimed by this document.

## Rule 1 — One authoritative source baseline

Build the entire game from a single tracked IX-Ray C++ tree, pinned upstream/base commit and one compiler/toolchain configuration. The branch `ixray.stratum` was created from the complete `liminal/movement` source snapshot (`33fbaf7061baa3c2afaeefc3d4b5b967e6d5554a`) rather than license-only `main`. Audio/physics changes must be **ported at source level** and reviewed against this version; do not transplant binary DLLs or reset dirty local workspaces. Keep isolated staging/game profiles.

**Acceptance:** the matched `xrEngine`, `xrGame`, `xrPhysics`, `xrSound`, `xrRHI` binaries load a real level from a single recorded source manifest; debug/runtime dependencies match.

## Rule 2 — One owner per state domain, entity and generation

Distinct state domains must have exactly one authoritative writer for each live entity/generation:

| State | Authoritative owner initially | Consumers |
| --- | --- | --- |
| Physical body pose, contacts, velocity | ODE **or** PhysX, never both | AI, sound, rendering, body |
| Sound voice/lifetime/final PCM | `xrSound` | output device |
| Final actor skeleton pose | STRATUM animation system **after** integration | camera, sound listener, weapon, hit zones |
| Static CFORM geometry | IX-Ray world / CFORM | queries, physics, acoustics, navigation |
| Dynamic collision geometry | explicit STRATUM overlay owner | queries, sound, AI |
| NPC intention/decision | IX-Ray A-Life/AI plus authorized STRATUM AI behavior | movement commands |

Owning the final skeleton pose does **not** automatically transfer the physical movement controller; ODE remains owner of actor collision until explicit measured migration. The intent producer is not the pose writer. Movement, audio and physics exchange **immutable snapshots and events**, not mutable copies of each other's state.

`src/stratum/core/AuthorityRegistry.hpp` is a minimal no-engine-dependency contract. It is not yet connected to runtime producers. Authority transitions require expected-current-owner validation at an agreed simulation boundary; do not swap active solver bodies mid-step. A failed transition leaves the original owner intact.

**Acceptance:** one owner is registered per live entity+generation+domain; duplicate assignment and unauthorized transfer fail; no simultaneous ODE/PhysX transforms for one live body; audio never writes body/pose.

## Rule 3 — Delegate algorithms, own the world semantics

IX-Ray owns gameplay, entity lifecycle, save/load and original asset semantics. External SDKs are **replaceable computation backends**, not owners of the game world:
* ODE/PhysX: contact and constraint solvers.
* PhysX GPU PBD: isolated particle systems, after runtime bridge verification.
* NVIDIA Blast: optional pre-fractured structures, after collision ownership and native tests.
* Existing `xrSound`: sound voice/mix; a propagation library is only a candidate subordinate calculation.
* Existing OMF player / future ozz: per-clip evaluation/blending; STRATUM decides the canonical output pose and mechanics.
* R4 D3D11 now; proposed D3D12 backend later, after prior stage gates. Do not describe it as currently shipped.

External adapters expose a small versioned C++ contract, use explicit lifetimes and retain SDK-specific types behind the adapter boundary. Never couple the audio or AI implementation directly to NVIDIA, ozz or Direct3D SDK structures.

**Acceptance:** SDK can be disabled without breaking IX-Ray native fallback for unaffected entities. No Python interpreter required by game runtime. ABI/toolchain matched.

## Rule 4 — Preserve native formats; convert at the boundary

Keep native `.ogf`, `.omf`, `.anm`, `.ltx`, `gamemtl.xr`, `level.cform`, `level.snd_rooms`, `level.snd_env` and existing game/save data readable. New FBX/glTF/GLB animation sources are *offline* imported/retargeted, not assumed runtime-readable. A future cooked asset format must be explicitly versioned and must not replace compatible original resources wholesale.

Maintain distinctions between *persistent material ID*, *runtime GameMtlLib index* and *CFORM triangle material index*. A material is not a wall assembly; thickness cannot be inferred from one triangle.

**Acceptance:** original CoP test level and original GAMMA motion resources load unmodified; one modern test animation can be cooked and blended with its validated rig.

## Rule 5 — One coordinate/time/event contract

Define all cross-system vectors in an explicitly tagged IX-Ray world-coordinate basis, translations in metres, velocity in m/s, impulses in N*s, time intervals in seconds, and authoritative integral simulation ticks. Do not assume ODE/PBD/audio/animation run at identical rates: use queued sampled states with timestamps and explicit interpolation/extrapolation limits.

Events carry unique IDs, source IDs, generation, stable material IDs, validity flags for measured quantities, and monotonically ordered frame/tick stamps. An absent impact impulse is **unknown**, not a fabricated physical measurement. Event queues are bounded; consumers must record drops/truncation. Contact clusters are aggregated for audio; not every debris/PBD particle spawns an audio voice.

**Acceptance:** one recorded shot/contact traces through physics -> material -> sound/AI using the same event ID and spatial basis, with measured latency and no duplicated delivery.

## Rule 6 — Feature gates and reproducible real-world acceptance

Per subsystem: original baseline hashes -> isolated build -> small deterministic fixture -> in-engine real-level smoke -> profile/performance -> explicit accept/revert. Never label an offline fixture, menu launch, exported DLL set, or uncalibrated acoustic geometry as a game PASS. Do not promote geometry-only `snd_rooms` or PhysX PBD laboratory results into release.

Tests stay short and focused; do not unnecessarily rebuild the complete acoustic geometry or run huge gameplay matrices. Keep old working profile and stable R4 renderer as fallback. Full development sequence: **PHYSICS -> SOUND -> AI -> MOVEMENT -> GRAPHICS/D3D12**.

**Acceptance:** for each stage, record commit, binary hashes, exact profile, native runtime log, game-world event and regression against previously accepted stages.

## Global integration status

The first delivery in `ixray.stratum` is the branch plus this architecture/contract scaffolding. Rules 2–6 are **specified**, not yet wired into the running game. See INTEGRATION-STATUS.md.
