# Humanoid resource preparation

`prepare_resources.py` is the single offline importer for the native actor47,
HUD donor42 and rigid target61 contracts in `profile.json`. It does not install
files, change original OMF/ANM, launch the game or build binaries.

```powershell
python -B tools/humanoid/prepare_resources.py `
  --source C:/staging/native/meshes/actors/stalker_hero/stalker_hero_stalker_1.ogf `
  --source-visual 'actors\stalker_hero\stalker_hero_stalker_1' `
  --donor D:/Development/LIMINAL-IX/play-integration/ixr_addons/gamma-original-hands-library/meshes/liminal_gamma/original_hands/wpn_hand_loner_sunrise.ogf `
  --output-dir C:/staging/humanoid/stalker_hero_stalker_1
python -B tools/humanoid/validate_resources.py C:/staging/humanoid/stalker_hero_stalker_1/manifest.json --check-determinism
```

Use a separate output directory for each source. A run owns its output manifest
and mapping file; it does not merge an unrelated previous run. Only models in
`manifest.models` with `PREPARED_STATIC_VALIDATED` are eligible for installation.
The sibling `mod_system_liminal_humanoid.ltx` contains normalized source-to-target
visual mappings, without extensions. The JSON manifest is the audit receipt;
the runtime need only consume the LTX mapping and the prepared OGF.

The original 47 bone names, indices and parent relations remain. The importer
adds `bip01_[lr]_finger3/31/32`, `finger4/41/42`, and `forearm_twist`. Source
`finger2` is a grouped middle/ring/little influence in native47. The target
rebases its pivots to the independently modelled middle digit. This changes the
target bind skeleton, not the source animation skeleton or neutral surface.
Source `bip01` is a HUD root; `lead_gun` is an item anchor. Neither is mapped to
the identically named actor root/container as a human joint.

The surface gate uses the highest-detail progressive triangle window, welds
only coincident position samples for analysis, and requires four non-thumb
branches across three consecutive normalized distal cuts. Thumb and index
identity are checked using original influences. Digit ownership is propagated
geodesically across that existing hand surface. It does not create polygons or
substitute a different hand model. An inseparable surface is rejected with
`GEOMETRY_FINGERS_INSEPARABLE` and its measured cut/component evidence.

New finger weights remain in their digit region; twist weights are restricted
to the existing forearm influence and longitudinal forearm interval. Vertices
outside modified regions retain their original weights. The maximum is four
influences. New joints receive non-physical shapes and an OBB derived from their
actual influenced vertices. Original middle-joint OBBs are recomputed after
rebinding. The original surface positions, normals, tangents, binormals, UVs,
triangle-index payloads, materials and progressive LOD windows are preserved
byte-for-byte. Model and child neutral bounds remain valid and unchanged.

The output is `MT_SKELETON_RIGID` and contains no 47/42-bone OMF references or
skeletal LOD reference. It is a pose receiver, never an alternative animation
controller. The runtime must calculate inverse bind transforms from its 61
bind records and update animated bounds when applying a pose.

`validate_resources.py` independently decodes output bytes, compares protected
surface fingerprints and neutral skinning, checks anatomy confinement, checks
all 14 added joints have actual vertex influence, verifies hashes and can rerun
the importer to require byte-identical output. `PASS_STATIC` does not establish
runtime pose-transfer quality, collision agreement, or acceptance of another
outfit. Donor validation reads its skeleton only: donor geometry is neither
converted nor used to establish the target surface gate.
