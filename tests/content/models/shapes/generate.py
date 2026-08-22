#!/usr/bin/env python3
"""Generates shapes.gltf, the geometry the GPU frame check renders.

A ground plane and five boxes, each with its own colour and roughness. Every
material is opaque and every one is single sided, which is the whole point:
issue #188 was a black frame that only a scene of that kind could show, and the
sandbox cannot be one because its glass panes are blended and double sided.

Nothing here is meant to be pretty. The frame has to be varied enough that a
uniform one is obviously wrong, and it has to be the same bytes on every
machine.

Writes shapes.gltf to the current directory. Run it from this directory:

    python3 generate.py
"""

import base64
import json
import struct

# A ground plane, then five boxes. Each box is (centre, half extent, colour,
# roughness, metallic, name). They sit at different depths and different
# heights so that no two project onto the same pixels.
GROUND = ((-7.0, 0.0, -7.0), (7.0, 0.0, 7.0), (0.55, 0.55, 0.58), 0.9)

BOXES = [
    ((-3.2, 0.9, -2.0), 0.9, (0.75, 0.18, 0.15), 0.55, 0.0, "red"),
    ((-1.0, 0.6, 0.5), 0.6, (0.18, 0.55, 0.25), 0.35, 0.0, "green"),
    ((1.2, 1.3, -1.2), 1.3, (0.20, 0.32, 0.72), 0.70, 0.0, "blue"),
    ((3.6, 0.7, 1.0), 0.7, (0.80, 0.68, 0.22), 0.25, 1.0, "gold"),
    ((0.2, 2.6, -4.0), 0.5, (0.85, 0.85, 0.88), 0.15, 1.0, "silver"),
]

# The six faces of a unit box, each as four corners in counter-clockwise order
# seen from outside, with its outward normal. Winding decides which side the
# rasterizer keeps, and every material here is single sided, so a face wound
# the wrong way would be invisible from outside and the check would be testing
# a hole.
FACES = [
    ([(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)], (0, 0, 1)),
    ([(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)], (0, 0, -1)),
    ([(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)], (1, 0, 0)),
    ([(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)], (-1, 0, 0)),
    ([(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)], (0, 1, 0)),
    ([(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)], (0, -1, 0)),
]


def add_quad(verts, idxs, corners, normal):
    """Appends one quad as two triangles. Returns nothing."""
    base = len(verts) // 8
    for corner in corners:
        verts.extend(corner)
        verts.extend(normal)
        verts.extend((0.0, 0.0))
    for tri in ((0, 1, 2), (0, 2, 3)):
        for vi in tri:
            idxs.append(base + vi)


def build():
    """Builds the glTF document and its one embedded buffer."""
    primitives = []
    materials = []
    accessors = []
    views = []
    blob = bytearray()

    def emit(verts, idxs, name, colour, roughness, metallic):
        """Writes one primitive and the material it uses."""
        vertex_bytes = b"".join(struct.pack("<f", v) for v in verts)
        index_bytes = b"".join(struct.pack("<H", i) for i in idxs)

        vertex_view = len(views)
        views.append({"buffer": 0, "byteOffset": len(blob),
                      "byteLength": len(vertex_bytes), "byteStride": 32})
        blob.extend(vertex_bytes)
        index_view = len(views)
        views.append({"buffer": 0, "byteOffset": len(blob),
                      "byteLength": len(index_bytes)})
        blob.extend(index_bytes)
        # A buffer view has to start on a four byte boundary for the reader
        # that follows it. Two bytes of index data can leave it on two.
        while len(blob) % 4:
            blob.append(0)

        count = len(verts) // 8
        positions = [verts[i * 8 + 0:i * 8 + 3] for i in range(count)]
        first = len(accessors)
        accessors.append({"bufferView": vertex_view, "byteOffset": 0,
                          "componentType": 5126, "count": count, "type": "VEC3",
                          "min": [min(p[a] for p in positions) for a in range(3)],
                          "max": [max(p[a] for p in positions) for a in range(3)]})
        accessors.append({"bufferView": vertex_view, "byteOffset": 12,
                          "componentType": 5126, "count": count, "type": "VEC3"})
        accessors.append({"bufferView": vertex_view, "byteOffset": 24,
                          "componentType": 5126, "count": count, "type": "VEC2"})
        accessors.append({"bufferView": index_view, "componentType": 5123,
                          "count": len(idxs), "type": "SCALAR"})

        material = len(materials)
        materials.append({
            "name": name,
            # No alphaMode and no doubleSided. Both default to the answer this
            # scene needs, OPAQUE and false, and saying so here would only
            # invite somebody to change it.
            "pbrMetallicRoughness": {
                "baseColorFactor": [*colour, 1.0],
                "metallicFactor": metallic,
                "roughnessFactor": roughness,
            },
        })
        primitives.append({
            "attributes": {"POSITION": first, "NORMAL": first + 1,
                           "TEXCOORD_0": first + 2},
            "indices": first + 3,
            "material": material,
        })

    lo, hi, colour, roughness = GROUND
    verts, idxs = [], []
    add_quad(verts, idxs,
             [(lo[0], 0.0, hi[2]), (hi[0], 0.0, hi[2]),
              (hi[0], 0.0, lo[2]), (lo[0], 0.0, lo[2])], (0.0, 1.0, 0.0))
    emit(verts, idxs, "ground", colour, roughness, 0.0)

    for centre, half, colour, roughness, metallic, name in BOXES:
        verts, idxs = [], []
        for corners, normal in FACES:
            placed = [(centre[0] + c[0] * half,
                       centre[1] + c[1] * half,
                       centre[2] + c[2] * half) for c in corners]
            add_quad(verts, idxs, placed, normal)
        emit(verts, idxs, name, colour, roughness, metallic)

    uri = "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode()
    return {
        "asset": {"version": "2.0", "generator": "camina tests/content generate.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "shapes"}],
        "meshes": [{"name": "shapes", "primitives": primitives}],
        "materials": materials,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob), "uri": uri}],
    }


if __name__ == "__main__":
    with open("shapes.gltf", "w", encoding="utf-8") as out:
        json.dump(build(), out, indent=1)
        out.write("\n")
    print("shapes.gltf written")
