# UI

`panel.png` is the one image the sandbox game UI draws. `generate.py` writes it, the way
`models/room/` and `models/spheres/` are generated. Nothing here is authored art, so no
licence has to be checked.

## Why it looks like that

The pattern is asymmetric. A red mark sits in the top left corner and a green bar along the
bottom left, inside a light border.

That is what makes a wrong draw visible. A quad whose texture coordinates are mirrored moves
the corner mark to the other side. One that is turned puts it at the bottom. A source
rectangle read the wrong way up swaps the mark and the bar. A flat colour or a symmetric
checker would hide every one of those. `DESIGN.md` section 3 states the conventions for that
reason, because nothing else finds this class of error.

The border also shows tiling. `moth_ui::ImageScaleType::Tile` repeats the whole image, so a
run of tiles reads as a grid.

## How a layout names it

The engine resolves an image path against the cooked manifest, so a moth_ui layout names this
file as `ui/panel.png`, relative to the game content root. `DESIGN.md` section 8.4 records why
the engine resolves the path rather than moth_ui carrying a GUID.
