#!/usr/bin/env python3
"""Fetches a large test scene that is too big to keep in git.

A scene worth measuring a renderer against is larger than GitHub accepts. Intel
Sponza carries 133 MiB of geometry in one file, and GitHub refuses any file over
100 MiB. So the scene lives in a release asset and this script brings it down.

Nothing here runs in CI and nothing here runs during a build. A clean clone
builds and tests with no network, and the sandbox scene is what ships. Run this
only when you want the large scene.

    python3 scripts/fetch-scene.py sponza

The archive extracts into a source content tree that the cooker reads like any
other. This then cooks it, because `runtime --content` takes a cooked tree and
not a source one, and pointing it at the source tree fails in a way that reads
like a missing file.

The download is checked against a SHA-256 recorded here. A file that does not
match is deleted rather than kept, because a half-downloaded archive that stays
on disk looks like a cached one on the next run.
"""

import argparse
import hashlib
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

# Each scene, with the archive that carries it. The digest is what makes a
# cached file trustworthy, so it is pinned here rather than fetched beside the
# archive. A digest that travels with the file it describes proves nothing.
SCENES = {
    "sponza": {
        "url": "https://github.com/instinkt900/camina/releases/download/"
               "scene-sponza-1/sponza-1024.zip",
        "sha256": "0" * 64,
        "into": "scenes/sponza",
        # One file the archive is known to hold, so a tree that is already
        # there can be recognised without unpacking it again.
        "marker": "models/sponza/NewSponza_Main_glTF_003.gltf",
        "about": "Intel Sponza, 3.75M triangles over 115 meshes and 28 materials, "
                 "with its textures at 1024 and its own sky.",
    },
}

CHUNK = 1 << 20


def report(message):
    print(message, file=sys.stderr, flush=True)


def download(url, destination):
    """Downloads to a temporary name and reports progress as it goes."""
    partial = destination.with_suffix(destination.suffix + ".part")
    digest = hashlib.sha256()
    read = 0

    try:
        with urllib.request.urlopen(url) as response:
            total = int(response.headers.get("Content-Length", 0))
            with partial.open("wb") as out:
                while True:
                    block = response.read(CHUNK)
                    if not block:
                        break
                    out.write(block)
                    digest.update(block)
                    read += len(block)
                    if total and read % (16 * CHUNK) < CHUNK:
                        report(f"  {read / 1e6:.0f} of {total / 1e6:.0f} MB")
    except urllib.error.HTTPError as error:
        partial.unlink(missing_ok=True)
        raise SystemExit(f"{url} answered {error.code} {error.reason}.") from error
    except urllib.error.URLError as error:
        partial.unlink(missing_ok=True)
        raise SystemExit(f"{url} could not be reached: {error.reason}") from error

    partial.replace(destination)
    return digest.hexdigest()


def digest_of(path):
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def extract(archive, into):
    """Unpacks the archive, refusing any member that would escape the tree."""
    into.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as zipped:
        for member in zipped.namelist():
            target = (into / member).resolve()
            if not target.is_relative_to(into.resolve()):
                raise SystemExit(f"{archive} holds {member}, which leaves the tree.")
        zipped.extractall(into)
        return len(zipped.namelist())


def cook(cooker, source, cooked):
    """Cooks the fetched tree, so that the runtime has something it can open.

    `runtime --content` takes a cooked tree. Handing it the source tree reports
    a missing manifest, which reads like a failed download rather than like the
    one step nobody ran.
    """
    report(f"Cooking {source} into {cooked}. The textures take a few minutes.")
    result = subprocess.run([str(cooker), "--content", str(source), "--out", str(cooked)])
    if result.returncode != 0:
        report(f"The cooker failed with {result.returncode}.")
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene", nargs="?", choices=sorted(SCENES), default="sponza",
                        help="which scene to fetch")
    parser.add_argument("--into", type=Path,
                        help="where to unpack it, overriding the default tree")
    parser.add_argument("--cache", type=Path, default=Path(".cache/scenes"),
                        help="where the archive is kept between runs")
    parser.add_argument("--force", action="store_true",
                        help="fetch and unpack again, even when the tree is there")
    parser.add_argument("--list", action="store_true", help="say what there is and stop")
    parser.add_argument("--from-file", type=Path,
                        help="unpack this archive instead of downloading one, which is "
                             "how a maintainer checks an archive before publishing it")
    parser.add_argument("--cooker", type=Path,
                        default=Path("build/RelWithDebInfo/bin/cooker"),
                        help="the cooker to run over the fetched tree")
    parser.add_argument("--cooked-into", type=Path,
                        default=Path("build/RelWithDebInfo/bin/content"),
                        help="the cooked content root the runtime reads")
    parser.add_argument("--no-cook", action="store_true",
                        help="unpack only, and say what to run to cook it")
    options = parser.parse_args()

    if options.list:
        for name, scene in sorted(SCENES.items()):
            print(f"{name}\n  {scene['about']}\n  into {scene['into']}")
        return 0

    scene = SCENES[options.scene]

    # An archive on disk skips the download and the digest both. The digest
    # exists to say that a download arrived whole, and a file a maintainer just
    # built has nothing to compare against yet.
    if options.from_file is not None:
        into = options.into or Path(scene["into"])
        shutil.rmtree(into / "models", ignore_errors=True)
        count = extract(options.from_file, into)
        report(f"Unpacked {count} files from {options.from_file} into {into}.")
        report(f"sha256  {digest_of(options.from_file)}")
        return finish(options, scene, into)

    if set(scene["sha256"]) == {"0"}:
        report(f"{options.scene} has no digest recorded yet, so it cannot be fetched.")
        report("A maintainer publishes the archive and records its SHA-256 here first.")
        return 1

    into = options.into or Path(scene["into"])
    marker = into / scene["marker"]
    if marker.is_file() and not options.force:
        report(f"{into} already holds the scene. Pass --force to fetch it again.")
        return 0

    options.cache.mkdir(parents=True, exist_ok=True)
    archive = options.cache / Path(scene["url"]).name

    if archive.is_file() and not options.force:
        report(f"Checking the archive already in {options.cache}.")
        if digest_of(archive) != scene["sha256"]:
            report("  it does not match, so it goes and the download starts again.")
            archive.unlink()

    if not archive.is_file():
        report(f"Fetching {scene['url']}")
        found = download(scene["url"], archive)
        if found != scene["sha256"]:
            archive.unlink(missing_ok=True)
            report("The download does not match the digest recorded for it.")
            report(f"  expected {scene['sha256']}")
            report(f"  found    {found}")
            return 1
        report("  the digest matches.")

    if options.force and into.is_dir():
        shutil.rmtree(into / "models", ignore_errors=True)

    count = extract(archive, into)
    report(f"Unpacked {count} files into {into}.")
    return finish(options, scene, into)


def finish(options, scene, into):
    """Cooks the fetched tree, or says how to, and says how to run it."""
    cooked = options.cooked_into / options.scene
    cooker = options.cooker.expanduser()
    run = f"./build/RelWithDebInfo/bin/runtime --content {cooked} --watch {into}"

    if options.no_cook or not cooker.is_file():
        if not options.no_cook:
            report(f"{cooker} is not there, so the tree is unpacked and not cooked.")
        report("")
        report(f"Cook it with:  {cooker} --content {into} --out {cooked}")
        report(f"Then run:      {run}")
        return 0

    report("")
    if not cook(cooker, into, cooked):
        return 1

    report("")
    report(f"Run it with:  {run}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
