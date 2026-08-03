# Flight Helmet

The model the sandbox draws, and the one the asset pipeline is tested against.

## Where it came from

[Khronos glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/FlightHelmet),
`Models/FlightHelmet/glTF`.

**License: CC0 1.0 Universal.** The Khronos `LICENSE.md` puts every file of the
model, text, image, and binary, in the public domain. CC0 asks for no
attribution, so this file records where the model came from as a courtesy and
not as a condition.

## What changed

The geometry is untouched. `FlightHelmet.gltf` and `FlightHelmet.bin` are the
files Khronos publishes, byte for byte.

The 15 textures were 2048 by 2048, which came to 46 MB. They are 256 by 256
here, which comes to 4.3 MB with the geometry. A repository keeps a binary file
for the life of the project, and nothing this model is here to prove depends on
the resolution.

The color maps went to linear light for the resize and back afterwards. The
normal maps and the occlusion, roughness, and metal maps did not, because they
hold numbers rather than color. That is the same rule the cooker follows when it
builds a mip chain, and DESIGN.md section 5 says why it matters.

```
convert in.png -colorspace RGB -resize 256x256 -colorspace sRGB out.png   # color
convert in.png -resize 256x256 out.png                                    # numbers
```

## Why this model

- **Six meshes and six materials.** One glTF file that holds several meshes is
  what the sub-asset identities in M4.4a exist for. A single-mesh model would
  leave that path tested only by the files the tests build.
- **A base color map and a normal map for each material.** That is what proves
  the sRGB and linear split end to end. Without a normal map the linear path
  would have unit tests behind it and nothing else.
- **An obvious front and top.** A mirrored or inverted import has to be visible
  at once, and issue #38 asks for a model with a known front face.
- Every primitive carries POSITION, NORMAL, TANGENT, and TEXCOORD_0, which is
  the layout `assets::MeshVertex` holds. The tangents come from the file, so
  the cooker does not build them here.
- No skinning, no animation, no morph targets, and no Draco compression. The
  importer handles none of those.

## Notes

The file declares `KHR_materials_transmission` for the glass and the lenses.
Nothing reads that extension yet, and the two materials render as opaque until
something does.

The model stands about 72 cm tall, which is the scale Khronos authored. It is
not a mistake, and nothing here rescales it.
