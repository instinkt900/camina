#!/usr/bin/env python3
"""Generates spheres.gltf and spheres.bin.

A row of seven metal spheres from roughness 0.05 to 0.95. Each sphere is a
separate mesh and material, and all seven share one set of vertex accessors.

Writes spheres.gltf and spheres.bin to the current directory.
"""

import json
import struct
import math

STACKS = 16
SECTORS = 32
RADIUS = 1.0
ROUGHNESSES = [0.05, 0.20, 0.35, 0.50, 0.65, 0.80, 0.95]
SPACING = 2.5  # centre-to-centre distance along X


def build_sphere_geometry():
    """Returns (positions, normals, texcoords, indices) as bytearrays."""

    positions = []
    normals = []
    texcoords = []

    for i in range(STACKS + 1):
        phi = math.pi * i / STACKS
        sin_phi = math.sin(phi)
        cos_phi = math.cos(phi)

        for j in range(SECTORS + 1):
            theta = 2.0 * math.pi * j / SECTORS
            sin_theta = math.sin(theta)
            cos_theta = math.cos(theta)

            nx = sin_phi * cos_theta
            ny = cos_phi
            nz = sin_phi * sin_theta

            positions.append((nx * RADIUS, ny * RADIUS, nz * RADIUS))
            normals.append((nx, ny, nz))
            texcoords.append((j / SECTORS, i / STACKS))

    pos_bytes = bytearray()
    nor_bytes = bytearray()
    tc_bytes = bytearray()
    idx_bytes = bytearray()

    for p in positions:
        pos_bytes.extend(struct.pack("<fff", *p))
    for n in normals:
        nor_bytes.extend(struct.pack("<fff", *n))
    for t in texcoords:
        tc_bytes.extend(struct.pack("<ff", *t))

    for i in range(STACKS):
        for j in range(SECTORS):
            a = i * (SECTORS + 1) + j
            b = a + SECTORS + 1
            idx_bytes.extend(struct.pack("<HHH", a, b, a + 1))
            idx_bytes.extend(struct.pack("<HHH", a + 1, b, b + 1))

    return pos_bytes, nor_bytes, tc_bytes, idx_bytes


def build_gltf(pos_bytes, nor_bytes, tc_bytes, idx_bytes):
    """Returns the glTF dict. Writes the binary data to spheres.bin."""

    buffer_bytes = bytes(pos_bytes + nor_bytes + tc_bytes + idx_bytes)
    with open("spheres.bin", "wb") as f:
        f.write(buffer_bytes)

    vertex_count = (STACKS + 1) * (SECTORS + 1)
    index_count = STACKS * SECTORS * 6

    meshes = []
    materials = []
    nodes = []

    for i, r in enumerate(ROUGHNESSES):
        x = (i - 3) * SPACING
        meshes.append({
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 3,
            }],
        })
        materials.append({
            "name": f"sphere {r:.2f}",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.94, 0.93, 0.92, 1.0],
                "metallicFactor": 1.0,
                "roughnessFactor": r,
            },
        })
        nodes.append({
            "mesh": i,
            "translation": [x, 2.0, -4.0],
        })

    return {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "buffers": [{"uri": "spheres.bin", "byteLength": len(buffer_bytes)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes),
             "byteLength": len(nor_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nor_bytes),
             "byteLength": len(tc_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nor_bytes) + len(tc_bytes),
             "byteLength": len(idx_bytes), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": vertex_count,
             "type": "VEC3"},
            {"bufferView": 1, "componentType": 5126, "count": vertex_count,
             "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": vertex_count,
             "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": index_count,
             "type": "SCALAR"},
        ],
    }


if __name__ == "__main__":
    gltf = build_gltf(*build_sphere_geometry())
    with open("spheres.gltf", "w") as f:
        json.dump(gltf, f, indent=2)
        f.write("\n")
