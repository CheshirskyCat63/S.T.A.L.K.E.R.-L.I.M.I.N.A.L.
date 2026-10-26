# LIMINAL Physics Core — read-only audit, 2026-10-26

**Strict scope: native IX-Ray physics only.** No graphics, particles, campfires, gameplay mod collections, audio engine edits, or installation work. Read-only source: D:\Development\LIMINAL-IX\source\pripyat-audio-v1\src. Dedicated GitHub branch: liminal/destruction.

## Architecture verified in the local sources

- xrPhysics/Physics.cpp: ODE contact friction is the product of two GameMtlLib material fPHFriction values, springs/dampers determine contact ERP and CFM; collision object callbacks may override these dynamically. See native wheel contact callback in xrGame/CarWheels.cpp. Initial fixed_step is 0.006666667 s, phIterations=18 and gravity=19.62 in source; gameplay-configured or actual running values are not measured here.
- xrPhysics/PHWorld.cpp: collision, PhTune, island integration, PhDataUpdate and cleanup phases; elapsed time can cause multiple Step calls. Do not clamp or multithread globally without tests.
- xrPhysics/PHIsland.cpp: uses exact dWorldStep on exact-preferred islands below the 30-joint threshold; otherwise dWorldQuickStep.
- xrPhysics/PHJoint.cpp: ball, hinge, hinge2, slider, motor, axis spring/damping, limits, joint feedback and breakable joints.
- xrPhysics/PHFracture.cpp, PHShellSplitter.cpp and xrGame/PHDestroyable.cpp: fracture, split, physics-body fragments. PHDestroyable can spawn a whole game entity for every fragment: unsuitable for thousands of cosmetic shards.
- xrPhysics/PHContactBodyEffector.cpp: applies velocity-opposing drag derived from fFlotationFactor where slow/liquid materials overlap. This is NOT water buoyancy or a liquid solver.
- xrGame/NoGravityZone.cpp: per-object gravity switches; xrGame/AnomalyGravity.cpp::SetForce currently overrides physical velocities, NOT force integration.
- xrPhysics/PHDisabling.cpp and DisablingParams.cpp: thresholds, two-level object sleep, configurable per object, default L2frames=64.
- Lua physics_shell_scripted.cpp and physics_element_scripted.cpp expose apply_force and physics elements; physics_joint_scripted.cpp exposes joint damping, limits, motor force and velocity. Build experimental fixtures in Lua before new C++ interfaces.

## Source bugs and compatibility risks — NOT PATCHED

P0. xrPhysics/PHJointDestroyInfo.cpp: constructor computes m_sq_break_torque, but Update checks both torque-feedback vectors against the FORCE threshold, leaving torque threshold unused. Verified same in IX-Ray public default and OpenXRay dev. Require focused force-only/torque-only regression before change.
P0. xrPhysics/PHFracture.cpp: in the first-part impact branch the torque is added to second_torque instead of first_torque. Check gravity contribution also using first-part mass for both contributions. Confirm with controlled mass-asymmetric geometry before changing.
P0. xrGame/Explosive.cpp: explosion-to-target vector is not normalized before analytic upward impulse expression; real OpenXRay PR #2119 fixes a reproduced NaN case. Unit tests: above/below/diagonal/coincident vectors.
P1. OpenXRay PR #2157 reports contact-effect use-after-free after body destruction; compare our local body destroy and effector pools, then test targeted regression.
P1. Physics-step burst after a stall requires profiling; NEVER silently reduce global accuracy.

## Small to medium physics-only experiment portfolio

1. Native breakable-joint threshold corrections. CHOICE: P0 / small. Evidence: PHJointDestroyInfo, PHJoint. Acceptance: force and torque thresholds work independently; separate test for ten chained bodies.
2. Correct first/second fracture impulse assignment. P0 / small-medium. Evidence: PHFracture. Acceptance: mirror-image impacts split symmetrically; no NaNs and finite masses.
3. Normalize blast impulse direction and lifecycle fixes. P0 / small-medium. Evidence: Explosive.cpp, external PR #2119 and #2157. Acceptance: 100 controlled blast positions; teardown and replay without invalid body access.
4. Jointed mechanical assemblies: pendulum, hinge-driven door/panel, 10-link chain, controlled break point and elastic stop. E1 / small-medium. Existing PHJoint + Lua joint tuning. Measure island solve time and joint feedback. Does not need full world destruction.
5. Mass-aware force fields: gravity/vortex/wind by physical force, not set_LinearVel. E2 / small-medium. PHShell::applyForce or Lua bindings, anomaly coordinates; measure different acceleration for 1 vs 10 kg and zero force outside region.
6. Runtime material contact states: static surface dry/wet/sticky with per-contact friction and damping, rather than rewriting gamemtl.xr or weather shaders. E3 / small-medium. Physics.cpp contact callback and same car-wheel technique; incline-plane travel distance and no global material-ID changes.
7. Water rigid-body buoyancy: use known water level/bounds and body mass/volume to compute per-step submerged fraction, hydrostatic upward force and existing drag. E4 / medium. NOT SPH/FLIP fluid solver; compare light/heavy box floating/sinking.
8. Physical blast transfer to hinged walls/debris with ODE impulses; existing CExplosive occlusion and measured exposure. E5 / medium. Controlled one-body and chain experiments, no custom mesh solver.
9. Physics sleep scaling for temporary debris, per-object disabling and pooling rather than globally altering world dt/iterations. E6 / small-medium. Measure active body count, contact count and complete physics time in 10/50/200 body fixtures.
10. Optional runtime CFORM negative-mask *physics only*: localized ray and contact rejection with invalidation of stale cached triangles, no rebuild of entire CFORM. E7 / medium-high. Static CDB and ODE contact pipeline modifications, NOT first priority.
11. New physics backend Jolt: research-only/high to extreme. Prefer benchmarking methods, individual contact/data lifetime improvements and benchmark harness. Do NOT wholesale replace ODE.

## Real external engine experiments, statuses checked 2026-10-26

- https://github.com/OpenXRay/xray-16/pull/2149 — draft unmerged, optional Jolt static collision queries, semantic parity tests; workload-dependent results.
- https://github.com/OpenXRay/xray-16/pull/2164 — draft unmerged, Jolt dynamics bridge that preserves ODE API, increased idle overhead.
- https://github.com/OpenXRay/xray-16/pull/2165 — draft unmerged, repeatable real-level physics measurement harness; useful without backend migration.
- https://github.com/OpenXRay/xray-16/pull/2166 — draft unmerged, native Jolt bodies/constraints/characters, shared CDB mesh and workers: extremely broad native C++ changes.
- https://github.com/OpenXRay/xray-16/pull/2167 — draft unmerged, optimized native contact handling and benchmark reports.
- https://github.com/OpenXRay/xray-16/pull/2168 — draft unmerged, further optimized native Jolt: on authors' CoP 12-ragdoll test, full physics step 0.706 ms ODE vs 0.645 ms Jolt, but complete mean frame 3.117 ms ODE vs 3.248 ms Jolt; other games regress and shutdown 0xC0000005 remains unexplained. Different machine, not our LIMINAL benchmark.
- https://github.com/OpenXRay/xray-16/pull/2157 — draft/unmerged lifetime bugfix for stale physical effectors, with ASan fixture.
- https://github.com/OpenXRay/xray-16/pull/2119 — unmerged blast direction normalization fix with actual grenade reproduction.
- https://github.com/ixray-team/ixray-1.6-stcop/blob/default/.github/CHANGELOG.md — IX-Ray already fixed thread-safe PHJoint creation/destruction in 1.3.5; avoid duplicate feature patches.
- https://ode.org/wiki/index.php/Manual — original ODE joints/rigid-body API; XI-Ray wrappers differ.

## Precise limits

NO general soft body, finite element masonry destruction, full volumetric SPH/FLIP fluid simulation, fully dynamic CFORM topology, free parallel game physics or ready-to-merge Jolt migration is demonstrated in this current native code. Do not claim that fFlotationFactor is true buoyancy. Do not claim a physics step or game runtime PASS from source inspection.

## Next tests without major pain

A. Isolated physics fixture, record actual fixed step/iterations/contacts/islands/peak time; no production profile edits.
B. Three tiny source fixes with force/torque/blast tests; life-cycle fix only after confirming local matching code.
C. Three physics scenes: breakable joint assembly; vortex on bodies of different mass; inclined plane changing dynamic contact friction. Prefer existing Lua force/joint controls and one native callback when necessary.
D. Only if E1-E3 work, add water buoyancy and cheap debris sleeping. Reconsider collision backend after target-level benchmarks, not synthetic-only marketing numbers.

**Status:** architecture and sources audited read-only. No patches applied, no local builds, no installed experiments, no performance verification on user's hardware in this work. Entirety of game/audio/movement profiles remains untouched.
