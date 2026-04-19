#!/usr/bin/env python3
"""Generate city USD scenes of varying sizes."""

import random
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

BLOCK_SIZE = 30.0
ROAD_WIDTH = 6.0
SIDEWALK_WIDTH = 2.0
CELL_SIZE = BLOCK_SIZE + ROAD_WIDTH + 2 * SIDEWALK_WIDTH  # 40

BUILDINGS_PER_BLOCK_MIN = 2
BUILDINGS_PER_BLOCK_MAX = 6
HEIGHT_MIN, HEIGHT_MAX = 3, 40
WIDTH_MIN, WIDTH_MAX = 4, 14
DEPTH_MIN, DEPTH_MAX = 4, 14

BLOCKS_PER_FILE = 20

BUILDING_COLORS = [
    (0.55, 0.55, 0.55),
    (0.65, 0.63, 0.58),
    (0.72, 0.68, 0.62),
    (0.60, 0.58, 0.52),
    (0.50, 0.52, 0.58),
    (0.45, 0.48, 0.55),
    (0.58, 0.56, 0.50),
    (0.68, 0.66, 0.64),
    (0.42, 0.44, 0.48),
    (0.62, 0.60, 0.56),
    (0.48, 0.46, 0.44),
    (0.55, 0.50, 0.45),
]


def usda_header(default_prim="Root"):
    return f'''#usda 1.0
(
    defaultPrim = "{default_prim}"
    upAxis = "Y"
)
'''


def block_origin(bx, bz):
    x = bx * CELL_SIZE + SIDEWALK_WIDTH + ROAD_WIDTH / 2
    z = bz * CELL_SIZE + SIDEWALK_WIDTH + ROAD_WIDTH / 2
    return x, z


def generate_ground(out_dir, grid):
    total = grid * CELL_SIZE
    lines = [usda_header("Ground")]
    lines.append('def Xform "Ground" {\n')

    hw = total / 2
    cx, cz = total / 2, total / 2
    lines.append(f'''    def Mesh "RoadPlane" {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 3, 2, 1]
        point3f[] points = [
            ({cx - hw}, -0.05, {cz - hw}),
            ({cx + hw}, -0.05, {cz - hw}),
            ({cx + hw}, -0.05, {cz + hw}),
            ({cx - hw}, -0.05, {cz + hw})
        ]
        color3f[] primvars:displayColor = [(0.25, 0.25, 0.27)]
    }}
''')

    sw_idx = 0
    for i in range(grid + 1):
        road_center = i * CELL_SIZE
        for side in [-1, 1]:
            y = 0.02
            z_edge = road_center + side * (ROAD_WIDTH / 2 + SIDEWALK_WIDTH / 2)
            z_half = SIDEWALK_WIDTH / 2
            lines.append(f'''    def Mesh "SidewalkH_{sw_idx}" {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 3, 2, 1]
        point3f[] points = [
            (0, {y}, {z_edge - z_half}),
            ({total}, {y}, {z_edge - z_half}),
            ({total}, {y}, {z_edge + z_half}),
            (0, {y}, {z_edge + z_half})
        ]
        color3f[] primvars:displayColor = [(0.62, 0.60, 0.58)]
    }}
''')
            sw_idx += 1

        for side in [-1, 1]:
            y = 0.02
            x_edge = road_center + side * (ROAD_WIDTH / 2 + SIDEWALK_WIDTH / 2)
            x_half = SIDEWALK_WIDTH / 2
            lines.append(f'''    def Mesh "SidewalkV_{sw_idx}" {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 3, 2, 1]
        point3f[] points = [
            ({x_edge - x_half}, {y}, 0),
            ({x_edge + x_half}, {y}, 0),
            ({x_edge + x_half}, {y}, {total}),
            ({x_edge - x_half}, {y}, {total})
        ]
        color3f[] primvars:displayColor = [(0.62, 0.60, 0.58)]
    }}
''')
            sw_idx += 1

    for bz in range(grid):
        for bx in range(grid):
            ox, oz = block_origin(bx, bz)
            lines.append(f'''    def Mesh "Grass_{bx}_{bz}" {{
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 3, 2, 1]
        point3f[] points = [
            ({ox}, 0.0, {oz}),
            ({ox + BLOCK_SIZE}, 0.0, {oz}),
            ({ox + BLOCK_SIZE}, 0.0, {oz + BLOCK_SIZE}),
            ({ox}, 0.0, {oz + BLOCK_SIZE})
        ]
        color3f[] primvars:displayColor = [(0.32, 0.42, 0.28)]
    }}
''')

    lines.append('}\n')

    with open(os.path.join(out_dir, "ground.usda"), "w") as f:
        f.write("".join(lines))


def generate_blocks(out_dir, grid):
    all_blocks = [(bx, bz) for bz in range(grid) for bx in range(grid)]
    file_index = 0
    block_files = []

    for chunk_start in range(0, len(all_blocks), BLOCKS_PER_FILE):
        chunk = all_blocks[chunk_start:chunk_start + BLOCKS_PER_FILE]
        fname = f"blocks_{file_index:02d}.usda"
        block_files.append(fname)

        lines = [usda_header("Blocks")]
        lines.append('def Xform "Blocks" {\n')

        for bx, bz in chunk:
            ox, oz = block_origin(bx, bz)
            lines.append(f'    def Xform "Block_{bx}_{bz}" {{\n')

            num_buildings = random.randint(BUILDINGS_PER_BLOCK_MIN, BUILDINGS_PER_BLOCK_MAX)
            placed = []
            for bi in range(num_buildings):
                w = random.uniform(WIDTH_MIN, WIDTH_MAX)
                d = random.uniform(DEPTH_MIN, DEPTH_MAX)
                h = random.uniform(HEIGHT_MIN, HEIGHT_MAX)

                for _attempt in range(20):
                    margin = 1.0
                    bx_pos = random.uniform(margin, BLOCK_SIZE - w - margin)
                    bz_pos = random.uniform(margin, BLOCK_SIZE - d - margin)
                    overlap = False
                    for pw, pd, px, pz in placed:
                        if (bx_pos < px + pw + 0.5 and bx_pos + w + 0.5 > px and
                                bz_pos < pz + pd + 0.5 and bz_pos + d + 0.5 > pz):
                            overlap = True
                            break
                    if not overlap:
                        placed.append((w, d, bx_pos, bz_pos))
                        break
                else:
                    bx_pos = random.uniform(0, max(0.1, BLOCK_SIZE - w))
                    bz_pos = random.uniform(0, max(0.1, BLOCK_SIZE - d))
                    placed.append((w, d, bx_pos, bz_pos))

                wx = ox + bx_pos + w / 2
                wz = oz + bz_pos + d / 2
                color = random.choice(BUILDING_COLORS)

                lines.append(f'''        def Xform "Bldg_{bi}" (
            references = @./building.usda@
        ) {{
            double3 xformOp:translate = ({wx}, 0, {wz})
            double3 xformOp:scale = ({w}, {h}, {d})
            uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]

            over Mesh "Body" {{
                color3f[] primvars:displayColor = [({color[0]}, {color[1]}, {color[2]})]
            }}
        }}
''')

            lines.append('    }\n')

        lines.append('}\n')

        with open(os.path.join(out_dir, fname), "w") as f:
            f.write("".join(lines))
        file_index += 1

    return block_files


def generate_props(out_dir, grid):
    total = grid * CELL_SIZE
    lines = [usda_header("Props")]

    lines.append('''class Xform "TreeProto" {
    def Mesh "Trunk" {
        int[] faceVertexCounts = [4, 4, 4, 4]
        int[] faceVertexIndices = [4,0,1,5, 5,1,2,6, 6,2,3,7, 7,3,0,4]
        point3f[] points = [
            (-0.15, 0, -0.15), (0.15, 0, -0.15), (0.15, 0, 0.15), (-0.15, 0, 0.15),
            (-0.15, 1.2, -0.15), (0.15, 1.2, -0.15), (0.15, 1.2, 0.15), (-0.15, 1.2, 0.15)
        ]
        color3f[] primvars:displayColor = [(0.4, 0.28, 0.16)]
    }
    def Mesh "Canopy" {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [3,2,1,0, 4,5,6,7, 0,1,5,4, 7,6,2,3, 4,7,3,0, 1,2,6,5]
        point3f[] points = [
            (-0.6, 1.2, -0.6), (0.6, 1.2, -0.6), (0.6, 1.2, 0.6), (-0.6, 1.2, 0.6),
            (-0.6, 2.4, -0.6), (0.6, 2.4, -0.6), (0.6, 2.4, 0.6), (-0.6, 2.4, 0.6)
        ]
        color3f[] primvars:displayColor = [(0.22, 0.45, 0.2)]
    }
}

class Xform "LampProto" {
    def Mesh "Pole" {
        int[] faceVertexCounts = [4, 4, 4, 4]
        int[] faceVertexIndices = [4,0,1,5, 5,1,2,6, 6,2,3,7, 7,3,0,4]
        point3f[] points = [
            (-0.05, 0, -0.05), (0.05, 0, -0.05), (0.05, 0, 0.05), (-0.05, 0, 0.05),
            (-0.05, 3.5, -0.05), (0.05, 3.5, -0.05), (0.05, 3.5, 0.05), (-0.05, 3.5, 0.05)
        ]
        color3f[] primvars:displayColor = [(0.3, 0.3, 0.32)]
    }
    def Mesh "Light" {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [3,2,1,0, 4,5,6,7, 0,1,5,4, 7,6,2,3, 4,7,3,0, 1,2,6,5]
        point3f[] points = [
            (-0.15, 3.5, -0.15), (0.15, 3.5, -0.15), (0.15, 3.5, 0.15), (-0.15, 3.5, 0.15),
            (-0.15, 3.8, -0.15), (0.15, 3.8, -0.15), (0.15, 3.8, 0.15), (-0.15, 3.8, 0.15)
        ]
        color3f[] primvars:displayColor = [(0.95, 0.9, 0.6)]
    }
}

''')

    lines.append('def Xform "Props" {\n')

    tree_idx = 0
    lamp_idx = 0

    tree_spacing = 8.0
    for i in range(grid + 1):
        road_center = i * CELL_SIZE
        for side in [-1, 1]:
            x = road_center + side * (ROAD_WIDTH / 2 + SIDEWALK_WIDTH / 2)
            t = tree_spacing / 2
            while t < total:
                near_road = any(abs(t - j * CELL_SIZE) < ROAD_WIDTH / 2 + SIDEWALK_WIDTH for j in range(grid + 1))
                if not near_road:
                    lines.append(f'''    def Xform "Tree_{tree_idx}" (
        inherits = </TreeProto>
    ) {{
        double3 xformOp:translate = ({x}, 0, {t})
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
''')
                    tree_idx += 1
                t += tree_spacing

        for side in [-1, 1]:
            z = road_center + side * (ROAD_WIDTH / 2 + SIDEWALK_WIDTH / 2)
            t = tree_spacing / 2
            while t < total:
                near_road = any(abs(t - j * CELL_SIZE) < ROAD_WIDTH / 2 + SIDEWALK_WIDTH for j in range(grid + 1))
                if not near_road:
                    lines.append(f'''    def Xform "Tree_{tree_idx}" (
        inherits = </TreeProto>
    ) {{
        double3 xformOp:translate = ({t}, 0, {z})
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
''')
                    tree_idx += 1
                t += tree_spacing

    lamp_spacing = 12.0
    for i in range(grid + 1):
        road_center = i * CELL_SIZE
        x = road_center + ROAD_WIDTH / 2 + 0.5
        t = lamp_spacing / 2
        while t < total:
            near_cross = any(abs(t - j * CELL_SIZE) < ROAD_WIDTH / 2 + 1 for j in range(grid + 1))
            if not near_cross:
                lines.append(f'''    def Xform "Lamp_{lamp_idx}" (
        inherits = </LampProto>
    ) {{
        double3 xformOp:translate = ({x}, 0, {t})
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
''')
                lamp_idx += 1
            t += lamp_spacing

        z = road_center + ROAD_WIDTH / 2 + 0.5
        t = lamp_spacing / 2
        while t < total:
            near_cross = any(abs(t - j * CELL_SIZE) < ROAD_WIDTH / 2 + 1 for j in range(grid + 1))
            if not near_cross:
                lines.append(f'''    def Xform "Lamp_{lamp_idx}" (
        inherits = </LampProto>
    ) {{
        double3 xformOp:translate = ({t}, 0, {z})
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }}
''')
                lamp_idx += 1
            t += lamp_spacing

    lines.append('}\n')

    with open(os.path.join(out_dir, "props.usda"), "w") as f:
        f.write("".join(lines))
    return tree_idx, lamp_idx


def generate_city(out_dir, block_files):
    lines = [usda_header("City")]
    lines.append('def Xform "City" {\n')
    lines.append('    def Xform "Ground" (\n        references = @./ground.usda@\n    ) {}\n')
    for i, fname in enumerate(block_files):
        lines.append(f'    def Xform "Blocks_{i:02d}" (\n        references = @./{fname}@\n    ) {{}}\n')
    lines.append('    def Xform "Props" (\n        references = @./props.usda@\n    ) {}\n')
    lines.append('}\n')

    with open(os.path.join(out_dir, "city.usda"), "w") as f:
        f.write("".join(lines))


def generate(grid, name):
    out_dir = os.path.join(SCRIPT_DIR, name)
    os.makedirs(out_dir, exist_ok=True)

    # Copy building.usda reference
    building_src = os.path.join(SCRIPT_DIR, "building.usda")
    building_dst = os.path.join(out_dir, "building.usda")
    if not os.path.exists(building_dst) and os.path.exists(building_src):
        import shutil
        shutil.copy2(building_src, building_dst)

    random.seed(42)
    print(f"\n=== Generating {name} ({grid}x{grid} = {grid*grid} blocks) ===")

    generate_ground(out_dir, grid)
    block_files = generate_blocks(out_dir, grid)
    trees, lamps = generate_props(out_dir, grid)
    generate_city(out_dir, block_files)

    num_buildings = sum(random.randint(BUILDINGS_PER_BLOCK_MIN, BUILDINGS_PER_BLOCK_MAX) for _ in range(grid * grid))
    # Reset seed and recount properly
    random.seed(42)
    total_b = 0
    for _ in range(grid * grid):
        n = random.randint(BUILDINGS_PER_BLOCK_MIN, BUILDINGS_PER_BLOCK_MAX)
        total_b += n
        for _ in range(n):
            random.uniform(WIDTH_MIN, WIDTH_MAX)
            random.uniform(DEPTH_MIN, DEPTH_MAX)
            random.uniform(HEIGHT_MIN, HEIGHT_MAX)
            for _a in range(20):
                random.uniform(0, 1)
                random.uniform(0, 1)
                break
            random.choice(BUILDING_COLORS)

    print(f"  {grid*grid} blocks, ~{total_b} buildings, {trees} trees, {lamps} lamps")
    print(f"  Output: {out_dir}/city.usda")


if __name__ == "__main__":
    for grid, name in [(1, "city_1x1"), (5, "city_5x5"), (10, "city_10x10"), (20, "city_20x20")]:
        generate(grid, name)
    print("\nDone!")
