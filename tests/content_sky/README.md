# The sky test content tree

One blended pane standing over open sky, which the GPU frame check renders.
`tests/test_gpu_frame.cpp` is the check and issue #435 is why it exists.

The build cooks this tree the way it cooks the other three. It lands next to the
executable at `content/sky/`, so `runtime --content <that>` reads it.

## What it is for

`render::SkyPass` used to draw after every mesh draw, blended ones included. A
blended surface reads what is already in the colour attachment and writes no
depth, so the sky then passed its own depth-equal test over that surface. The
sky is opaque, so it did not tint the pane. It painted over it, and the pane was
gone.

Nothing in the repository could show that. The sandbox is a closed room with no
open sky. Intel Sponza has open sky and no blended geometry. `tests/content`
names no environment at all, deliberately, and its own README says why.

So this tree is the smallest scene that has both halves.

## The scene

The camera sits at the origin and looks down -Z. There is no opaque geometry and
no light. Everything in the picture is either the sky or the pane over it.

`models/pane/generate.py` writes `pane.gltf`: one quad, BLEND and double sided,
tinted red. It covers the left of the frame and stops just short of the middle,
so the left third of the picture is pane over sky and the right third is sky
alone.

`environments/generate.py` writes `sky.hdr`: an equirectangular map whose colour
depends on elevation and on nothing else. That is the property the check needs.
A purely vertical gradient puts the same colour at `(x, y)` and at `(-x, y)` for
a camera with no roll, so the two halves of a frame of open sky are the same.
Any difference between them came from what was drawn over one of them.

Both are generated rather than authored, for the reason `tests/content` gives:
a model library brings a licence question, and the tree has to be the same bytes
for everybody.

## The check

Compare the mean colour of the left third against the mean colour of the right
third. With the pane drawn, they differ by a wide margin. With the sky painting
over the pane, they are the same picture twice.
