#!/usr/bin/env python3
"""Generate minimal sphere.glb and coin.glb (thick disc) files.

Single-mesh, positions + normals + indices, one material each. No textures.
Output goes to assets/models/.
"""

import json
import math
import struct
import sys
from pathlib import Path


def uv_sphere(radius: float, stacks: int, slices: int):
    """Return (positions, normals, indices) for a UV sphere."""
    positions = []
    normals = []
    # Vertices
    for i in range(stacks + 1):
        v = i / stacks
        phi = v * math.pi  # 0 .. pi
        for j in range(slices + 1):
            u = j / slices
            theta = u * 2.0 * math.pi
            nx = math.sin(phi) * math.cos(theta)
            ny = math.cos(phi)
            nz = math.sin(phi) * math.sin(theta)
            positions.extend([nx * radius, ny * radius, nz * radius])
            normals.extend([nx, ny, nz])
    # Indices (CCW)
    indices = []
    row = slices + 1
    for i in range(stacks):
        for j in range(slices):
            a = i * row + j
            b = a + row
            c = a + 1
            d = b + 1
            indices.extend([a, b, c, c, b, d])
    return positions, normals, indices


def cylinder(radius: float, height: float, slices: int):
    """Return (positions, normals, indices) for a capped cylinder (coin)."""
    positions = []
    normals = []
    indices = []
    hy = height * 0.5

    # Side ring: duplicate top and bottom verts for flat-shaded side normals.
    for j in range(slices + 1):
        u = j / slices
        theta = u * 2.0 * math.pi
        nx = math.cos(theta)
        nz = math.sin(theta)
        # Bottom
        positions.extend([nx * radius, -hy, nz * radius])
        normals.extend([nx, 0.0, nz])
        # Top
        positions.extend([nx * radius, hy, nz * radius])
        normals.extend([nx, 0.0, nz])

    # Side tris
    for j in range(slices):
        a = j * 2
        b = a + 1
        c = a + 2
        d = a + 3
        indices.extend([a, c, b, b, c, d])

    # Top cap (fan): vertices with normal = +Y
    top_start = len(positions) // 3
    positions.extend([0.0, hy, 0.0])
    normals.extend([0.0, 1.0, 0.0])
    for j in range(slices + 1):
        u = j / slices
        theta = u * 2.0 * math.pi
        positions.extend([math.cos(theta) * radius, hy, math.sin(theta) * radius])
        normals.extend([0.0, 1.0, 0.0])
    for j in range(slices):
        indices.extend([top_start, top_start + 1 + j, top_start + 2 + j])

    # Bottom cap (fan): vertices with normal = -Y
    bot_start = len(positions) // 3
    positions.extend([0.0, -hy, 0.0])
    normals.extend([0.0, -1.0, 0.0])
    for j in range(slices + 1):
        u = j / slices
        theta = u * 2.0 * math.pi
        positions.extend([math.cos(theta) * radius, -hy, math.sin(theta) * radius])
        normals.extend([0.0, -1.0, 0.0])
    for j in range(slices):
        # Reverse winding for bottom
        indices.extend([bot_start, bot_start + 2 + j, bot_start + 1 + j])

    return positions, normals, indices


def build_glb(positions, normals, indices, albedo, metallic, roughness, out_path: Path):
    """Build a .glb binary with one mesh, one material."""
    # Binary buffer layout: positions, normals, indices
    pos_bytes = struct.pack(f"<{len(positions)}f", *positions)
    nrm_bytes = struct.pack(f"<{len(normals)}f", *normals)
    # Use uint32 indices for safety
    idx_bytes = struct.pack(f"<{len(indices)}I", *indices)

    def pad4(b: bytes) -> bytes:
        pad = (-len(b)) % 4
        return b + b"\x00" * pad

    pos_bytes = pad4(pos_bytes)
    nrm_bytes = pad4(nrm_bytes)
    idx_bytes_padded = pad4(idx_bytes)

    pos_offset = 0
    nrm_offset = pos_offset + len(pos_bytes)
    idx_offset = nrm_offset + len(nrm_bytes)
    bin_size = idx_offset + len(idx_bytes_padded)

    # Compute bounds for POSITION accessor (required by glTF)
    xs = positions[0::3]; ys = positions[1::3]; zs = positions[2::3]
    pos_min = [min(xs), min(ys), min(zs)]
    pos_max = [max(xs), max(ys), max(zs)]

    vert_count = len(positions) // 3
    idx_count = len(indices)

    gltf = {
        "asset": {"version": "2.0", "generator": "make_primitives.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1},
                "indices": 2,
                "material": 0,
                "mode": 4,  # TRIANGLES
            }]
        }],
        "materials": [{
            "name": "mat",
            "pbrMetallicRoughness": {
                "baseColorFactor": list(albedo),
                "metallicFactor": metallic,
                "roughnessFactor": roughness,
            },
        }],
        "buffers": [{"byteLength": bin_size}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_offset, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": nrm_offset, "byteLength": len(nrm_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": idx_offset, "byteLength": len(idx_bytes), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": vert_count, "type": "VEC3",
             "min": pos_min, "max": pos_max},
            {"bufferView": 1, "componentType": 5126, "count": vert_count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5125, "count": idx_count, "type": "SCALAR"},
        ],
    }

    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_bytes = json_bytes + b" " * ((-len(json_bytes)) % 4)  # pad with spaces

    bin_chunk = pos_bytes + nrm_bytes + idx_bytes_padded
    assert len(bin_chunk) == bin_size

    header = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(json_bytes) + 8 + len(bin_chunk))
    json_chunk_hdr = struct.pack("<II", len(json_bytes), 0x4E4F534A)  # 'JSON'
    bin_chunk_hdr = struct.pack("<II", len(bin_chunk), 0x004E4942)    # 'BIN\0'

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(json_chunk_hdr)
        f.write(json_bytes)
        f.write(bin_chunk_hdr)
        f.write(bin_chunk)
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes)")


def main():
    root = Path(__file__).resolve().parent.parent
    out_dir = root / "assets" / "models"

    # Sphere: diameter 1.0 (matches the engine's unit-cube convention)
    pos, nrm, idx = uv_sphere(radius=0.5, stacks=24, slices=32)
    build_glb(pos, nrm, idx,
              albedo=[0.85, 0.15, 0.15, 1.0], metallic=0.2, roughness=0.4,
              out_path=out_dir / "sphere.glb")

    # Coin: diameter 1.0, thickness 0.1 (scaled by entity scale)
    pos, nrm, idx = cylinder(radius=0.5, height=0.1, slices=32)
    build_glb(pos, nrm, idx,
              albedo=[1.0, 0.85, 0.2, 1.0], metallic=0.9, roughness=0.35,
              out_path=out_dir / "coin.glb")


if __name__ == "__main__":
    sys.exit(main())
