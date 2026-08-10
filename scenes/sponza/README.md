# Intel Sponza, the large test scene

This tree is a content tree the cooker reads like any other. The scene file and this
note are in git. The model is not, and `scripts/fetch-scene.py` brings it down.

```bash
python3 scripts/fetch-scene.py sponza
./build/RelWithDebInfo/bin/runtime --content build/RelWithDebInfo/bin/content/sponza \
    --watch scenes/sponza
```

The fetch downloads the archive, checks it against a SHA-256, unpacks it here, and cooks
it. `--content` takes the cooked tree and `--watch` takes this source one. Pass
`--no-cook` to unpack only, and the script says what to run.

Nothing fetches this during a build and nothing fetches it in CI. A clean clone builds
and tests with no network.

## It does not draw yet

The scene loads and cooks, and the viewport is black. Issue #188 holds it. The cause is
not this scene: a sandbox scene with no blended geometry goes black the same way, and the
clear colour does not arrive either, so it sits between the mesh pass and the tonemap.

What works today is everything up to the picture. 158 entities, 115 meshes, 28 materials
and 74 textures load, and the mesh pass reports its draws and its culls. The ImGui overlay
draws over the tonemapped image rather than through it, so a windowed run still shows the
whole scene tree and every component on it.

## Why it is not in git

The source geometry is one 133 MiB file, and GitHub refuses any file over 100 MiB. It
cannot be trimmed down either. The triangle budget is flat across the scene, and the
fifteen heaviest meshes are only 28 percent of the total. See issue #130.

## What the archive holds, and what was done to the source

The archive is built by `scripts/pack-sponza.py` from the Intel Sponza glTF package.
Three things were done to it, and the licence asks that changes be stated.

- **Only the referenced files ship.** The package holds 137 images. The glTF names 72 of
  them, and the other 65 are separate roughness and metalness maps that the glTF does not
  use, because it names the packed pair instead.
- **Every texture was scaled down to 1024, from 4096.** At the source size, 72 textures
  cooked to BC7 need about 2.2 GB of video memory. The reference GPU has 2 GB, so the
  scene could not be drawn at all. The filter is Lanczos, and no image had its channels
  changed, because a packed map carries roughness and metalness in separate channels.
- **The sky was scaled down to 1024 by 512**, from 4096 by 2048.

The geometry is untouched. `NewSponza_Main_glTF_003.gltf` and its `.bin` are the source
files byte for byte.

The archive also carries an asset sidecar for each file. One of them corrects a colour
space, because `dirt_decal_01_dirt_decal_01_mask_gltf_alpha_dirt_decal_Opacity.png` is a
base colour texture whose name holds the word `mask`, and the cooker guesses linear from
the name. Issue #187 moves that decision to the material slot, where the answer already is.

## Licence and citation

The scene is **Sponza 2022**, commissioned by Frank Meinl and sponsored by Anton
Kaplanyan, published by Intel. `credits_license.txt` ships inside the archive and carries
the full text.

Cite it in a publication with:

```bibtex
@misc{sponza22,
  Author = {Frank Meinl and Anton Kaplanyan},
  Year = {2022},
  Note = {https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html},
  Title = {Intel Sample Library}
}
```

Credit for the reference photographs goes to Katica Putica and Princino.photo, Dubrovnik,
Croatia.

### The wording in that file is not consistent

`credits_license.txt` opens with:

> For personal use and educational use. Limited commercial use for marketing and print
> purposes.

The rest of the same file is the full text of the Creative Commons Attribution 4.0
International Public License, which grants more than that opening line does.

This repository uses the scene as a test scene, which the narrower reading already allows,
so nothing here depends on which reading wins. Anybody who wants to use it for something
else should read the file and decide for themselves. Intel asks that questions go to
`sponza dot feedback at intel dot com`.

## Publishing a new archive

The archive lives in a release under its own tag, so it never rides along with an engine
release. To publish another one:

```bash
python3 scripts/pack-sponza.py --source <the Intel package> --out sponza-1024.zip
gh release create scene-sponza-<n> sponza-1024.zip --title "..." --notes "..."
```

Then record the new URL and its SHA-256 in `SCENES` in `scripts/fetch-scene.py`. The
packer gives every asset a GUID derived from its path, so a rebuilt archive names the same
assets and nothing that references them breaks.
