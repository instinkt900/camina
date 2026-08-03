# Crate

`crate.gltf` is a unit cube, generated rather than modelled. It carries the vertex table
that `render/cube_pass.cpp` held before that pass was retired, so the shape the sandbox
draws did not change when the crates became mesh entities.

Every face winds counter-clockwise seen from outside and the texture origin is top-left,
which is what `DESIGN.md` section 3 fixes and what glTF already agrees with.

The buffer rides in a data URI, so the model is one text file rather than a `.gltf` and a
`.bin` that have to travel together. A generated cube is small enough for that to be
reasonable, and the Flight Helmet next door shows the two-file form.

`crate.png` came from `src/render/content/`, where the retired pass read it.

Both files carry a `.meta` sidecar in version control. The identity in a sidecar is what
`crate.prefab` derives its mesh GUID from, so deleting one would break that reference.
