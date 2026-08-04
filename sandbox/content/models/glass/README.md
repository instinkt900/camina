# Glass

`glass.gltf` is two flat panes, generated rather than modelled. Each pane is one quad with a
`BLEND` material, and the two carry different tints and different alpha values.

The sandbox needs this because nothing else in it is transparent. A blended surface takes a
different path through `render::MeshPass` than an opaque one: it draws from a second
pipeline that blends and does not write depth, and it draws after every opaque surface, back
to front. Without a transparent asset in the scene that path would ship untested.

The two panes are separate meshes on purpose. `MeshPass` sorts by the bounds of a whole mesh,
so two panes that shared one mesh would share one sort key and the order between them would
never be tested. Issue #99 holds the per-submesh bounds this works around.

The panes overlap on screen from the starting camera. That overlap is the part of the picture
that changes when the sort is wrong, so it is the part to look at after a change to the
blending.

Both faces draw, because a pane is flat and a person flies around it.

The buffer rides in a data URI, so the model is one text file. `crate.gltf` explains that
choice, and the Flight Helmet next door shows the two-file form.

The `.meta` sidecar is in version control. The identity in it is what `main.scene` resolves
the prefab from, so deleting it would break that reference.
