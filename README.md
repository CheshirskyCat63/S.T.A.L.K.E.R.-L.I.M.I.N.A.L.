# S.T.A.L.K.E.R. L.I.M.I.N.A.L. — Movement

Independent movement/animation/bodycam workstream: `liminal/movement`, branched from `main`.

**Ownership:** actor first-person body and bones, incoming authored weapon-specific animations (do not replace them with generic clips), camera/head turning, movement and stance transitions, leaning, weapon mass/inertia, recoil, procedural links, gear load and injury interactions. The goal is consistent skeletal attachment and motion relationships, not per-weapon hand-offset hacks.

**Isolation:** changes to sound engine, sound samples, acoustic atlas, materials, `xrSound`, `level.snd_rooms` or `level.snd_env` belong exclusively to `liminal/audio`. This branch does not alter or depend on the sibling working tree.

**Native integration contract:** movement publishes the actual actor pose and listener position/orientation through existing IX-Ray interfaces. Audio consumes the authoritative ear transform without owning or rewriting movement animations. Branch integration must preserve both systems independently until a tested common integration snapshot is prepared.

**State:** branch created. This README establishes scope; it is not a movement build or completion claim. Existing local actor/Bodycam sources remain untouched.
