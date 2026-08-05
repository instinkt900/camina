# Roughness spheres

`spheres.gltf` is a row of seven metal spheres, generated rather than modelled. Every sphere
carries the same near-white metal at a different roughness, from 0.05 to 0.95.

The sandbox needs this because image based lighting is the difference between a smooth metal
and a rough one. A cooked environment reaches a surface through three parts: the irradiance
coefficients, the cubemap prefiltered by roughness across its mips, and the shared lookup
table. Every other model in the sandbox is one material at one roughness, so a wrong
prefiltered chain would read as a picture that is merely a little dull, and nothing would
point at the cause.

The row makes the failure visible instead. Read it left to right. The reflection has to get
softer at every step, and the sphere at 0.05 has to show the shape of the room while the one
at 0.95 shows only its color. A chain that is box filtered rather than GGX filtered leaves the
right-hand spheres too sharp. A mip level that roughness does not choose leaves the whole row
identical.

Each sphere is a separate node with a separate material, and all seven point at the same
accessors. So the geometry is in the file once and the cooker writes seven meshes over it.
Seven materials is the point of the model, and a material belongs to a submesh.

The buffer is a separate `spheres.bin`. The two flat panes next door ride in a data URI, which
keeps a small model to one text file. This one is twenty-four kilobytes of vertices, so it
takes the two-file form the Flight Helmet uses.

The `.meta` sidecar is in version control. The identity in it is what the manifest resolves
the prefab from, so deleting it would break the reference `main.scene` holds.
