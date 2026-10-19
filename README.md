<!--
Modified by: gengxun + Codex
Date: 2026-10-19
Summary: Describe the standalone generic small-body DSK renderer and its mission-free input contract.
-->
# Small-Body DSK Renderer

This repository renders synthetic images and geometry rasters from a small-body Type-2 DSK, explicit per-frame body attitude, explicit camera pose, and a calibrated camera model. It is a reusable geometry tool; it does not estimate attitude, infer camera pose, or contain mission observation data.

The renderer accepts a target-body-to-J2000 rotation, a target-centered camera position and camera-to-J2000 rotation, and a TSAI or OpenCV radtan5 camera. It writes linear UInt16 image data, optional stretched products, visibility masks, first-hit depth, camera-frame normals, and DSK plate IDs. Point clouds and DEMs can be used upstream to build a validated DSK, but the formal renderer input is the DSK triangle surface so plate identity and exact first-hit geometry remain explicit.

Scientific conventions are fixed in the input contract: time is SPICE ET/TDB seconds past J2000, rotations are active `R_AB` mappings from frame B to frame A, quaternions are `wxyz`, positions are metres from the DSK shape centre, and DSK/SPICE boundary lengths are kilometres. No local-time comparison, implicit frame, origin, unit, or half-pixel shift is inferred.

## Build

Use an ISIS 9.0.0 installation and the repository's validated environment. The build does not modify the ISIS installation.

```bash
source "$(conda info --base)/etc/profile.d/conda.sh"
conda activate asp360_new
test "$CONDA_DEFAULT_ENV" = "asp360_new"
test -d "$ISISROOT"
cmake -S . -B .build -DISISROOT="$ISISROOT" -DCMAKE_BUILD_TYPE=Release
cmake --build .build --parallel 8
ctest --test-dir .build --output-on-failure
```

The build produces `smallbody_dsk_render` and `smallbody_illumination_resolve`. The external ISIS, Bullet, CSPICE, Eigen, nlohmann-json, and libtiff libraries are runtime prerequisites; they are not vendored here.

## Render

Copy `configs/dsk_attitude_render.v2.example.json`, replace every placeholder with an existing absolute path, validate the DSK metadata and all CSV rows, then run:

```bash
PYTHONPATH="$PWD/python" python python/render_batch.py --config /absolute/path/to/render-config.json
```

A run is published atomically only after all frames and manifests pass validation. Use a new output directory for every run. The batch command consumes supplied states; it does not query an unbound mission file or silently download kernels.

## Repository boundary

The repository contains only generic renderer code, synthetic DSK fixtures, camera/illumination contracts, and unit/integration tests. It intentionally excludes spacecraft mission data, 21-column RS tables, task-specific navigation, real images, mission kernels, and mission-specific configuration. A mission adapter should live in a separate repository or a private downstream project and provide validated `attitudes.csv`, `frames.csv`, `illumination.csv`, and DSK inputs through this public contract.
