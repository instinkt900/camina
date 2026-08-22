# The test content tree

One scene of opaque, single sided geometry, which the GPU frame check renders.
`tests/test_gpu_frame.cpp` is the check and issue #190 is why it exists.

The build cooks this tree the way it cooks the other two. It lands next to the
executable at `content/test/`, so `runtime --content <that>` reads it. The editor
reads this directory itself, because it opens source assets rather than a cooked
tree.

## Why the sandbox cannot be this scene

Issue #188 was a pure black frame. Every graphics pipeline declares dynamic cull
mode, so the tonemap pass inherited back face culling from the mesh pass and its
one full screen triangle was culled. Any scene of opaque geometry alone rendered
black.

The sandbox hid it for months. Its glass panes are double sided and blended, so
they draw last and turn culling off before the tonemap runs. **So the sandbox
cannot express the case that finds this class of bug**, and that is the whole
reason this tree exists.

Every material here is opaque and single sided. `generate.py` says so in a
comment and writes no `alphaMode` and no `doubleSided`, because both default to
what this scene needs.

## Why there is no environment

`scene::Environment` is deliberately absent. With one, `render::SkyPass` fills
every pixel no geometry covers with a gradient, and then a frame stays varied
even when the mesh pass draws nothing at all. The check would pass on a picture
that is entirely broken.

With none, the background is the flat clear colour and every varied pixel came
from geometry. That is what makes "the frame is one colour" mean something.

## The geometry

`models/shapes/generate.py` writes `shapes.gltf`: a ground plane and five boxes
at different depths, heights, colours and roughnesses, two of them metal. Run it
from its own directory.

It is generated rather than authored for the same reason the sandbox room and
the spheres are. A model library would bring a licence question, and the tree
has to be the same bytes for everybody.
