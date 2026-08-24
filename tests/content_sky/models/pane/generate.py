#!/usr/bin/env python3
"""Generates pane.gltf, the blended surface the sky-order check renders.

One quad, BLEND and double sided, standing over the left of the view and
nothing else. There is no opaque geometry in this scene at all, which is the
whole point: issue #435 is a blended surface with open sky behind it, and every
other content tree here either has no sky or no blended surface.

The quad reaches past the top and the bottom of the frame and stops just short
of the middle, so the left third of the picture is pane over sky and the right
third is sky alone.

Writes pane.gltf to the current directory. Run it from this directory:

    python3 generate.py
"""

import base64
import json
import struct

# The camera sits at the origin and looks down -Z with a 60 degree vertical
# field of view. At this depth the frame is about 2.3 units tall, so a quad
# this size covers the height with room to spare.
DEPTH = -2.0
LEFT = -3.0
RIGHT = -0.05
TOP = 2.0
BOTTOM = -2.0

# A strong tint, so the difference between the two halves is a long way above
# anything a driver would argue about. The alpha lets the sky through, which is
# what makes the order matter at all.
COLOUR = (0.90, 0.12, 0.10, 0.55)


def build():
    """Builds the glTF document and its one embedded buffer."""
    corners = [
        (LEFT, BOTTOM, DEPTH),
        (RIGHT, BOTTOM, DEPTH),
        (RIGHT, TOP, DEPTH),
        (LEFT, TOP, DEPTH),
    ]
    # Facing +Z, which is towards the camera.
    normal = (0.0, 0.0, 1.0)

    verts = []
    for corner in corners:
        verts.extend(corner)
        verts.extend(normal)
        verts.extend((0.0, 0.0))
    idxs = [0, 1, 2, 0, 2, 3]

    vertex_bytes = b"".join(struct.pack("<f", v) for v in verts)
    index_bytes = b"".join(struct.pack("<H", i) for i in idxs)
    blob = bytearray(vertex_bytes)
    index_offset = len(blob)
    blob.extend(index_bytes)
    while len(blob) % 4:
        blob.append(0)

    positions = [verts[i * 8 + 0:i * 8 + 3] for i in range(4)]
    accessors = [
        {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 4,
         "type": "VEC3",
         "min": [min(p[a] for p in positions) for a in range(3)],
         "max": [max(p[a] for p in positions) for a in range(3)]},
        {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": 4,
         "type": "VEC3"},
        {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": 4,
         "type": "VEC2"},
        {"bufferView": 1, "componentType": 5123, "count": 6, "type": "SCALAR"},
    ]
    views = [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(vertex_bytes),
         "byteStride": 32},
        {"buffer": 0, "byteOffset": index_offset, "byteLength": len(index_bytes)},
    ]

    uri = "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode()
    return {
        "asset": {"version": "2.0",
                  "generator": "camina tests/content_sky generate.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "pane"}],
        "meshes": [{"name": "pane", "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3,
            "material": 0,
        }]}],
        "materials": [{
            "name": "tinted glass",
            "alphaMode": "BLEND",
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorFactor": list(COLOUR),
                "metallicFactor": 0.0,
                "roughnessFactor": 0.35,
            },
        }],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob), "uri": uri}],
    }


if __name__ == "__main__":
    with open("pane.gltf", "w", encoding="utf-8") as out:
        json.dump(build(), out, indent=1)
        out.write("\n")
    print("pane.gltf written")
