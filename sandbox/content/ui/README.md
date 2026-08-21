# UI

`panel.png` is the one image the sandbox game UI draws. `generate.py` writes it, the way
`models/room/` and `models/spheres/` are generated. Nothing here is authored art, so no licence
has to be checked.

`fonts/` holds the one font, with its licence beside it.

## The four layouts

M10.7 gave the game three screens, and `scripts/puzzle.lua` moves between them.

| File | What it is | What puts it up |
|---|---|---|
| `main_menu.mothui` | The title screen, with the game held | The first start, and the pause menu's Main menu button |
| `hud.mothui` | The lines the running game writes, and its three buttons | The Play button |
| `pause.mothui` | Resume and Main menu, over the HUD | P, and the Pause button |
| `button.mothui` | One button, referred to by all three | Nothing. It is never shown on its own |

Each is a moth_ui layout in the JSON format moth_ui reads. Every keyframe track carries one
frame, so nothing animates.

**A run starts at the main menu with the game paused**, so an offscreen capture with no
arguments is the title screen over a world that has not moved. `runtime --click 5:640,382`
presses Play, and the game runs from there.

**Which screen is up is the UI's own state.** No component records it. A reload keeps every
layout showing and keeps the game paused, so `on_start` puts the main menu up only when nothing
is up yet.

**A build with no game UI plays without a menu.** `ui.show` answers false when nobody bound a
surface, which is what `with_ui=False` gives and what the editor's play mode gives today.
Pausing with no menu on the screen would be a game nobody could start, because only a button
can resume one.

`button.mothui` is one button. Its root carries `"class": "button"`, which is what makes
moth_ui build a `UIButton` for it, and the three menus refer to it seven times between them.

**A button is a referenced layout and not a group inside a menu.** moth_ui reads a widget class
only for a group entity, and the only group a `.mothui` can name as a child is a reference. The
id belongs to the reference, so the same button file stands up as `throw button` and as
`play button`, and `on_ui_press` in `scripts/puzzle.lua` tells them apart by that name. A press
names the layout as well, because a node id is only unique inside the file that declares it.

**A button carries its own label**, and the menu names it through the reference. A node id is
unique only within the layout that declares it, so both references hold a child called `label`
and a bare name answers with the first of them. A script says `throw button/label` instead, and
each segment is searched inside what the one before it found. `scripts/puzzle.lua` writes both.

The label is authored as `Button` and the game replaces it. A reload builds the nodes again from
the file, so both go back to `Button` and `on_start` writes them again.

Inside the button the label anchors to the whole face, 0,0 to 1,1 with no offsets, so it follows
whatever rectangle the menu gives the reference. It comes after the face, because a group draws
its children in order. A text node consumes no mouse event, so it cannot steal the press.

**A save while the game runs reloads the layout and loses every line the script wrote.** Each
label goes back to `Button` and the two HUD lines go stale, because a reload builds the node
tree again and nothing tells a script it happened. Issue #410 holds it. The text the file itself
carries is right, so an edit to a layout is still visible at once.

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
