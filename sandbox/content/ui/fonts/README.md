# Fonts

`LiberationSans-Regular.ttf` is the one font the sandbox game UI draws. It is Liberation
2.1.5, and `OFL.txt` beside it is the license.

## Why this one

The license is the reason. It is the SIL Open Font License 1.1, which allows a project to
bundle and ship a font as long as the license travels with it and a derived font does not
reuse the reserved name. `OFL.txt` is that copy, and this project renames nothing.

    Digitized data copyright (c) 2010 Google Corporation with Reserved Font Arimo, Tinos
    and Cousine.
    Copyright (c) 2012 Red Hat, Inc. with Reserved Font Name Liberation.

This matters because a font is easy to add and hard to check later. Intel Sponza was
rejected over its license after somebody measured it, and a font carries the same risk with
none of the size. See `DESIGN.md` section 8.3.

The other reasons are smaller. It carries the metrics of a font most people have seen, so a
wrong advance or a wrong line height is easy to spot. It has real kerning in its GPOS table,
which is what makes the HarfBuzz half of `engine::ui::Font` do visible work. At 410 KiB it
is small enough for git.

## How a layout names it

The cooker copies a file it has no rule for, so this reaches the cooked tree unchanged. The
engine resolves the path against the manifest, the same way it resolves an image. So a
layout names this file as `ui/fonts/LiberationSans-Regular.ttf`, relative to the game
content root. `DESIGN.md` section 8.4 records why the engine resolves the path rather than
moth_ui carrying a GUID.
