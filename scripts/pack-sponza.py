#!/usr/bin/env python3
"""Builds the published archive of the large test scene.

This is a maintainer tool. It runs once for each published archive, and it
records exactly what was done to the source, which the licence needs. Everybody
else runs scripts/fetch-scene.py instead.

The source is the Intel Sponza glTF package. Three things happen to it.

Only the referenced files ship. The package holds 137 images and the glTF names
72 of them. The rest are the separate roughness and metalness maps that the glTF
does not use, because it names the packed pair instead.

Every image is scaled down. The source is 4096 by 4096 throughout, which is
2.6 GB of PNG and about 2.2 GB of VRAM once cooked to BC7. The reference GPU has
2 GB, so the scene could not be drawn at all.

The archive carries its sidecars. The cooker guesses a colour space from a file
name when it writes a new sidecar, and that guess is wrong for one file in this
scene. See issue #187. Shipping the sidecars settles it, and it makes the cook
give the same identities on every machine.

The sidecars are written by the cooker rather than by this script. A sidecar is
a versioned document, and one with no version key reads as the oldest schema, so
every field added after that would be dropped in silence. Cooking the staged
tree once produces the shape the engine of the day reads. This script then
corrects the colour spaces the glTF disagrees with, and cooks again to check.
"""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import uuid
import zipfile
from pathlib import Path

# The glTF package, and the sky that Intel ships beside it.
GLTF = "NewSponza_Main_glTF_003.gltf"
BUFFER = "NewSponza_Main_glTF_003.bin"
LICENCE = "credits_license.txt"
SKY = "textures/kloppenheim_05_4k.hdr"

# Where the tree lands inside a content tree, so the archive extracts straight
# into one.
PREFIX = "models/sponza"

# Fixed, so two runs of this script give the same GUIDs and the same archive.
# Everybody who fetches the archive then cooks the same identities.
NAMESPACE = uuid.UUID("6f9619ff-8b86-d011-b42d-00c04fc964ff")

# A zip entry carries a timestamp, and a changing one changes the hash of an
# archive whose content did not change.
EPOCH = (1980, 1, 1, 0, 0, 0)


def report(message):
    print(message, file=sys.stderr, flush=True)


def guid_for(relative):
    """A stable GUID for one file, from its path inside the archive."""
    return str(uuid.uuid5(NAMESPACE, relative))


def referenced_images(document):
    """The image URIs the glTF names, which is what has to ship."""
    return [image["uri"] for image in document.get("images", []) if "uri" in image]


def colour_spaces(document):
    """The colour space of each image, from the material slot that uses it.

    The file name is what the cooker guesses from, and it guesses wrong for one
    file here. The glTF says which slot an image is bound to, and that is the
    real answer. Issue #187 covers moving this into the cooker.
    """
    linear = {"metallicRoughnessTexture", "normalTexture", "occlusionTexture"}
    out = {}

    def mark(reference, slot):
        if reference is None:
            return
        source = document["textures"][reference["index"]]["source"]
        space = "Linear" if slot in linear else "sRGB"
        # sRGB wins a disagreement, the way the cooker resolves one. A base
        # colour read as linear is wrong at every value except 0 and 1.
        if space == "sRGB" or source not in out:
            out[source] = space

    for material in document.get("materials", []):
        pbr = material.get("pbrMetallicRoughness", {})
        mark(pbr.get("baseColorTexture"), "baseColorTexture")
        mark(pbr.get("metallicRoughnessTexture"), "metallicRoughnessTexture")
        mark(material.get("normalTexture"), "normalTexture")
        mark(material.get("occlusionTexture"), "occlusionTexture")
        mark(material.get("emissiveTexture"), "emissiveTexture")

    return out


def cook(cooker, tree, out):
    """Runs the cooker over the staged tree, which writes the sidecars."""
    result = subprocess.run(
        [str(cooker), "--content", str(tree), "--out", str(out)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        report(result.stdout[-4000:])
        report(result.stderr[-4000:])
        raise SystemExit(f"the cooker failed with {result.returncode}")


def settle_sidecars(root, spaces, document):
    """Corrects a colour space the file name guessed and the glTF disagrees with.

    The cooker writes a sidecar from the file name, and the glTF says which slot
    each image is bound to. The slot is the real answer. Only the ones that
    disagree are touched, and only the one field, so the rest of the document
    stays exactly as the engine wrote it. Issue #187 moves this into the cooker.
    """
    fixed = []
    for index, image in enumerate(document.get("images", [])):
        uri = image.get("uri")
        if uri is None:
            continue
        wanted = spaces.get(index)
        if wanted is None:
            continue

        path = root / (uri.replace("\\", "/") + ".meta")
        meta = json.loads(path.read_text())
        texture = meta.get("texture")
        if texture is None or texture.get("color_space") == wanted:
            continue

        fixed.append(f"{uri}: {texture.get('color_space')} -> {wanted}")
        texture["color_space"] = wanted
        path.write_text(json.dumps(meta, indent=2) + "\n")

    return fixed


def set_guids(staged):
    """Replaces each generated GUID with one derived from the path.

    The cooker generates a random GUID for a new sidecar. Two runs of this
    script would then publish two archives that name the same asset differently,
    and so would two people cooking the same tree. A GUID from the path is the
    same everywhere and it stays the same across a re-pack.
    """
    count = 0
    for path in sorted(staged.rglob("*.meta")):
        meta = json.loads(path.read_text())
        if "guid" not in meta:
            continue
        # The path inside the archive, which is what every consumer sees.
        relative = path.relative_to(staged).as_posix().removesuffix(".meta")
        meta["guid"] = guid_for(relative)
        path.write_text(json.dumps(meta, indent=2) + "\n")
        count += 1
    return count


def scale_image(source, destination, size):
    """Scales one PNG down, keeping its channels.

    A packed map holds roughness and metalness in separate channels, so nothing
    here may collapse RGB into greyscale. Lanczos is the filter, because a box
    filter on a normal map loses the high frequencies that are the whole point
    of one.
    """
    from PIL import Image

    with Image.open(source) as image:
        if max(image.size) <= size:
            shutil.copy2(source, destination)
            return image.size, False
        was = image.size
        longest = max(was)
        wanted = (max(1, round(was[0] * size / longest)),
                  max(1, round(was[1] * size / longest)))
        scaled = image.resize(wanted, Image.LANCZOS)
        scaled.save(destination, optimize=True)
        return was, True


def scale_sky(source, destination, width):
    """Scales the Radiance sky down, which needs a tool that reads .hdr.

    Pillow does not. ImageMagick does, and it is the one extra thing this script
    asks for. A missing one copies the file whole rather than failing, because a
    23 MB sky is a cost and not an error.
    """
    try:
        subprocess.run(
            ["convert", str(source), "-resize", f"{width}x{width // 2}", str(destination)],
            check=True,
            capture_output=True,
        )
        return True
    except (OSError, subprocess.CalledProcessError) as error:
        report(f"  the sky did not scale, so it ships whole: {error}")
        shutil.copy2(source, destination)
        return False


def stage(source, staged, size, sky_width):
    """Builds the tree that goes into the archive."""
    document = json.loads((source / GLTF).read_text())
    images = referenced_images(document)
    spaces = colour_spaces(document)

    root = staged / PREFIX
    (root / "textures").mkdir(parents=True, exist_ok=True)

    report(f"The glTF holds {len(document.get('meshes', []))} meshes and "
           f"{len(document.get('materials', []))} materials, and it names {len(images)} "
           f"of the {len(list((source / 'textures').iterdir()))} files in textures/.")

    shutil.copy2(source / GLTF, root / GLTF)
    shutil.copy2(source / BUFFER, root / BUFFER)
    shutil.copy2(source / LICENCE, root / LICENCE)

    for at, uri in enumerate(sorted(images), start=1):
        name = uri.replace("\\", "/")
        destination = root / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        was, scaled = scale_image(source / name, destination, size)
        if at % 12 == 0 or at == len(images):
            report(f"  {at} of {len(images)} images")
        if not scaled:
            report(f"  {name} was already {was[0]}x{was[1]}, so it ships as it is")

    scale_sky(source / SKY, root / SKY, sky_width)

    return root, document, spaces


def pack(staged, out):
    """Zips the staged tree so that the same tree gives the same bytes."""
    files = sorted(p for p in staged.rglob("*") if p.is_file())
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in files:
            info = zipfile.ZipInfo(str(path.relative_to(staged)), date_time=EPOCH)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())
    return len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path,
                        help="the Intel Sponza glTF package")
    parser.add_argument("--out", required=True, type=Path, help="the archive to write")
    parser.add_argument("--size", type=int, default=1024, help="the longest edge of a texture")
    parser.add_argument("--sky-width", type=int, default=1024,
                        help="the width of the equirectangular sky")
    parser.add_argument("--keep", type=Path, help="keep the staged tree here as well")
    parser.add_argument("--cooker", type=Path,
                        default=Path("build/RelWithDebInfo/bin/cooker"),
                        help="the cooker that writes the sidecars")
    options = parser.parse_args()

    source = options.source.expanduser().resolve()
    missing = [name for name in (GLTF, BUFFER, LICENCE) if not (source / name).is_file()]
    if missing:
        report(f"{source} is not an Intel Sponza package. It has no {', '.join(missing)}.")
        return 1

    cooker = options.cooker.expanduser().resolve()
    if not cooker.is_file():
        report(f"{cooker} is not there. Build the cooker target, or pass --cooker.")
        return 1

    staged = options.keep or options.out.with_suffix(".staged")
    for path in (staged, staged.with_name(staged.name + ".cooked")):
        if path.exists():
            shutil.rmtree(path)
    staged.mkdir(parents=True)
    cooked = staged.with_name(staged.name + ".cooked")

    root, document, spaces = stage(source, staged, options.size, options.sky_width)

    # The first cook writes a sidecar for every file, which is the shape the
    # engine of the day reads.
    report("Cooking once, so the engine writes the sidecars.")
    cook(cooker, staged, cooked)

    fixed = settle_sidecars(root, spaces, document)
    for line in fixed:
        report(f"  the glTF disagreed with the file name: {line}")
    report(f"{len(fixed)} colour spaces corrected from the material slots.")

    report(f"{set_guids(staged)} GUIDs replaced with one derived from the path.")

    # The second cook proves the corrected tree still cooks, and it rewrites
    # nothing, because every sidecar is now there.
    report("Cooking again, to check the corrected tree.")
    shutil.rmtree(cooked)
    cook(cooker, staged, cooked)
    shutil.rmtree(cooked)

    count = pack(staged, options.out)

    digest = hashlib.sha256(options.out.read_bytes()).hexdigest()
    size = options.out.stat().st_size
    report("")
    report(f"{options.out} holds {count} files and is {size / 1e6:.1f} MB.")
    report(f"sha256  {digest}")
    print(digest)

    if not options.keep:
        shutil.rmtree(staged)
    return 0


if __name__ == "__main__":
    sys.exit(main())
