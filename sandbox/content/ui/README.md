# UI

`panel.png` is the one image the sandbox game UI draws, and `main.mothui` is the layout that
draws it. `generate.py` writes the image, the way `models/room/` and `models/spheres/` are
generated. Nothing here is authored art, so no licence has to be checked.

`fonts/` holds the one font, with its licence beside it.

## The layout

`main.mothui` is a moth_ui layout in the JSON format moth_ui reads. It holds an image and two
strings, which are the two paths the M6 spike exists to test. It is static: every keyframe
track carries one frame, so nothing animates and nothing needs a tick. Animation, input and
widgets are all M10.

The runtime loads it from the **cooked** tree rather than from here. That matters:
`moth_ui::Layout::Load` resolves an image path against the directory the layout was read from,
so a layout read from this directory would name a source image that the manifest does not
hold. See `DESIGN.md` section 8.6.

**`textureFilter` is a number and every other enum in the file is a name.** That is moth_ui's
format rather than a mistake here, and writing the name throws an error that names no field.
Issue #220 holds it.

## Why panel.png looks like that

The pattern is asymmetric. A red mark sits in the top left corner and a green bar along the
bottom left, inside a light border.

That is what makes a wrong draw visible. A quad whose texture coordinates are mirrored moves
the corner mark to the other side. One that is turned puts it at the bottom. A source
rectangle read the wrong way up swaps the mark and the bar. A flat colour or a symmetric
checker would hide every one of those. `DESIGN.md` section 3 states the conventions for that
reason, because nothing else finds this class of error.

The border also shows tiling. `moth_ui::ImageScaleType::Tile` repeats the whole image, so a
run of tiles reads as a grid.

## How a layout names an asset

The engine resolves an image path against the cooked manifest, so a layout names this file as
`ui/panel.png`, relative to the game content root. A font is named rather than pathed, and the
game registers the name before a layout loads. `DESIGN.md` section 8.4 records why the engine
resolves the path, and section 8.6 records why the two differ.
