# IX-Ray STRATUM — baseline, statuses and evidence

Read-only source reconnaissance: 2026-10-27. This file documents measured state without implying a new game installation or Windows compile.

## Repository baseline

* `main` (`f0b9470990bb3db5a8cd7689d1f58ef0151a1cc8`): license only — not a usable engine base.
* `liminal/movement` (`33fbaf7061baa3c2afaeefc3d4b5b967e6d5554a`): pinned IX-Ray source including `src/xrPhysics`, `src/xrSound`, `src/xrRHI`, `src/Layers`, Movement overlays.
* `liminal/audio` (`5d46d16124f18dfece5faffcf7094443e1299c9b`): original acoustic atlas tooling and contracts; not a unified native sound DLL build. The local acoustic source has additional **uncommitted** work; preserve it.
* `liminal/destruction` (`3539df4549515c8f6489504ced9b3b3fcb918f27`): ODE source audit and standalone NVIDIA PhysX 5.12 GPU PBD prototype. The GPU PBD simulation has **NOT** been linked to game runtime.
* `ixray.stratum`: based on the full `liminal/movement` source, not `main`.

## Evidence-based statuses

| System | Confirmed | Not yet established |
| --- | --- | --- |
| Source baseline | Full IX-Ray source inherited from Movement tree | Coherent STRATUM game build with all native DLLs |
| Authority | Standalone typed owner-registry source/test created | Runtime owner registration in `xrGame`/`xrPhysics`/`xrSound` |
| Physics | Original IX-Ray ODE source; standalone PhysX GPU PBD laboratory evidence | In-game PhysXBridge, gravity-stable PBD, Blast and save/load |
| Acoustic geometry | 42,652 logical rooms computed; nine axes geometry finished | 39,749 rooms physical material calibration; in-game v6 `physical=1` |
| Native acoustic route | Built-in IX-Ray sound, Resonance integration and separate tooling | End-to-end energy-correct outdoor/indoor propagation test |
| Movement | IX-Ray source and Movement module files; original OMF/OGF assets | Single accepted full-body runtime, verified retargeting, combined sound/movement build |
| AI | IX-Ray original A-Life/AI | STRATUM dynamic navigation/sound cognition |
| Rendering | D3D11 R4 RHI and shader compiler exist | D3D12 RHI/R5, DXC path, render graph, real D3D12 device smoke |

## What is allowed next (in strict stage order)

0. Pin toolchain, source hashes, uncommitted work inventory; construct one *separate* local game staging profile. Never overwrite independent previous builds.
1. Physics: repair source-verifiable ODE force/torque/fracture faults in targeted tests. Wire `AuthorityRegistry` and one physical event producer; compile and launch a real world-level smoke. Then attach PhysX PBD under a separate owner, without migrating all actor bodies.
2. Sound: port existing native acoustic work by source-level comparison; wire immutable event consumer, persistent material IDs, room geometry and eventual calibrated `snd_rooms`; test actual rendered PCM in a real level.
3. AI: events reach perception after acoustic propagation; dynamic obstruction becomes queryable without wholesale rebuilds of CFORM/game graph.
4. Movement: common actor skeleton/output pose; OMF path and cooked modern animation path; explicit handoff of listener/head transforms, no double-writers.
5. Rendering: preserve DX11 R4 and develop D3D12 backend in `xrRHI`; defer costly feature adoption until earlier gates pass.

## Independent test command

```bash
cmake -S tests/stratum -B build/stratum-contract
cmake --build build/stratum-contract
ctest --test-dir build/stratum-contract --output-on-failure
```

This tests *contracts only*. It does not demonstrate a running STRATUM game, as no native game build is included in this change.
