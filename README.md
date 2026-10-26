# S.T.A.L.K.E.R. L.I.M.I.N.A.L. — Native physics / destruction

**This branch owns engine physics experiments ONLY:** native IX-Ray/ODE dynamics, contacts, joint feedback, force fields, spring/damping, fracture, explosion impulses, buoyancy experiments and physical runtime collision. It never replaces the separate audio, movement or camera systems.

## Current research

[Deep IX-Ray ODE physics-core audit (26 October 2026)](docs/ODE-CORE-AUDIT-20261026.md).

Read-only local source inspected:
D:\Development\LIMINAL-IX\source\pripyat-audio-v1\src\xrPhysics,
D:\Development\LIMINAL-IX\source\pripyat-audio-v1\src\xrGame.

Most productive direction:
1. Fix verified physical force/torque/fracture defects with standalone fixtures.
2. Exercise existing Lua/ODE rigid-body force and joint controls before adding new engine code.
3. Controlled physics-only experiments for jointed assemblies, real force fields, dynamic contact friction, approximate rigid-body buoyancy and physical debris sleep.
4. Benchmark actual LIMINAL physics and use OpenXRay Jolt experiment as a research reference, not a wholesale engine replacement.

Not in scope: particle packs, rendered bullet impact marks, graphical surface clipping, gameplay QoL, mod lists, fire shaders, fake puddles or artificial complex simulation.

**Status:** research findings and external source comparison only. No local source patch, compilation or game-runtime physics experiment accepted as complete. Never reset or modify dirty audio/movement local source trees.

The previous world-destruction visual design remains in Git history, but is not the current physics-core mandate.
