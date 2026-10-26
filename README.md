# IX-Ray STRATUM — S.T.A.L.K.E.R. L.I.M.I.N.A.L.

This is the **integration branch** `ixray.stratum`, derived from the complete IX-Ray-based source snapshot in `liminal/movement` (`33fbaf7061baa3c2afaeefc3d4b5b967e6d5554a`), **NOT** the license-only `main` branch.

The purpose of STRATUM is to retain IX-Ray game formats, content, scripting, gameplay and proven native functionality while adding deliberately bounded physics, acoustic, AI, body/animation and modern-rendering subsystems.

**Fixed development order:** physics -> acoustics -> AI -> movement/animation -> rendering.

## Integration rules

See [docs/stratum/ENGINE-CONTRACT.md](docs/stratum/ENGINE-CONTRACT.md) for the six binding architecture rules and [docs/stratum/INTEGRATION-STATUS.md](docs/stratum/INTEGRATION-STATUS.md) for branch provenance, stage gates and evidence.

`src/stratum/core/` contains a **standalone C++17 contract prototype**, including an owner registry and a versioned physical contact event. Nothing in this commit claims these contracts are already hooked into `xrGame`, `xrPhysics`, `xrSound`, or the game loop.

To compile/run the standalone ownership contract test on a machine with CMake and a C++17 compiler:

```bash
cmake -S tests/stratum -B build/stratum-contract
cmake --build build/stratum-contract --config Release
ctest --test-dir build/stratum-contract -C Release --output-on-failure
```

## Safety boundaries

* Do not overwrite `liminal/audio`, `liminal/movement`, or `liminal/destruction`; their local Windows working trees may include uncommitted changes.
* Do not merge or copy a previously compiled `xrGame.dll` into a different engine source baseline: all native binaries must come from one coherent source+build profile.
* The public repository does **not** automatically grant the right to redistribute upstream engine source, paid/licensed tools, GAMMA/STCoP assets or original game data.
* `D3D12`, `PhysX GPU PBD` in the game, full room atlas and full-body animation bridge are **targets**, not verified integration.
