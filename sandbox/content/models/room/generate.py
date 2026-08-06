#!/usr/bin/env python3
"""Generates room.gltf, the five-walled interior in the sandbox.

A floor, a ceiling, a back wall, a red left wall, and a green right wall.
Each wall is a quad with its own material. The front is open.

Writes room.gltf to the current directory.
"""

import base64
import json
import struct

WALLS = [
    # corners (4 × x,y,z),      normal,   color (r,g,b),  roughness
    ([( -6,   0, -8), ( -6,   0, 8), ( 6,   0, 8), ( 6,   0, -8)], ( 0,  1, 0), (0.73,0.72,0.70), 0.85, "floor"),
    ([(  6, 7.5, -8), (  6, 7.5, 8), (-6, 7.5, 8), (-6, 7.5, -8)], ( 0, -1, 0), (0.82,0.82,0.80), 0.95, "ceiling"),
    ([(  6,   0, -8), (  6, 7.5,-8), (-6, 7.5,-8), (-6,   0, -8)], ( 0,  0, 1), (0.70,0.70,0.72), 0.9,  "back"),
    ([(-6, 7.5, -8), (-6, 7.5, 8), (-6,   0, 8), (-6,   0, -8)], ( 1,  0, 0), (0.62,0.11,0.11), 0.9,  "left"),
    ([(  6,   0, -8), (  6,   0, 8), ( 6, 7.5, 8), ( 6, 7.5, -8)], (-1,  0, 0), (0.11,0.48,0.17), 0.9,  "right"),
]

def build_room_gltf():
    verts = bytearray()
    idxs = bytearray()

    for wall_idx, (corners, normal, color, roughness, name) in enumerate(WALLS):
        base = wall_idx * 4
        for corner in corners:
            verts.extend(struct.pack("<fff", *corner))   # position
            verts.extend(struct.pack("<fff", *normal))    # normal
            verts.extend(struct.pack("<ff", 0.0, 0.0))    # texcoord
        for tri in ((0, 1, 2), (0, 2, 3)):
            for vi in tri:
                idxs.extend(struct.pack("<H", base + vi))

    buffer_bytes = bytes(verts + idxs)

    return {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [
            {
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 3 + w,
                "material": w,
            }
            for w in range(len(WALLS))
        ]}],
        "materials": [
            {
                "name": name,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [*color, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": roughness,
                },
            }
            for _, _, color, roughness, name in WALLS
        ],
        "buffers": [{
            "uri": "data:application/octet-stream;base64," +
                   base64.b64encode(buffer_bytes).decode(),
            "byteLength": len(buffer_bytes),
        }],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(verts),
             "byteStride": 32, "target": 34962},
            {"buffer": 0, "byteOffset": len(verts), "byteLength": len(idxs),
             "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(WALLS) * 4,
             "type": "VEC3"},
            {"bufferView": 0, "componentType": 5126, "count": len(WALLS) * 4,
             "type": "VEC3", "byteOffset": 12},
            {"bufferView": 0, "componentType": 5126, "count": len(WALLS) * 4,
             "type": "VEC2", "byteOffset": 24},
        ] + [
            {"bufferView": 1, "componentType": 5123, "count": 6,
             "type": "SCALAR", "byteOffset": w * 12}
            for w in range(len(WALLS))
        ],
    }


if __name__ == "__main__":
    gltf = build_room_gltf()
    with open("room.gltf", "w") as f:
        json.dump(gltf, f, indent=2)
        f.write("\n")
