# UI

`panel.png` is the one image the sandbox game UI draws, and `main.mothui` is the layout that
draws it. `generate.py` writes the image, the way `models/room/` and `models/spheres/` are
generated. Nothing here is authored art, so no licence has to be checked.

`fonts/` holds the one font, with its licence beside it.

## The layout

`main.mothui` is a moth_ui layout in the JSON format moth_ui reads. It holds an image, two
strings, and two buttons for the two things the game does. Every keyframe track carries one
frame, so nothing animates.

`button.mothui` is one button. Its root carries `"class": "button"`, which is what makes
moth_ui build a `UIButton` for it, and `main.mothui` refers to it twice.

**A button is a referenced layout and not a group inside a menu.** moth_ui reads a widget class
only for a group entity, and the only group a `.mothui` can name as a child is a reference. The
id belongs to the reference, so the same button file stands up as `throw button` and as
`reset button`, and `on_ui_press` in `scripts/puzzle.lua` tells them apart by that name.

**A label sits in the menu, not in the button.** A node id is unique only within the layout
that declares it, so two references to one button file would each hold a child called the same
thing and `FindChild` would answer with the first. The two `Text` nodes come after the two
references, because a group draws its children in order and a label has to sit over the face it
names. A text node consumes no mouse event, so it cannot steal the press.

The runtime loads these from the **cooked** tree rather than from here, and a script asks for
one by its source path. The cooker rewrites both an image reference and a sub-layout reference
into an identity, so nothing at run time resolves a path against a directory. See `DESIGN.md`
section 8.4.

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
