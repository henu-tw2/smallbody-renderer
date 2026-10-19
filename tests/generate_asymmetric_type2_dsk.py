# Modified by: gengxun + Codex
# Date: 2026-10-19
# Summary: Extract this generic renderer component from the mission repository.

#!/usr/bin/env python3
"""Generate a tiny asymmetric Type-2 DSK integration-test fixture."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import spiceypy as spice


def outward_plates(vertices: np.ndarray) -> np.ndarray:
    faces = np.array(
        [[0, 1, 2], [0, 3, 1], [0, 2, 3], [1, 3, 2]],
        dtype=np.int32,
    )
    centroid = vertices.mean(axis=0)
    for index, face in enumerate(faces):
        a, b, c = vertices[face]
        face_center = (a + b + c) / 3.0
        if np.dot(np.cross(b - a, c - a), face_center - centroid) < 0.0:
            faces[index, 1], faces[index, 2] = (
                faces[index, 2],
                faces[index, 1],
            )
    return faces + 1


def write_segment(
    handle: int,
    vertices: np.ndarray,
    plates: np.ndarray,
    surface_id: int,
) -> None:
    fine_scale = 5.0
    coarse_scale = 4
    workspace_size = 10000
    voxel_pointer_size = 10000
    voxel_list_size = 10000
    spatial_index_size = 120027
    spatial_double, spatial_integer = spice.dskmi2(
        vertices,
        plates,
        fine_scale,
        coarse_scale,
        workspace_size,
        voxel_pointer_size,
        voxel_list_size,
        True,
        spatial_index_size,
    )
    coordinate_parameters = np.zeros(10, dtype=np.float64)
    minimum_radius, maximum_radius = spice.dskrb2(
        vertices, plates, 1, coordinate_parameters
    )
    spice.dskw02(
        handle,
        999001,
        surface_id,
        2,
        "J2000",
        1,
        coordinate_parameters,
        -spice.pi(),
        spice.pi(),
        -spice.pi() / 2.0,
        spice.pi() / 2.0,
        minimum_radius,
        maximum_radius,
        -1.0e50,
        1.0e50,
        vertices,
        plates,
        spatial_double,
        spatial_integer,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--segments", type=int, choices=(1, 2), default=1)
    arguments = parser.parse_args()

    output = arguments.output.resolve()
    if output.exists():
        raise FileExistsError(f"refusing to overwrite DSK fixture: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)

    vertices = np.array(
        [
            [0.0, 0.0, 0.004],
            [-0.003, -0.002, -0.001],
            [0.004, -0.001, -0.001],
            [0.001, 0.005, -0.002],
        ],
        dtype=np.float64,
    )
    plates = outward_plates(vertices)
    handle = spice.dskopn(str(output), "ASYMMETRIC_TYPE2_TEST_DSK", 0)
    try:
        write_segment(handle, vertices, plates, 1)
        if arguments.segments == 2:
            write_segment(handle, vertices, plates, 2)
        spice.dskcls(handle, optmiz=True)
    except BaseException:
        spice.dascls(handle)
        raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
