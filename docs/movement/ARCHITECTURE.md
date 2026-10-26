# LIMINAL Movement

Single workspace: `D:\Development\LIMINAL-IX\movement`. Branch: `liminal/movement`.

- `src/`: pinned IX-Ray source with Movement overlays.
- `content/liminal-movement/`: one addon source plus required OGF/OMF/ANM.
- `tools/movement/`: source import, resource verification, build, install and smoke tools.
- `manifests/`: source revisions and resource hashes.
- `build/`, `game/`: generated output only.

The initial baseline snapshot is stored separately in `D:\Development\LIMINAL-IX\movement-snapshots`. Imported files in the original `source/integration` are not modified. Body Health is a read-only data dependency, not a Movement implementation.

Native names inside OGF and OMF must stay unchanged. Root `mod_system_liminal_movement.ltx` controls include order. No runtime references to temporary paths.
