# Modified by: gengxun + Codex
# Date: 2026-10-19
# Summary: Extract this generic renderer component from the mission repository.

# Modified by: gengxun + Codex
# Date: 2026-08-11
# Summary: Preserve schema-1 ASP provenance, make schema-2 rendering ASP-free, and keep audit-only ASP provenance outside the native resolved-config contract.
"""Strict preflight and orchestration contracts for DSK attitude rendering."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import subprocess
from typing import Any, Iterable

from .atomic import (
    PublishedDurabilityUnconfirmedError,
    atomic_write_bytes,
    publish_directory_noreplace,
    rename_noreplace,
)


ATTITUDE_COLUMNS = (
    "frame_id",
    "et_tdb_s",
    "q_dsk_to_j2000_w",
    "q_dsk_to_j2000_x",
    "q_dsk_to_j2000_y",
    "q_dsk_to_j2000_z",
)
FRAME_COLUMNS = (
    "frame_id",
    "et_tdb_s",
    "camera_path",
    "width_px",
    "height_px",
    "camera_center_j2000_x_m",
    "camera_center_j2000_y_m",
    "camera_center_j2000_z_m",
    "q_camera_to_j2000_w",
    "q_camera_to_j2000_x",
    "q_camera_to_j2000_y",
    "q_camera_to_j2000_z",
)
FRAME_V2_COLUMNS = (
    "frame_id",
    "et_tdb_s",
    "camera_path",
    "camera_model_format",
    "width_px",
    "height_px",
    "target_to_camera_j2000_x_m",
    "target_to_camera_j2000_y_m",
    "target_to_camera_j2000_z_m",
    "q_camera_to_j2000_w",
    "q_camera_to_j2000_x",
    "q_camera_to_j2000_y",
    "q_camera_to_j2000_z",
)
ILLUMINATION_COLUMNS = (
    "frame_id",
    "et_tdb_s",
    "target_to_sun_j2000_x_m",
    "target_to_sun_j2000_y_m",
    "target_to_sun_j2000_z_m",
)
_CPP_INT_MIN = -2147483648
_CPP_INT_MAX = 2147483647
_MAX_TILE_EDGE_PX = 4096
_MAX_TILE_PIXELS = 1048576
# JSON preserves binary64 round trips; this also stays stricter than the
# renderer's 1e-10 proper-rotation guard while allowing arithmetic roundoff.
_ROTATION_REPORT_ABS_TOLERANCE = 1.0e-12
_SAFE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
_TOOL_NAMES_V1 = (
    "native",
    "std2isis",
    "isis2std",
    "stats",
    "tiffinfo",
    "gdalinfo",
    "asp_version",
    "parallel",
)
_TOOL_NAMES_V2 = tuple(name for name in _TOOL_NAMES_V1 if name != "asp_version")
_V2_ASP_PROVENANCE = {
    "status": "NOT_INVOKED_BY_RENDERER_V2",
    "reason": "Native DSK rendering consumes the audited TSAI camera directly",
}
RAW_PRODUCT_NAMES = (
    "image_u16.tif",
    "mask_u8.tif",
    "depth_m_f32.tif",
    "normal_camera_f32.tif",
    "facet_id_u32.tif",
    "render.json",
)
ILLUMINATION_PRODUCT_NAMES = (
    "mu0_f32.tif",
    "illumination_mask_u8.tif",
)
ALBEDO_PRODUCT_NAMES = ("albedo_f32.tif",)
_FORMAL_PRODUCT_NAMES = frozenset(
    {
        *RAW_PRODUCT_NAMES[:-1],
        "image_u16.cub",
        "image_u8_stretch.tif",
        "frame.json",
    }
)
_RASTER_CONTRACTS = {
    "image_u16.tif": ("UInt16", 1),
    "mask_u8.tif": ("Byte", 1),
    "depth_m_f32.tif": ("Float32", 1),
    "normal_camera_f32.tif": ("Float32", 3),
    "facet_id_u32.tif": ("UInt32", 1),
    "mu0_f32.tif": ("Float32", 1),
    "illumination_mask_u8.tif": ("Byte", 1),
    "albedo_f32.tif": ("Float32", 1),
    "image_u16.cub": ("Float32", 1),
    "image_u8_stretch.tif": ("Byte", 1),
}


def _illumination_mode_from_resolved(
    payload: dict[str, object],
) -> str:
    schema_version = payload.get("schema_version")
    if schema_version == 1 and type(schema_version) is int:
        return "camera_headlight"
    if schema_version != 2 or type(schema_version) is not int:
        raise ValueError(
            "resolved_config schema_version must be integer 1 or 2"
        )
    render = payload.get("render")
    if not isinstance(render, dict):
        raise ValueError("resolved_config render must be an object")
    illumination = render.get("illumination")
    if not isinstance(illumination, dict):
        raise ValueError(
            "resolved_config render illumination must be an object"
        )
    mode = illumination.get("mode")
    if mode not in {"camera_headlight", "point_sun_lambert"}:
        raise ValueError(
            "resolved_config render illumination mode is invalid"
        )
    return str(mode)


def _surface_appearance_mode_from_resolved(payload: dict[str, object]) -> str:
    schema_version = payload.get("schema_version")
    if schema_version == 1 and type(schema_version) is int:
        return "uniform"
    if schema_version != 2 or type(schema_version) is not int:
        raise ValueError(
            "resolved_config schema_version must be integer 1 or 2"
        )
    render = payload.get("render")
    if not isinstance(render, dict):
        raise ValueError("resolved_config render must be an object")
    surface = render.get("surface_appearance")
    if not isinstance(surface, dict):
        raise ValueError(
            "resolved_config render surface_appearance must be an object"
        )
    mode = surface.get("mode")
    if mode not in {"uniform", "plate_albedo"}:
        raise ValueError(
            "resolved_config surface_appearance mode is invalid"
        )
    return str(mode)


def _raw_product_names(
    illumination_mode: str, surface_appearance_mode: str = "uniform"
) -> tuple[str, ...]:
    products = [*RAW_PRODUCT_NAMES[:-1]]
    if illumination_mode == "point_sun_lambert":
        products.extend(ILLUMINATION_PRODUCT_NAMES)
    if surface_appearance_mode == "plate_albedo":
        products.extend(ALBEDO_PRODUCT_NAMES)
    products.append("render.json")
    return tuple(products)


def _formal_product_names(
    illumination_mode: str, surface_appearance_mode: str = "uniform"
) -> frozenset[str]:
    products = set(_FORMAL_PRODUCT_NAMES)
    if illumination_mode == "point_sun_lambert":
        products.update(ILLUMINATION_PRODUCT_NAMES)
    if surface_appearance_mode == "plate_albedo":
        products.update(ALBEDO_PRODUCT_NAMES)
    return frozenset(products)


@dataclass(frozen=True)
class ResolvedRun:
    raw_config: Path
    resolved_config: Path
    run_dir: Path
    frame_ids: tuple[str, ...]
    native_executable: Path
    std2isis_executable: Path
    isis2std_executable: Path
    stats_executable: Path
    tiffinfo_executable: Path
    gdalinfo_executable: Path
    asp_version_executable: Path | None
    parallel_executable: Path
    jobs: int
    input_fingerprints: dict[str, dict[str, object]]


class ExternalCommandError(RuntimeError):
    """An ISIS or validation executable returned a nonzero status."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _exact_columns(reader: csv.DictReader, expected: tuple[str, ...], path: Path) -> None:
    if reader.fieldnames != list(expected):
        raise ValueError(
            f"{path}: columns must be exactly {','.join(expected)}"
        )


def _finite(text: str, field: str) -> float:
    try:
        value = float(text)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{field}: must be a finite number") from error
    if not math.isfinite(value):
        raise ValueError(f"{field}: must be a finite number")
    return value


def _positive_int(text: str, field: str) -> int:
    try:
        value = int(text)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{field}: must be a positive integer") from error
    if str(value) != str(text).strip() or value <= 0:
        raise ValueError(f"{field}: must be a positive integer")
    return value


def _positive_cpp_int_text(text: str, field: str) -> int:
    value = _positive_int(text, field)
    if value > _CPP_INT_MAX:
        raise ValueError(
            f"{field}: must be within the positive C++ int range"
        )
    return value


def _v2_positive_json_int(value: object, field: str) -> int:
    if type(value) is not int or not 1 <= value <= _CPP_INT_MAX:
        raise ValueError(
            f"{field}: must be a positive JSON integer within the C++ int range"
        )
    return value


def _v2_json_cpp_int(value: object, field: str) -> int:
    if type(value) is not int or not _CPP_INT_MIN <= value <= _CPP_INT_MAX:
        raise ValueError(
            f"{field}: must be a JSON integer within the C++ int range"
        )
    return value


def _v2_json_number(value: object, field: str) -> float:
    if type(value) not in {int, float}:
        raise ValueError(f"{field}: must be a finite JSON number")
    try:
        result = float(value)
    except OverflowError as error:
        raise ValueError(f"{field}: must be a finite JSON number") from error
    if not math.isfinite(result):
        raise ValueError(f"{field}: must be a finite JSON number")
    return result


def _validate_tile_workset(
    tile_width: int,
    tile_height: int,
    *,
    field_prefix: str,
) -> None:
    width_field = f"{field_prefix}tile_width_px"
    height_field = f"{field_prefix}tile_height_px"
    pixels_field = f"{field_prefix}tile_pixels"
    if type(tile_width) is not int or tile_width <= 0 or tile_width % 16:
        raise ValueError(f"{width_field}: must be a positive multiple of 16")
    if type(tile_height) is not int or tile_height <= 0 or tile_height % 16:
        raise ValueError(f"{height_field}: must be a positive multiple of 16")
    if tile_width > _MAX_TILE_EDGE_PX:
        raise ValueError(
            f"{width_field}: must be <= {_MAX_TILE_EDGE_PX}"
        )
    if tile_height > _MAX_TILE_EDGE_PX:
        raise ValueError(
            f"{height_field}: must be <= {_MAX_TILE_EDGE_PX}"
        )
    # Division-first comparison keeps the check valid without ever forming
    # an unbounded product.
    if tile_height > _MAX_TILE_PIXELS // tile_width:
        raise ValueError(
            f"{pixels_field}: tile_width_px * tile_height_px must be "
            f"<= {_MAX_TILE_PIXELS}"
        )


def _v2_absolute_path(value: object, field: str) -> Path:
    if not isinstance(value, (str, os.PathLike)):
        raise ValueError(f"{field}: must be an absolute path")
    path = Path(value)
    if not path.is_absolute():
        raise ValueError(f"{field}: must be an absolute path")
    return path


def _frame_id(text: str, field: str) -> str:
    if not isinstance(text, str) or not _SAFE_ID.fullmatch(text) or text in {".", ".."}:
        raise ValueError(f"{field}: unsafe frame_id")
    return text


def _quaternion(values: Iterable[float], field: str) -> list[float]:
    result = list(values)
    norm = math.hypot(math.hypot(result[0], result[1]), math.hypot(result[2], result[3]))
    if not math.isfinite(norm) or abs(norm - 1.0) > 1.0e-8:
        raise ValueError(f"{field}: quaternion norm must be within 1e-8 of unity")
    return [value / norm for value in result]


def _active_rotation_from_wxyz(quaternion: list[float]) -> list[float]:
    w, x, y, z = quaternion
    return [
        1.0 - 2.0 * (y * y + z * z),
        2.0 * (x * y - z * w),
        2.0 * (x * z + y * w),
        2.0 * (x * y + z * w),
        1.0 - 2.0 * (x * x + z * z),
        2.0 * (y * z - x * w),
        2.0 * (x * z - y * w),
        2.0 * (y * z + x * w),
        1.0 - 2.0 * (x * x + y * y),
    ]


def load_attitudes(path: Path) -> dict[str, dict[str, object]]:
    source = _regular_file(path, "attitudes_csv")
    rows: dict[str, dict[str, object]] = {}
    ets: set[float] = set()
    with source.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        _exact_columns(reader, ATTITUDE_COLUMNS, source)
        for line, raw in enumerate(reader, start=2):
            prefix = f"{source}:{line}"
            identifier = _frame_id(raw["frame_id"], f"{prefix}.frame_id")
            if identifier in rows:
                raise ValueError(f"{prefix}: duplicate frame_id {identifier}")
            et = _finite(raw["et_tdb_s"], f"{prefix}.et_tdb_s")
            if et in ets:
                raise ValueError(f"{prefix}: duplicate et_tdb_s {et}")
            quaternion = _quaternion(
                [
                    _finite(raw[column], f"{prefix}.{column}")
                    for column in ATTITUDE_COLUMNS[2:]
                ],
                f"{prefix}.q_dsk_to_j2000_wxyz",
            )
            rows[identifier] = {
                "frame_id": identifier,
                "et_tdb_s": et,
                "q_dsk_to_j2000_wxyz": quaternion,
            }
            ets.add(et)
    if not rows:
        raise ValueError(f"{source}: attitudes CSV must not be empty")
    return rows


def load_frames(path: Path) -> dict[str, dict[str, object]]:
    source = _regular_file(path, "frames_csv")
    rows: dict[str, dict[str, object]] = {}
    ets: set[float] = set()
    with source.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        _exact_columns(reader, FRAME_COLUMNS, source)
        for line, raw in enumerate(reader, start=2):
            prefix = f"{source}:{line}"
            identifier = _frame_id(raw["frame_id"], f"{prefix}.frame_id")
            if identifier in rows:
                raise ValueError(f"{prefix}: duplicate frame_id {identifier}")
            et = _finite(raw["et_tdb_s"], f"{prefix}.et_tdb_s")
            if et in ets:
                raise ValueError(f"{prefix}: duplicate et_tdb_s {et}")
            camera = _regular_file(Path(raw["camera_path"]), f"{prefix}.camera_path")
            quaternion = _quaternion(
                [
                    _finite(raw[column], f"{prefix}.{column}")
                    for column in FRAME_COLUMNS[-4:]
                ],
                f"{prefix}.q_camera_to_j2000_wxyz",
            )
            rows[identifier] = {
                "frame_id": identifier,
                "et_tdb_s": et,
                "camera_path": str(camera),
                "width_px": _positive_int(raw["width_px"], f"{prefix}.width_px"),
                "height_px": _positive_int(raw["height_px"], f"{prefix}.height_px"),
                "camera_center_j2000_m": [
                    _finite(raw[column], f"{prefix}.{column}")
                    for column in FRAME_COLUMNS[5:8]
                ],
                "q_camera_to_j2000_wxyz": quaternion,
            }
            ets.add(et)
    if not rows:
        raise ValueError(f"{source}: frames CSV must not be empty")
    return rows


def load_frames_v2(path: Path) -> dict[str, dict[str, object]]:
    """Load exact schema-v2 frame columns and explicit vector directions."""
    source = _regular_file(path, "frames_csv")
    rows: dict[str, dict[str, object]] = {}
    ets: set[float] = set()
    with source.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        _exact_columns(reader, FRAME_V2_COLUMNS, source)
        for line, raw in enumerate(reader, start=2):
            prefix = f"{source}:{line}"
            identifier = _frame_id(raw["frame_id"], f"{prefix}.frame_id")
            if identifier in rows:
                raise ValueError(f"{prefix}: duplicate frame_id {identifier}")
            et = _finite(raw["et_tdb_s"], f"{prefix}.et_tdb_s")
            if et in ets:
                raise ValueError(f"{prefix}: duplicate et_tdb_s {et}")
            camera_field = f"{prefix}.camera_path"
            camera = _regular_file(
                _v2_absolute_path(raw["camera_path"], camera_field),
                camera_field,
            )
            camera_model_format = raw["camera_model_format"]
            if camera_model_format not in {
                "asp_tsai_null",
                "asp_tsai5",
                "opencv_radtan5_json",
            }:
                raise ValueError(
                    f"{prefix}.camera_model_format: unknown value "
                    f"{camera_model_format!r}"
                )
            quaternion = _quaternion(
                [
                    _finite(raw[column], f"{prefix}.{column}")
                    for column in FRAME_V2_COLUMNS[-4:]
                ],
                f"{prefix}.q_camera_to_j2000_wxyz",
            )
            rows[identifier] = {
                "frame_id": identifier,
                "et_tdb_s": et,
                "camera_path": str(camera),
                "camera_model_format": camera_model_format,
                "width_px": _positive_cpp_int_text(
                    raw["width_px"], f"{prefix}.width_px"
                ),
                "height_px": _positive_cpp_int_text(
                    raw["height_px"], f"{prefix}.height_px"
                ),
                "target_to_camera_j2000_m": [
                    _finite(raw[column], f"{prefix}.{column}")
                    for column in FRAME_V2_COLUMNS[6:9]
                ],
                "q_camera_to_j2000_wxyz": quaternion,
            }
            ets.add(et)
    if not rows:
        raise ValueError(f"{source}: frames CSV must not be empty")
    return rows


def load_illumination_v2(path: Path) -> dict[str, dict[str, object]]:
    """Load target-to-Sun vectors on J2000 axes in metres at ET/TDB seconds.

    The vector origin is the same DSK target/shape-center origin used by the
    frame contract, its direction is target to Sun, and all values must be
    finite and nonzero. No quaternion is present or implied. Invalid columns,
    frame IDs, duplicate frames, duplicate ET values, non-finite or zero
    vectors, and empty tables are rejected.
    """
    source = _regular_file(path, "illumination_csv")
    rows: dict[str, dict[str, object]] = {}
    ets: set[float] = set()
    with source.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.reader(stream, strict=True)
        try:
            header = next(reader)
        except StopIteration as error:
            raise ValueError(
                f"{source}: columns must be exactly "
                f"{','.join(ILLUMINATION_COLUMNS)}"
            ) from error
        if header != list(ILLUMINATION_COLUMNS):
            raise ValueError(
                f"{source}: columns must be exactly "
                f"{','.join(ILLUMINATION_COLUMNS)}"
            )
        for raw_row in reader:
            line = reader.line_num
            prefix = f"{source}:{line}"
            if len(raw_row) != len(ILLUMINATION_COLUMNS):
                raise ValueError(
                    f"{prefix}: expected exactly "
                    f"{len(ILLUMINATION_COLUMNS)} CSV fields"
                )
            for index, value in enumerate(raw_row):
                if not value:
                    raise ValueError(
                        f"{prefix}.{ILLUMINATION_COLUMNS[index]}: "
                        "field must not be empty"
                    )
                if value != value.strip():
                    raise ValueError(
                        f"{prefix}.{ILLUMINATION_COLUMNS[index]}: "
                        "field must not have leading or trailing whitespace"
                    )
            raw = dict(zip(ILLUMINATION_COLUMNS, raw_row, strict=True))
            identifier = _frame_id(raw["frame_id"], f"{prefix}.frame_id")
            if identifier in rows:
                raise ValueError(f"{prefix}: duplicate frame_id {identifier}")
            et = _finite(raw["et_tdb_s"], f"{prefix}.et_tdb_s")
            if et in ets:
                raise ValueError(f"{prefix}: duplicate et_tdb_s {et}")
            target_to_sun = [
                _finite(raw[column], f"{prefix}.{column}")
                for column in ILLUMINATION_COLUMNS[2:]
            ]
            if not max(abs(value) for value in target_to_sun) > 0.0:
                raise ValueError(
                    f"{prefix}.target_to_sun_j2000_m must be finite and nonzero"
                )
            rows[identifier] = {
                "frame_id": identifier,
                "et_tdb_s": et,
                "target_to_sun_j2000_m": target_to_sun,
            }
            ets.add(et)
    if not rows:
        raise ValueError(f"{source}: illumination CSV must not be empty")
    return rows


def inspect_type2_dsk(
    path: Path, frame_kernel_paths: tuple[Path, ...]
) -> dict[str, object]:
    import spiceypy as spice

    dsk = _regular_file(path, "dsk")
    for kernel in frame_kernel_paths:
        spice.furnsh(str(_regular_file(kernel, "frame_kernel_paths")))
    try:
        handle = spice.dasopr(str(dsk))
        try:
            descriptor = spice.dlabfs(handle)
            metadata = spice.dskgd(handle, descriptor)
            vertex_count, plate_count = spice.dskz02(handle, descriptor)
            try:
                spice.dlafns(handle, descriptor)
            except spice.utils.exceptions.NotFoundError:
                pass
            else:
                raise ValueError("DSK must contain exactly one segment")
        finally:
            spice.dascls(handle)
        frame_name = spice.frmnam(metadata.frmcde)
    finally:
        spice.kclear()
    return {
        "center_id": int(metadata.center),
        "surface_id": int(metadata.surfce),
        "frame_id": int(metadata.frmcde),
        "frame_name": str(frame_name),
        "vertex_count": int(vertex_count),
        "plate_count": int(plate_count),
        "segment_count": 1,
        "data_type": int(metadata.dtype),
    }


def _regular_file(path: Path, field: str) -> Path:
    resolved = Path(path).expanduser().resolve()
    if not resolved.is_absolute() or not resolved.is_file():
        raise ValueError(f"{field}: missing regular file: {resolved}")
    return resolved


def _executable(path: Path, field: str) -> Path:
    resolved = _regular_file(path, field)
    if not os.access(resolved, os.X_OK):
        raise ValueError(f"{field}: file is not executable: {resolved}")
    return resolved


def _exact_keys(payload: Any, expected: set[str], field: str) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValueError(f"{field}: must be an object")
    actual = set(payload)
    if actual != expected:
        raise ValueError(
            f"{field}: expected keys {sorted(expected)}, got {sorted(actual)}"
        )
    return payload


def _v2_mode(
    value: object,
    field: str,
    *,
    active: str,
    recognized_unimplemented: set[str],
) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{field}: must be a string")
    if value == active:
        return value
    if value in recognized_unimplemented:
        raise ValueError(
            f"UNIMPLEMENTED_V2_FEATURE: {field} recognized value "
            f"{value!r} is not implemented"
        )
    raise ValueError(f"{field}: unknown value {value!r}")


def _v2_render_controls(payload: Any) -> dict[str, object]:
    render = _exact_keys(
        payload,
        {
            "maximum_range_m",
            "tile_width_px",
            "tile_height_px",
            "exposure_scale",
            "illumination",
            "camera_distortion",
            "surface_appearance",
        },
        "render",
    )
    maximum_range = _v2_json_number(
        render["maximum_range_m"], "render.maximum_range_m"
    )
    if maximum_range <= 0:
        raise ValueError("render.maximum_range_m must be positive")
    tile_width = _v2_positive_json_int(
        render["tile_width_px"], "render.tile_width_px"
    )
    tile_height = _v2_positive_json_int(
        render["tile_height_px"], "render.tile_height_px"
    )
    _validate_tile_workset(
        tile_width, tile_height, field_prefix="render."
    )
    try:
        exposure_scale = _v2_json_number(
            render["exposure_scale"], "render.exposure_scale"
        )
    except ValueError as error:
        raise ValueError(
            "render.exposure_scale: must be a finite positive JSON number"
        ) from error
    if exposure_scale <= 0:
        raise ValueError(
            "render.exposure_scale: must be a finite positive JSON number"
        )

    illumination_raw = render["illumination"]
    if not isinstance(illumination_raw, dict):
        raise ValueError("render.illumination: must be an object")
    mode_value = illumination_raw.get("mode")
    if not isinstance(mode_value, str):
        raise ValueError("render.illumination.mode: must be a string")
    if mode_value not in {"camera_headlight", "point_sun_lambert"}:
        raise ValueError(
            f"render.illumination.mode: unknown value {mode_value!r}"
        )
    illumination_mode = mode_value
    illumination_keys = {"mode", "ambient", "diffuse", "cast_shadows"}
    if illumination_mode == "point_sun_lambert":
        illumination_keys |= {
            "shadow_origin_offset_m",
            "shadow_maximum_range_m",
        }
    illumination = _exact_keys(
        illumination_raw, illumination_keys, "render.illumination"
    )
    ambient = _v2_json_number(
        illumination["ambient"], "render.illumination.ambient"
    )
    diffuse = _v2_json_number(
        illumination["diffuse"], "render.illumination.diffuse"
    )
    if ambient < 0 or diffuse < 0 or abs(ambient + diffuse - 1.0) > 1.0e-12:
        raise ValueError(
            "render.illumination ambient and diffuse must be nonnegative "
            "and sum to one"
        )
    cast_shadows = illumination["cast_shadows"]
    if not isinstance(cast_shadows, bool):
        raise ValueError("render.illumination.cast_shadows: must be a boolean")
    if illumination_mode == "camera_headlight" and cast_shadows:
        raise ValueError(
            "render.illumination.cast_shadows: camera_headlight requires false"
        )
    if illumination_mode == "point_sun_lambert" and ambient <= 0:
        raise ValueError(
            "render.illumination.ambient: point_sun_lambert requires "
            "a positive value"
        )
    shadow_origin_offset = 0.0
    shadow_maximum_range = 0.0
    if illumination_mode == "point_sun_lambert":
        shadow_origin_offset = _v2_json_number(
            illumination["shadow_origin_offset_m"],
            "render.illumination.shadow_origin_offset_m",
        )
        shadow_maximum_range = _v2_json_number(
            illumination["shadow_maximum_range_m"],
            "render.illumination.shadow_maximum_range_m",
        )
        if shadow_origin_offset <= 0:
            raise ValueError(
                "render.illumination.shadow_origin_offset_m "
                "must be positive"
            )
        if shadow_maximum_range <= 0:
            raise ValueError(
                "render.illumination.shadow_maximum_range_m "
                "must be positive"
            )

    distortion = _exact_keys(
        render["camera_distortion"],
        {"inverse_max_iterations", "roundtrip_tolerance_px"},
        "render.camera_distortion",
    )
    inverse_max_iterations = _v2_positive_json_int(
        distortion["inverse_max_iterations"],
        "render.camera_distortion.inverse_max_iterations",
    )
    roundtrip_tolerance = _v2_json_number(
        distortion["roundtrip_tolerance_px"],
        "render.camera_distortion.roundtrip_tolerance_px",
    )
    if roundtrip_tolerance <= 0:
        raise ValueError(
            "render.camera_distortion.roundtrip_tolerance_px "
            "must be positive"
        )

    surface_raw = render["surface_appearance"]
    if not isinstance(surface_raw, dict):
        raise ValueError("render.surface_appearance: must be an object")
    surface_mode_value = surface_raw.get("mode")
    if surface_mode_value not in {"uniform", "plate_albedo"}:
        raise ValueError(
            "render.surface_appearance.mode: unknown value "
            f"{surface_mode_value!r}"
        )
    surface_mode = str(surface_mode_value)
    surface = _exact_keys(
        surface_raw,
        (
            {"mode", "default_albedo"}
            if surface_mode == "uniform"
            else {"mode", "default_albedo", "plate_albedo_csv"}
        ),
        "render.surface_appearance",
    )
    default_albedo = _v2_json_number(
        surface["default_albedo"],
        "render.surface_appearance.default_albedo",
    )
    if default_albedo <= 0:
        raise ValueError(
            "render.surface_appearance.default_albedo must be positive"
        )
    if illumination_mode == "camera_headlight" and exposure_scale != 1.0:
        raise ValueError(
            "UNIMPLEMENTED_V2_FEATURE: render.exposure_scale recognized "
            "non-identity value is not implemented; current shared "
            "contract requires 1.0"
        )
    if illumination_mode == "camera_headlight" and default_albedo != 1.0:
        raise ValueError(
            "UNIMPLEMENTED_V2_FEATURE: "
            "render.surface_appearance.default_albedo recognized "
            "non-identity value is not implemented; current shared "
            "contract requires 1.0"
        )

    return {
        "maximum_range_m": maximum_range,
        "tile_width_px": tile_width,
        "tile_height_px": tile_height,
        "exposure_scale": exposure_scale,
        "illumination": {
            "mode": illumination_mode,
            "ambient": ambient,
            "diffuse": diffuse,
            "cast_shadows": cast_shadows,
            **(
                {
                    "shadow_origin_offset_m": shadow_origin_offset,
                    "shadow_maximum_range_m": shadow_maximum_range,
                }
                if illumination_mode == "point_sun_lambert"
                else {}
            ),
        },
        "camera_distortion": {
            "inverse_max_iterations": inverse_max_iterations,
            "roundtrip_tolerance_px": roundtrip_tolerance,
        },
        "surface_appearance": {
            "mode": surface_mode,
            "default_albedo": default_albedo,
            **(
                {"plate_albedo_csv": surface["plate_albedo_csv"]}
                if surface_mode == "plate_albedo"
                else {}
            ),
        },
    }


def _fingerprint(path: Path) -> dict[str, object]:
    stat = path.stat()
    return {
        "path": str(path),
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "sha256": sha256_file(path),
    }


def _assert_fingerprints_unchanged(
    fingerprints: dict[str, dict[str, object]], context: str
) -> None:
    for role, expected in fingerprints.items():
        path = Path(str(expected["path"]))
        try:
            actual = _fingerprint(path)
        except OSError as error:
            raise RuntimeError(
                f"{role} changed during preflight ({context}): {error}"
            ) from error
        if actual != expected:
            raise RuntimeError(
                f"{role} changed during preflight ({context}): {path}"
            )


def _json_bytes(payload: object) -> bytes:
    return (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode("utf-8")


def std2isis_command(frame_staging: Path, executable: Path) -> list[str]:
    staging = Path(frame_staging)
    return [
        str(executable),
        f"from={staging / 'image_u16.tif'}",
        f"to={staging / 'image_u16.cub'}",
        "mode=grayscale",
        "setnullrange=yes",
        "nullmin=0",
        "nullmax=0",
    ]


def isis2std_command(frame_staging: Path, executable: Path) -> list[str]:
    staging = Path(frame_staging)
    return [
        str(executable),
        f"from={staging / 'image_u16.cub'}",
        f"to={staging / 'image_u8_stretch.tif'}",
        "format=tiff",
        "bittype=8bit",
        "stretch=linear",
        "minpercent=0.1",
        "maxpercent=99.9",
    ]


def cube_stats_command(frame_staging: Path, executable: Path) -> list[str]:
    staging = Path(frame_staging)
    return [
        str(executable),
        f"from={staging / 'image_u16.cub'}",
        f"to={staging / 'cube_stats.pvl'}",
        "format=pvl",
        "append=no",
    ]


def _run_checked(argv: list[str], role: str) -> dict[str, object]:
    result = subprocess.run(
        argv,
        check=False,
        capture_output=True,
        text=True,
    )
    evidence: dict[str, object] = {
        "argv": argv,
        "return_code": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }
    if result.returncode != 0:
        raise ExternalCommandError(
            f"{role} failed with exit code {result.returncode}: "
            f"{result.stderr.strip()}"
        )
    return evidence


def _gdalinfo_command(executable: Path, path: Path, *, histogram: bool) -> list[str]:
    argv = [
        str(executable),
        "--config",
        "GDAL_PAM_ENABLED",
        "NO",
        "-json",
    ]
    if histogram:
        argv.append("-hist")
    argv.append(str(path))
    return argv


def _require_exact_regular_files(directory: Path, expected: set[str], role: str) -> None:
    if not directory.is_dir() or directory.is_symlink():
        raise ValueError(f"{role}: missing regular directory: {directory}")
    entries = list(directory.iterdir())
    names = {entry.name for entry in entries}
    if names != expected:
        raise ValueError(
            f"{role} product set: expected {sorted(expected)}, got {sorted(names)}"
        )
    for entry in entries:
        if entry.is_symlink():
            raise ValueError(f"{role}: symlink is forbidden: {entry}")
        if not entry.is_file():
            raise ValueError(f"{role}: product is not a regular file: {entry}")
        if entry.stat().st_size == 0:
            raise ValueError(f"{role}: zero-byte product: {entry}")


def _parse_gdal_raster(
    evidence: dict[str, object],
    path: Path,
    width: int,
    height: int,
    expected_type: str,
    expected_bands: int,
) -> dict[str, object]:
    try:
        payload = json.loads(str(evidence["stdout"]))
    except (json.JSONDecodeError, TypeError) as error:
        raise ValueError(f"gdalinfo returned invalid JSON for {path}") from error
    if payload.get("size") != [width, height]:
        raise ValueError(f"{path.name}: gdalinfo dimensions do not match render report")
    bands = payload.get("bands")
    if (
        not isinstance(bands, list)
        or len(bands) != expected_bands
        or any(band.get("type") != expected_type for band in bands)
    ):
        raise ValueError(
            f"{path.name}: expected {expected_bands} band(s) of {expected_type}"
        )
    if "coordinateSystem" in payload or "geoTransform" in payload:
        raise ValueError(
            f"{path.name}: raster must not carry a coordinate system or geotransform"
        )
    return payload


def _parse_cube_stats(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    values: dict[str, int] = {}
    for name in ("ValidPixels", "NullPixels"):
        match = re.search(rf"(?m)^\s*{name}\s*=\s*([0-9]+)\s*$", text)
        if match is None:
            raise ValueError(f"cube stats missing integer {name}: {path}")
        values[name] = int(match.group(1))
    return values


def _require_hashes_unchanged(
    directory: Path,
    expected: dict[str, str],
    context: str,
) -> None:
    for name, digest in expected.items():
        path = directory / name
        if path.is_symlink() or not path.is_file() or sha256_file(path) != digest:
            raise RuntimeError(f"{name} changed during {context}")


def _report_json_int(
    value: object,
    field: str,
    *,
    minimum: int,
    maximum: int,
) -> int:
    if type(value) is not int:
        raise ValueError(f"{field}: must be a JSON integer")
    if not minimum <= value <= maximum:
        raise ValueError(
            f"{field}: must be in the inclusive range "
            f"[{minimum}, {maximum}]"
        )
    return value


def _report_nonempty_string(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field}: must be a non-empty string")
    return value


def validate_and_publish_frame(
    resolved: ResolvedRun,
    frame_id: str,
    attempt_frame_dir: Path,
) -> dict[str, object]:
    identifier = _frame_id(frame_id, "frame_id")
    if identifier not in resolved.frame_ids:
        raise ValueError(f"frame_id is not part of resolved run: {identifier}")
    attempt = Path(attempt_frame_dir).resolve()
    staging = attempt / "frame_staging"
    final_parent = resolved.run_dir / "frames"
    final = final_parent / identifier
    if final.exists() or final.is_symlink():
        raise FileExistsError(f"formal frame already exists: {final}")
    if not final_parent.is_dir() or final_parent.is_symlink():
        raise ValueError(f"formal frames directory is missing: {final_parent}")

    try:
        resolved_payload = json.loads(
            resolved.resolved_config.read_text(encoding="utf-8")
        )
    except json.JSONDecodeError as error:
        raise ValueError("resolved_config is invalid JSON") from error
    if not isinstance(resolved_payload, dict):
        raise ValueError("resolved_config root must be an object")
    schema_version = resolved_payload.get("schema_version")
    if type(schema_version) is not int or schema_version not in (1, 2):
        raise ValueError(
            "resolved_config schema_version must be integer 1 or 2"
        )
    illumination_mode = _illumination_mode_from_resolved(resolved_payload)
    surface_appearance_mode = _surface_appearance_mode_from_resolved(
        resolved_payload
    )
    raw_product_names = _raw_product_names(
        illumination_mode, surface_appearance_mode
    )
    formal_product_names = _formal_product_names(
        illumination_mode, surface_appearance_mode
    )
    _require_exact_regular_files(staging, set(raw_product_names), "raw")
    render_path = staging / "render.json"
    raw_hashes = {
        name: sha256_file(staging / name)
        for name in raw_product_names
    }
    try:
        render = json.loads(render_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"render.json is invalid JSON: {error}") from error
    if not isinstance(render, dict):
        raise ValueError("render.json root must be an object")
    _exact_keys(
        render,
        {
            "schema_version",
            "status",
            "coordinate_contract",
            "frame",
            "dsk",
            "summary",
            "photometry",
            "versions",
            "resources",
        }
        | ({"distortion"} if schema_version == 2 else set())
        | ({"surface_appearance"} if schema_version == 2 else set())
        | (
            {"raw_product_sha256"}
            if (
                illumination_mode == "point_sun_lambert"
                or surface_appearance_mode == "plate_albedo"
            )
            else set()
        ),
        "render",
    )
    render_schema_version = render.get("schema_version")
    if (
        type(render_schema_version) is not int
        or render_schema_version != schema_version
    ):
        raise ValueError(
            "render.json schema_version does not match resolved_config"
        )

    resolved_frames = resolved_payload.get("frames")
    if not isinstance(resolved_frames, list):
        raise ValueError("resolved_config frames must be an array")
    matching_frames = [
        row
        for row in resolved_frames
        if isinstance(row, dict) and row.get("frame_id") == identifier
    ]
    if len(matching_frames) != 1:
        raise ValueError(
            f"resolved_config does not contain exactly one frame {identifier}"
        )
    resolved_frame = matching_frames[0]

    common_resolved_frame_keys = {
        "frame_id",
        "et_tdb_s",
        "camera_path",
        "camera_sha256",
        "width_px",
        "height_px",
        "q_dsk_to_j2000_wxyz",
        "q_camera_to_j2000_wxyz",
    }
    common_report_frame_keys = {
        "frame_id",
        "et_tdb_s",
        "width_px",
        "height_px",
        "q_dsk_to_j2000_wxyz",
        "q_camera_to_j2000_wxyz",
        "rotation_j2000_from_dsk",
        "rotation_j2000_from_camera",
    }
    position_field = (
        "camera_center_j2000_m"
        if schema_version == 1
        else "target_to_camera_j2000_m"
    )
    resolved_frame_keys = common_resolved_frame_keys | {position_field}
    if schema_version == 2:
        resolved_frame_keys.add("camera_model_format")
    if illumination_mode == "point_sun_lambert":
        resolved_frame_keys.add("target_to_sun_j2000_m")
    _exact_keys(
        resolved_frame,
        resolved_frame_keys,
        "resolved_config frame",
    )

    if render["status"] != "passed":
        raise ValueError("render.status: must equal 'passed'")
    frame = _exact_keys(
        render["frame"],
        common_report_frame_keys | {position_field},
        "render.frame",
    )
    if frame["frame_id"] != identifier:
        raise ValueError("render.frame.frame_id does not match requested frame")

    expected_coordinate_contract = {
        "origin": "DSK shape center",
        "j2000_axes": "target-centered axes parallel to J2000",
        "dsk_rotation": "R_JD: DSK body-fixed to target-centered J2000",
        "camera_rotation": "R_JC: camera to target-centered J2000",
        "quaternion_order": "wxyz",
        "rotation_convention": "active",
        "time_scale": "ET/TDB",
        "length_unit": "m",
    }
    if schema_version == 2:
        expected_coordinate_contract.update(
            {
                "camera_vector_origin": "DSK shape center",
                "camera_vector_direction": "DSK shape center to camera",
                "camera_vector_axes": "J2000",
            }
        )
    coordinate_contract = _exact_keys(
        render["coordinate_contract"],
        set(expected_coordinate_contract),
        "render.coordinate_contract",
    )
    if coordinate_contract != expected_coordinate_contract:
        raise ValueError(
            "render.coordinate_contract values do not match schema contract"
        )

    dsk_report = _exact_keys(
        render["dsk"],
        {
            "center_id",
            "surface_id",
            "frame_id",
            "frame_name",
            "vertex_count",
            "plate_count",
        },
        "render.dsk",
    )
    for field in ("center_id", "surface_id", "frame_id"):
        _report_json_int(
            dsk_report[field],
            f"render.dsk.{field}",
            minimum=_CPP_INT_MIN,
            maximum=_CPP_INT_MAX,
        )
    _report_json_int(
        dsk_report["vertex_count"],
        "render.dsk.vertex_count",
        minimum=1,
        maximum=_CPP_INT_MAX,
    )
    plate_count = _report_json_int(
        dsk_report["plate_count"],
        "render.dsk.plate_count",
        minimum=1,
        maximum=_CPP_INT_MAX,
    )
    _report_nonempty_string(dsk_report["frame_name"], "render.dsk.frame_name")

    summary = _exact_keys(
        render["summary"],
        {
            "total_pixels",
            "valid_pixels",
            "minimum_valid_dn",
            "maximum_valid_dn",
            "minimum_depth_m",
            "maximum_depth_m",
            "distinct_plate_count",
        },
        "render.summary",
    )
    point_sun_photometry_keys = {
        "model",
        "ambient",
        "diffuse",
        "target_to_sun_j2000_m",
        "target_to_sun_dsk_m",
        "sun_distance_m",
        "aberration_correction",
        "state_source",
        "input_sha256",
        "cast_shadows",
        "shadow_origin_offset_m",
        "shadow_maximum_range_m",
        "direct_pixels",
        "night_pixels",
        "self_shadow_pixels",
    }
    photometry = _exact_keys(
        render["photometry"],
        (
            point_sun_photometry_keys
            if illumination_mode == "point_sun_lambert"
            else {"model", "ambient", "diffuse", "cast_shadows"}
        ),
        "render.photometry",
    )
    expected_model = (
        "point_sun_lambert"
        if illumination_mode == "point_sun_lambert"
        else "Lambert camera-coincident headlight"
    )
    if photometry["model"] != expected_model:
        raise ValueError(
            f"render.photometry.model: must equal {expected_model!r}"
        )
    ambient = _v2_json_number(
        photometry["ambient"], "render.photometry.ambient"
    )
    diffuse = _v2_json_number(
        photometry["diffuse"], "render.photometry.diffuse"
    )
    if (
        ambient < 0.0
        or diffuse < 0.0
        or abs(ambient + diffuse - 1.0) > 1.0e-12
    ):
        raise ValueError(
            "render.photometry ambient and diffuse must be nonnegative "
            "and sum to one"
        )
    if type(photometry["cast_shadows"]) is not bool:
        raise ValueError(
            "render.photometry.cast_shadows: must be a boolean"
        )
    if (
        illumination_mode == "camera_headlight"
        and photometry["cast_shadows"]
    ):
        raise ValueError(
            "render.photometry.cast_shadows: camera headlight requires false"
        )
    resolved_render = resolved_payload.get("render")
    if not isinstance(resolved_render, dict):
        raise ValueError("resolved_config render must be an object")
    if schema_version == 2:
        resolved_surface = resolved_render.get("surface_appearance")
        if not isinstance(resolved_surface, dict):
            raise ValueError(
                "resolved_config render surface_appearance must be an object"
            )
        surface = _exact_keys(
            render["surface_appearance"],
            (
                {"mode", "default_albedo"}
                if surface_appearance_mode == "uniform"
                else {
                    "mode",
                    "default_albedo",
                    "input_sha256",
                    "valid_plate_count",
                    "missing_plate_count",
                    "invalid_plate_count",
                    "minimum_relative_albedo",
                    "maximum_relative_albedo",
                    "median_relative_albedo",
                }
            ),
            "render.surface_appearance",
        )
        expected_surface_keys = (
            {"mode", "default_albedo"}
            if surface_appearance_mode == "uniform"
            else {
                "mode",
                "default_albedo",
                "plate_albedo_path",
                "plate_albedo_sha256",
            }
        )
        _exact_keys(
            resolved_surface,
            expected_surface_keys,
            "resolved_config render surface_appearance",
        )
        if (
            surface["mode"] != surface_appearance_mode
            or _v2_json_number(
                surface["default_albedo"],
                "render.surface_appearance.default_albedo",
            )
            != _v2_json_number(
                resolved_surface["default_albedo"],
                "resolved_config render surface_appearance.default_albedo",
            )
        ):
            raise ValueError(
                "render.surface_appearance does not match resolved_config"
            )
        if surface_appearance_mode == "plate_albedo":
            input_sha256 = surface["input_sha256"]
            if (
                input_sha256 != resolved_surface["plate_albedo_sha256"]
                or not isinstance(input_sha256, str)
                or re.fullmatch(r"[0-9a-f]{64}", input_sha256) is None
            ):
                raise ValueError(
                    "render.surface_appearance.input_sha256 does not match "
                    "resolved_config"
                )
            valid_plate_count = _report_json_int(
                surface["valid_plate_count"],
                "render.surface_appearance.valid_plate_count",
                minimum=0,
                maximum=plate_count,
            )
            missing_plate_count = _report_json_int(
                surface["missing_plate_count"],
                "render.surface_appearance.missing_plate_count",
                minimum=0,
                maximum=plate_count,
            )
            invalid_plate_count = _report_json_int(
                surface["invalid_plate_count"],
                "render.surface_appearance.invalid_plate_count",
                minimum=0,
                maximum=plate_count,
            )
            if valid_plate_count + missing_plate_count != plate_count:
                raise ValueError(
                    "render.surface_appearance valid and missing plate "
                    "counts do not match DSK plate_count"
                )
            if invalid_plate_count > missing_plate_count:
                raise ValueError(
                    "render.surface_appearance.invalid_plate_count must be "
                    "less than or equal to missing_plate_count"
                )
            relative_albedos = {
                field: _v2_json_number(
                    surface[field], f"render.surface_appearance.{field}"
                )
                for field in (
                    "minimum_relative_albedo",
                    "maximum_relative_albedo",
                    "median_relative_albedo",
                )
            }
            for field, relative_albedo in relative_albedos.items():
                if relative_albedo <= 0.0:
                    raise ValueError(
                        f"render.surface_appearance.{field} must be positive"
                    )
            if not (
                relative_albedos["minimum_relative_albedo"]
                <= relative_albedos["median_relative_albedo"]
                <= relative_albedos["maximum_relative_albedo"]
            ):
                raise ValueError(
                    "render.surface_appearance.minimum_relative_albedo <= "
                    "median_relative_albedo <= maximum_relative_albedo is "
                    "required"
                )
    resolved_photometry = (
        resolved_render.get("photometry")
        if schema_version == 1
        else resolved_render.get("illumination")
    )
    if not isinstance(resolved_photometry, dict):
        raise ValueError(
            "resolved_config render photometry contract is invalid"
        )
    if schema_version == 2:
        _exact_keys(
            resolved_photometry,
            (
                {
                    "mode",
                    "ambient",
                    "diffuse",
                    "cast_shadows",
                    "shadow_origin_offset_m",
                    "shadow_maximum_range_m",
                    "state_source",
                    "state_sha256",
                }
                if illumination_mode == "point_sun_lambert"
                else {"mode", "ambient", "diffuse", "cast_shadows"}
            ),
            "resolved_config render illumination",
        )
    expected_ambient = _v2_json_number(
        resolved_photometry.get("ambient"),
        "resolved_config render ambient",
    )
    expected_diffuse = _v2_json_number(
        resolved_photometry.get("diffuse"),
        "resolved_config render diffuse",
    )
    if ambient != expected_ambient or diffuse != expected_diffuse:
        raise ValueError(
            "render.photometry does not match resolved_config"
        )
    if illumination_mode == "point_sun_lambert":
        target_to_sun = photometry["target_to_sun_j2000_m"]
        expected_target_to_sun = resolved_frame["target_to_sun_j2000_m"]
        if (
            not isinstance(target_to_sun, list)
            or len(target_to_sun) != 3
            or target_to_sun != expected_target_to_sun
        ):
            raise ValueError(
                "render.photometry.target_to_sun_j2000_m does not match "
                "resolved_config frame"
            )
        sun_components = [
            _v2_json_number(
                value,
                f"render.photometry.target_to_sun_j2000_m[{index}]",
            )
            for index, value in enumerate(target_to_sun)
        ]
        expected_sun_distance = math.hypot(*sun_components)
        sun_distance = _v2_json_number(
            photometry["sun_distance_m"],
            "render.photometry.sun_distance_m",
        )
        if (
            expected_sun_distance <= 0.0
            or abs(sun_distance - expected_sun_distance)
            > 1.0e-15 * expected_sun_distance
        ):
            raise ValueError(
                "render.photometry.sun_distance_m does not match "
                "target_to_sun_j2000_m"
            )
        if photometry["aberration_correction"] != "NONE":
            raise ValueError(
                "render.photometry.aberration_correction must equal 'NONE'"
            )
        if photometry["state_source"] != "illumination_csv":
            raise ValueError(
                "render.photometry.state_source must equal "
                "'illumination_csv'"
            )
        state_source = resolved_photometry.get("state_source")
        state_sha256 = resolved_photometry.get("state_sha256")
        if state_source != "illumination_csv" or not re.fullmatch(
            r"[0-9a-f]{64}", str(state_sha256)
        ):
            raise ValueError(
                "resolved_config point-sun state provenance is invalid"
            )
        input_sha256 = _exact_keys(
            photometry["input_sha256"],
            {"dsk", "camera", "illumination_csv"},
            "render.photometry.input_sha256",
        )
        resolved_dsk = resolved_payload.get("dsk")
        if not isinstance(resolved_dsk, dict):
            raise ValueError("resolved_config dsk must be an object")
        expected_input_sha256 = {
            "dsk": resolved_dsk.get("sha256"),
            "camera": resolved_frame.get("camera_sha256"),
            "illumination_csv": state_sha256,
        }
        if input_sha256 != expected_input_sha256 or any(
            not isinstance(value, str)
            or re.fullmatch(r"[0-9a-f]{64}", value) is None
            for value in input_sha256.values()
        ):
            raise ValueError(
                "render.photometry.input_sha256 does not match "
                "resolved_config"
            )
        illumination_fingerprint = resolved.input_fingerprints.get(
            "illumination_csv"
        )
        if (
            not isinstance(illumination_fingerprint, dict)
            or illumination_fingerprint.get("sha256") != state_sha256
        ):
            raise ValueError(
                "resolved_config illumination state_sha256 does not match "
                "input_fingerprints"
            )
        target_to_sun_dsk = photometry["target_to_sun_dsk_m"]
        if (
            not isinstance(target_to_sun_dsk, list)
            or len(target_to_sun_dsk) != 3
        ):
            raise ValueError(
                "render.photometry.target_to_sun_dsk_m must have "
                "three components"
            )
        target_to_sun_dsk_values = [
            _v2_json_number(
                value,
                f"render.photometry.target_to_sun_dsk_m[{index}]",
            )
            for index, value in enumerate(target_to_sun_dsk)
        ]
        for field in (
            "cast_shadows",
            "shadow_origin_offset_m",
            "shadow_maximum_range_m",
        ):
            expected = resolved_photometry.get(field)
            if photometry[field] != expected:
                raise ValueError(
                    f"render.photometry.{field} does not match resolved_config"
                )
        for field in (
            "shadow_origin_offset_m",
            "shadow_maximum_range_m",
        ):
            value = _v2_json_number(
                photometry[field], f"render.photometry.{field}"
            )
            if value <= 0.0:
                raise ValueError(
                    f"render.photometry.{field} must be positive"
                )

    versions = _exact_keys(
        render["versions"],
        {"app", "isis", "bullet", "cspice", "libtiff"},
        "render.versions",
    )
    if versions["app"] != "smallbody_dsk_render/1":
        raise ValueError(
            "render.versions.app: must equal 'smallbody_dsk_render/1'"
        )
    for field in ("isis", "bullet", "cspice", "libtiff"):
        _report_nonempty_string(
            versions[field], f"render.versions.{field}"
        )

    resources = _exact_keys(
        render["resources"],
        {"elapsed_seconds", "peak_rss_kib"},
        "render.resources",
    )
    elapsed_seconds = _v2_json_number(
        resources["elapsed_seconds"],
        "render.resources.elapsed_seconds",
    )
    if elapsed_seconds < 0.0:
        raise ValueError(
            "render.resources.elapsed_seconds must be nonnegative"
        )
    _report_json_int(
        resources["peak_rss_kib"],
        "render.resources.peak_rss_kib",
        minimum=0,
        maximum=9223372036854775807,
    )

    camera_path = _v2_absolute_path(
        resolved_frame["camera_path"],
        "resolved_config frame.camera_path",
    )
    if (
        camera_path.is_symlink()
        or not camera_path.is_file()
        or not re.fullmatch(
            r"[0-9a-f]{64}",
            str(resolved_frame["camera_sha256"]),
        )
        or sha256_file(camera_path) != resolved_frame["camera_sha256"]
    ):
        raise ValueError(
            "resolved_config frame camera path or SHA-256 is invalid"
        )
    frozen_fields = (
        "frame_id",
        "et_tdb_s",
        "width_px",
        "height_px",
        position_field,
        "q_dsk_to_j2000_wxyz",
        "q_camera_to_j2000_wxyz",
    )
    for field in frozen_fields:
        if frame.get(field) != resolved_frame.get(field):
            raise ValueError(
                f"render.json frame {field} does not match resolved_config"
            )
    _v2_json_number(frame["et_tdb_s"], "render.frame.et_tdb_s")
    position = frame[position_field]
    if not isinstance(position, list) or len(position) != 3:
        raise ValueError(
            f"render.frame.{position_field} must have three components"
        )
    for index, value in enumerate(position):
        _v2_json_number(
            value,
            f"render.frame.{position_field}[{index}]",
        )
    validated_quaternions: dict[str, list[float]] = {}
    for field in (
        "q_dsk_to_j2000_wxyz",
        "q_camera_to_j2000_wxyz",
    ):
        quaternion = frame[field]
        if not isinstance(quaternion, list) or len(quaternion) != 4:
            raise ValueError(f"render.frame.{field} must have four components")
        validated_quaternions[field] = _quaternion(
            [
                _v2_json_number(
                    value,
                    f"render.frame.{field}[{index}]",
                )
                for index, value in enumerate(quaternion)
            ],
            f"render.frame.{field}",
        )
    rotation_quaternions = {
        "rotation_j2000_from_dsk": "q_dsk_to_j2000_wxyz",
        "rotation_j2000_from_camera": "q_camera_to_j2000_wxyz",
    }
    for field, quaternion_field in rotation_quaternions.items():
        rotation = frame[field]
        if not isinstance(rotation, list) or len(rotation) != 9:
            raise ValueError(f"render.frame.{field} must have nine components")
        actual = [
            _v2_json_number(
                value,
                f"render.frame.{field}[{index}]",
            )
            for index, value in enumerate(rotation)
        ]
        expected = _active_rotation_from_wxyz(
            validated_quaternions[quaternion_field]
        )
        for index, (actual_value, expected_value) in enumerate(
            zip(actual, expected, strict=True)
        ):
            if (
                abs(actual_value - expected_value)
                > _ROTATION_REPORT_ABS_TOLERANCE
            ):
                raise ValueError(
                    f"render.frame.{field}[{index}] does not match "
                    f"the active rotation derived from {quaternion_field} "
                    f"within {_ROTATION_REPORT_ABS_TOLERANCE:.1e}"
                )
    if illumination_mode == "point_sun_lambert":
        rotation_j2000_from_dsk = frame["rotation_j2000_from_dsk"]
        expected_sun_dsk = [
            sum(
                rotation_j2000_from_dsk[3 * row + column]
                * sun_components[row]
                for row in range(3)
            )
            for column in range(3)
        ]
        scale = max(1.0, *map(abs, expected_sun_dsk))
        for index, (actual, expected) in enumerate(
            zip(
                target_to_sun_dsk_values,
                expected_sun_dsk,
                strict=True,
            )
        ):
            if abs(actual - expected) > 1.0e-12 * scale:
                raise ValueError(
                    "render.photometry.target_to_sun_dsk_m"
                    f"[{index}] does not equal transpose(R_JD) * "
                    "target_to_sun_j2000_m"
                )
    width = _v2_positive_json_int(
        frame["width_px"], "render.frame.width_px"
    )
    height = _v2_positive_json_int(
        frame["height_px"], "render.frame.height_px"
    )
    if schema_version == 2:
        distortion = _exact_keys(
            render["distortion"],
            {"model", "mapping", "inverse"},
            "render.distortion",
        )
        camera_format = resolved_frame["camera_model_format"]
        expected_distortion = {
            "asp_tsai_null": ("none", "none", 0),
            "asp_tsai5": ("asp_tsai5", "ideal_to_observed", width * height),
            "opencv_radtan5_json": (
                "opencv_radtan5",
                "ideal_to_observed",
                width * height,
            ),
        }
        if camera_format not in expected_distortion:
            raise ValueError(
                "resolved_config frame camera_model_format is invalid"
            )
        expected_model, expected_mapping, expected_requested = (
            expected_distortion[camera_format]
        )
        if (
            distortion["model"] != expected_model
            or distortion["mapping"] != expected_mapping
        ):
            raise ValueError(
                "render.distortion model or mapping does not match "
                "resolved_config frame camera_model_format"
            )
        inverse = _exact_keys(
            distortion["inverse"],
            {
                "requested_pixel_count",
                "failed_pixel_count",
                "maximum_iterations",
                "maximum_roundtrip_error_px",
            },
            "render.distortion.inverse",
        )
        requested_pixels = _report_json_int(
            inverse["requested_pixel_count"],
            "render.distortion.inverse.requested_pixel_count",
            minimum=0,
            maximum=width * height,
        )
        failed_pixels = _report_json_int(
            inverse["failed_pixel_count"],
            "render.distortion.inverse.failed_pixel_count",
            minimum=0,
            maximum=width * height,
        )
        maximum_iterations = _report_json_int(
            inverse["maximum_iterations"],
            "render.distortion.inverse.maximum_iterations",
            minimum=0,
            maximum=int(resolved_render["camera_distortion"][
                "inverse_max_iterations"
            ]),
        )
        maximum_roundtrip_error = _v2_json_number(
            inverse["maximum_roundtrip_error_px"],
            "render.distortion.inverse.maximum_roundtrip_error_px",
        )
        tolerance = _v2_json_number(
            resolved_render["camera_distortion"]["roundtrip_tolerance_px"],
            "resolved_config render.camera_distortion.roundtrip_tolerance_px",
        )
        if (
            requested_pixels != expected_requested
            or failed_pixels != 0
            or (expected_requested == 0 and maximum_iterations != 0)
            or (expected_requested == 0 and maximum_roundtrip_error != 0.0)
            or maximum_roundtrip_error < 0.0
            or maximum_roundtrip_error > tolerance
        ):
            raise ValueError(
                "render.distortion.inverse does not match resolved_config "
                "camera model or inverse controls"
            )
    total_pixels = width * height
    reported_total_pixels = _report_json_int(
        summary["total_pixels"],
        "render.summary.total_pixels",
        minimum=1,
        maximum=_CPP_INT_MAX * _CPP_INT_MAX,
    )
    if reported_total_pixels != total_pixels:
        raise ValueError("render summary total_pixels does not match dimensions")
    valid_pixels = _report_json_int(
        summary["valid_pixels"],
        "render.summary.valid_pixels",
        minimum=1,
        maximum=total_pixels,
    )
    if illumination_mode == "point_sun_lambert":
        illumination_counts = {
            field: _report_json_int(
                photometry[field],
                f"render.photometry.{field}",
                minimum=0,
                maximum=valid_pixels,
            )
            for field in (
                "direct_pixels",
                "night_pixels",
                "self_shadow_pixels",
            )
        }
        if sum(illumination_counts.values()) != valid_pixels:
            raise ValueError(
                "render.photometry illumination pixel counts must sum "
                "to render.summary.valid_pixels"
            )
    minimum_valid_dn = _report_json_int(
        summary["minimum_valid_dn"],
        "render.summary.minimum_valid_dn",
        minimum=1,
        maximum=65535,
    )
    maximum_valid_dn = _report_json_int(
        summary["maximum_valid_dn"],
        "render.summary.maximum_valid_dn",
        minimum=1,
        maximum=65535,
    )
    if minimum_valid_dn > maximum_valid_dn:
        raise ValueError(
            "render.summary valid DN range is reversed"
        )
    minimum_depth_m = _v2_json_number(
        summary["minimum_depth_m"],
        "render.summary.minimum_depth_m",
    )
    maximum_depth_m = _v2_json_number(
        summary["maximum_depth_m"],
        "render.summary.maximum_depth_m",
    )
    maximum_range_m = _v2_json_number(
        resolved_render.get("maximum_range_m"),
        "resolved_config render.maximum_range_m",
    )
    if (
        minimum_depth_m <= 0.0
        or maximum_depth_m < minimum_depth_m
        or maximum_depth_m > maximum_range_m
    ):
        raise ValueError(
            "render.summary depth range is invalid"
        )
    _report_json_int(
        summary["distinct_plate_count"],
        "render.summary.distinct_plate_count",
        minimum=1,
        maximum=min(valid_pixels, plate_count),
    )

    commands: dict[str, object] = {"tiffinfo": [], "gdalinfo": []}
    if (
        illumination_mode == "point_sun_lambert"
        or surface_appearance_mode == "plate_albedo"
    ):
        reported_raw_hashes = _exact_keys(
            render["raw_product_sha256"],
            set(raw_product_names[:-1]),
            "render.raw_product_sha256",
        )
        actual_tiff_hashes = {
            name: raw_hashes[name] for name in raw_product_names[:-1]
        }
        if reported_raw_hashes != actual_tiff_hashes or any(
            not isinstance(value, str)
            or re.fullmatch(r"[0-9a-f]{64}", value) is None
            for value in reported_raw_hashes.values()
        ):
            raise ValueError(
                "render.raw_product_sha256 does not match staged TIFFs"
            )
    raster_payloads: dict[str, dict[str, object]] = {}
    for name in raw_product_names[:-1]:
        path = staging / name
        tiff_evidence = _run_checked(
            [str(resolved.tiffinfo_executable), str(path)],
            f"tiffinfo {name}",
        )
        commands["tiffinfo"].append(tiff_evidence)  # type: ignore[union-attr]
        expected_type, expected_bands = _RASTER_CONTRACTS[name]
        gdal_evidence = _run_checked(
            _gdalinfo_command(
                resolved.gdalinfo_executable,
                path,
                histogram=name
                in {"mask_u8.tif", "illumination_mask_u8.tif"},
            ),
            f"gdalinfo {name}",
        )
        commands["gdalinfo"].append(gdal_evidence)  # type: ignore[union-attr]
        raster_payloads[name] = _parse_gdal_raster(
            gdal_evidence,
            path,
            width,
            height,
            expected_type,
            expected_bands,
        )
    mask_histogram = raster_payloads["mask_u8.tif"]["bands"][0].get("histogram")
    if (
        not isinstance(mask_histogram, dict)
        or mask_histogram.get("count") != 256
        or not isinstance(mask_histogram.get("buckets"), list)
        or len(mask_histogram["buckets"]) != 256
        or mask_histogram["buckets"][0] != total_pixels - valid_pixels
        or mask_histogram["buckets"][1] != valid_pixels
        or any(mask_histogram["buckets"][2:])
    ):
        raise ValueError("mask_u8.tif histogram is inconsistent with render summary")
    if illumination_mode == "point_sun_lambert":
        illumination_histogram = raster_payloads[
            "illumination_mask_u8.tif"
        ]["bands"][0].get("histogram")
        expected_buckets = [
            total_pixels - valid_pixels,
            illumination_counts["direct_pixels"],
            illumination_counts["night_pixels"],
            illumination_counts["self_shadow_pixels"],
            *([0] * 252),
        ]
        if (
            not isinstance(illumination_histogram, dict)
            or illumination_histogram.get("count") != 256
            or illumination_histogram.get("buckets") != expected_buckets
        ):
            raise ValueError(
                "illumination_mask_u8.tif histogram is inconsistent "
                "with render photometry counts"
            )

    for role, argv in (
        ("std2isis", std2isis_command(staging, resolved.std2isis_executable)),
        ("isis2std", isis2std_command(staging, resolved.isis2std_executable)),
        ("stats", cube_stats_command(staging, resolved.stats_executable)),
    ):
        commands[role] = _run_checked(argv, role)

    cube = staging / "image_u16.cub"
    stretch = staging / "image_u8_stretch.tif"
    stats_path = staging / "cube_stats.pvl"
    for path in (cube, stretch, stats_path):
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"conversion produced invalid or zero-byte output: {path}")
    stretch_tiff = _run_checked(
        [str(resolved.tiffinfo_executable), str(stretch)],
        "tiffinfo image_u8_stretch.tif",
    )
    commands["tiffinfo"].append(stretch_tiff)  # type: ignore[union-attr]
    for path in (cube, stretch):
        expected_type, expected_bands = _RASTER_CONTRACTS[path.name]
        evidence = _run_checked(
            _gdalinfo_command(resolved.gdalinfo_executable, path, histogram=False),
            f"gdalinfo {path.name}",
        )
        commands["gdalinfo"].append(evidence)  # type: ignore[union-attr]
        _parse_gdal_raster(
            evidence, path, width, height, expected_type, expected_bands
        )

    cube_stats = _parse_cube_stats(stats_path)
    expected_null = total_pixels - valid_pixels
    if cube_stats["ValidPixels"] != valid_pixels:
        raise ValueError(
            f"cube stats ValidPixels: expected {valid_pixels}, "
            f"got {cube_stats['ValidPixels']}"
        )
    if cube_stats["NullPixels"] != expected_null:
        raise ValueError(
            f"cube stats NullPixels: expected {expected_null}, "
            f"got {cube_stats['NullPixels']}"
        )
    _require_hashes_unchanged(staging, raw_hashes, "conversion")

    fingerprint_payload = json.loads(
        (resolved.run_dir / "input_fingerprints.json").read_text(encoding="utf-8")
    )
    dependency_versions = {
        **versions,
        "asp": fingerprint_payload.get(
            "asp", fingerprint_payload.get("asp_version")
        ),
    }
    stats_path.unlink()
    render_path.unlink()
    output_hashes = {
        name: sha256_file(staging / name)
        for name in sorted(formal_product_names - {"frame.json"})
    }
    manifest = {
        "schema_version": 1,
        "status": "passed",
        "frame": frame,
        "coordinate_contract": coordinate_contract,
        "dimensions": {"width_px": width, "height_px": height},
        "summary": summary,
        "cube_stats": cube_stats,
        "dependency_versions": dependency_versions,
        "input_fingerprints": resolved.input_fingerprints,
        "input_sha256": raw_hashes,
        "output_sha256": output_hashes,
        "commands": commands,
        "raw_render_report": {
            "sha256": raw_hashes["render.json"],
            "content": render,
        },
    }
    atomic_write_bytes(staging / "frame.json", _json_bytes(manifest))
    _require_exact_regular_files(staging, set(formal_product_names), "formal")
    _require_hashes_unchanged(
        staging,
        {name: digest for name, digest in raw_hashes.items() if name != "render.json"},
        "publication preparation",
    )

    hidden = final_parent / f".{identifier}.staging-{attempt.parent.name}"
    rename_noreplace(staging, hidden)
    try:
        publish_directory_noreplace(hidden, final)
    except FileExistsError:
        rename_noreplace(hidden, staging)
        raise
    return {
        "frame_id": identifier,
        "frame_dir": str(final),
        "frame_manifest_sha256": sha256_file(final / "frame.json"),
        "products": output_hashes,
    }


def preflight_render_run(config_path: Path) -> ResolvedRun:
    config_source = _regular_file(config_path, "config_path")
    try:
        raw = json.loads(config_source.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"config_path: invalid JSON: {error}") from error
    config_keys = {
        "schema_version",
        "inputs",
        "executables",
        "output_run_dir",
        "render",
        "isis_stretch",
        "jobs",
    }
    if set(raw) == config_keys | {"_change_record"}:
        change_record = _exact_keys(
            raw["_change_record"],
            {"modified_by", "date", "summary"},
            "_change_record",
        )
        if not all(
            isinstance(change_record[name], str) and change_record[name]
            for name in ("modified_by", "date", "summary")
        ):
            raise ValueError("_change_record values must be nonempty strings")
    else:
        _exact_keys(raw, config_keys, "config")
    raw_schema_version = raw["schema_version"]
    if isinstance(raw_schema_version, bool):
        raise ValueError("schema_version: unsupported value")
    if raw_schema_version == 1:
        schema_version = 1
    elif type(raw_schema_version) is int and raw_schema_version == 2:
        schema_version = 2
    else:
        raise ValueError("schema_version: unsupported value")
    if schema_version == 1:
        render = _exact_keys(
            raw["render"],
            {
                "maximum_range_m",
                "tile_width_px",
                "tile_height_px",
                "ambient",
                "diffuse",
            },
            "render",
        )
        v2_render: dict[str, object] | None = None
    else:
        render = {}
        v2_render = _v2_render_controls(raw["render"])
    input_keys = {
        "attitudes_csv",
        "frames_csv",
        "dsk",
        "frame_kernel_paths",
        "expected_dsk_center_id",
        "expected_dsk_frame_id",
        "expected_dsk_frame_name",
    }
    if (
        schema_version == 2
        and v2_render is not None
        and v2_render["illumination"]["mode"] == "point_sun_lambert"
    ):
        input_keys.add("illumination_csv")
    inputs = _exact_keys(raw["inputs"], input_keys, "inputs")
    stretch = _exact_keys(
        raw["isis_stretch"], {"minpercent", "maxpercent"}, "isis_stretch"
    )
    tool_names = _TOOL_NAMES_V1 if schema_version == 1 else _TOOL_NAMES_V2
    executables_raw = _exact_keys(raw["executables"], set(tool_names), "executables")
    run_dir_value = (
        Path(raw["output_run_dir"])
        if schema_version == 1
        else _v2_absolute_path(raw["output_run_dir"], "output_run_dir")
    )
    run_dir = run_dir_value.expanduser().resolve()
    if run_dir.exists() or run_dir.is_symlink():
        raise FileExistsError(f"output_run_dir already exists: {run_dir}")
    if not run_dir.is_absolute() or not run_dir.parent.is_dir():
        raise ValueError("output_run_dir must be a new absolute directory")

    if schema_version == 1:
        maximum_range = _finite(render["maximum_range_m"], "maximum_range_m")
        if maximum_range <= 0:
            raise ValueError("maximum_range_m must be positive")
        tile_width = _positive_int(str(render["tile_width_px"]), "tile_width_px")
        tile_height = _positive_int(
            str(render["tile_height_px"]), "tile_height_px"
        )
        _validate_tile_workset(
            tile_width, tile_height, field_prefix=""
        )
        ambient = _finite(render["ambient"], "ambient")
        diffuse = _finite(render["diffuse"], "diffuse")
        if (
            ambient < 0
            or diffuse < 0
            or abs(ambient + diffuse - 1.0) > 1.0e-12
        ):
            raise ValueError(
                "ambient and diffuse must be nonnegative and sum to one"
            )
    else:
        assert v2_render is not None
        maximum_range = float(v2_render["maximum_range_m"])
        tile_width = int(v2_render["tile_width_px"])
        tile_height = int(v2_render["tile_height_px"])
        ambient = 0.0
        diffuse = 0.0
    if schema_version == 1:
        minpercent = _finite(
            stretch["minpercent"], "isis_stretch.minpercent"
        )
        maxpercent = _finite(
            stretch["maxpercent"], "isis_stretch.maxpercent"
        )
    else:
        minpercent = _v2_json_number(
            stretch["minpercent"], "isis_stretch.minpercent"
        )
        maxpercent = _v2_json_number(
            stretch["maxpercent"], "isis_stretch.maxpercent"
        )
    if minpercent != 0.1 or maxpercent != 99.9:
        raise ValueError(
            "isis_stretch must use minpercent=0.1 and maxpercent=99.9"
        )
    jobs = (
        _positive_int(str(raw["jobs"]), "jobs")
        if schema_version == 1
        else _v2_positive_json_int(raw["jobs"], "jobs")
    )

    def input_path(name: str) -> Path:
        value = inputs[name]
        return (
            Path(value)
            if schema_version == 1
            else _v2_absolute_path(value, name)
        )

    attitudes_path = _regular_file(input_path("attitudes_csv"), "attitudes_csv")
    frames_path = _regular_file(input_path("frames_csv"), "frames_csv")
    dsk_path = _regular_file(input_path("dsk"), "dsk")
    kernel_values = inputs["frame_kernel_paths"]
    if not isinstance(kernel_values, list):
        raise ValueError("frame_kernel_paths must be an array")
    kernel_paths = tuple(
        _regular_file(
            (
                Path(value)
                if schema_version == 1
                else _v2_absolute_path(
                    value, f"frame_kernel_paths[{index}]"
                )
            ),
            f"frame_kernel_paths[{index}]",
        )
        for index, value in enumerate(kernel_values)
    )
    attitudes = load_attitudes(attitudes_path)
    frames = (
        load_frames(frames_path)
        if schema_version == 1
        else load_frames_v2(frames_path)
    )
    illumination: dict[str, dict[str, object]] | None = None
    illumination_path: Path | None = None
    illumination_fingerprint: dict[str, object] | None = None
    plate_albedo_path: Path | None = None
    plate_albedo_fingerprint: dict[str, object] | None = None
    if (
        schema_version == 2
        and v2_render is not None
        and v2_render["illumination"]["mode"] == "point_sun_lambert"
    ):
        illumination_path = _regular_file(
            input_path("illumination_csv"), "illumination_csv"
        )
        illumination_fingerprint = _fingerprint(illumination_path)
        illumination = load_illumination_v2(illumination_path)
        _assert_fingerprints_unchanged(
            {"illumination_csv": illumination_fingerprint},
            "after illumination parsing",
        )
    if (
        schema_version == 2
        and v2_render is not None
        and v2_render["surface_appearance"]["mode"] == "plate_albedo"
    ):
        surface = v2_render["surface_appearance"]
        assert isinstance(surface, dict)
        plate_albedo_path = _regular_file(
            _v2_absolute_path(
                surface["plate_albedo_csv"],
                "render.surface_appearance.plate_albedo_csv",
            ),
            "render.surface_appearance.plate_albedo_csv",
        )
        plate_albedo_fingerprint = _fingerprint(plate_albedo_path)
        _assert_fingerprints_unchanged(
            {"plate_albedo_csv": plate_albedo_fingerprint},
            "after plate albedo hashing",
        )
    if set(attitudes) != set(frames):
        raise ValueError("attitude/frame frame_id sets must match exactly")
    if illumination is not None and set(attitudes) != set(illumination):
        raise ValueError(
            "attitude/frame/illumination frame_id sets must match exactly"
        )
    for identifier in attitudes:
        if abs(float(attitudes[identifier]["et_tdb_s"]) -
               float(frames[identifier]["et_tdb_s"])) > 1.0e-6:
            raise ValueError(f"{identifier}: attitude/frame ET/TDB differs by more than 1e-6 s")
        if (
            illumination is not None
            and (
                abs(
                    float(attitudes[identifier]["et_tdb_s"])
                    - float(illumination[identifier]["et_tdb_s"])
                )
                > 1.0e-6
                or abs(
                    float(frames[identifier]["et_tdb_s"])
                    - float(illumination[identifier]["et_tdb_s"])
                )
                > 1.0e-6
            )
        ):
            raise ValueError(
                f"{identifier}: attitude/frame/illumination ET/TDB "
                "differs by more than 1e-6 s"
            )

    metadata = inspect_type2_dsk(dsk_path, kernel_paths)
    if schema_version == 1:
        expected_center_id = inputs["expected_dsk_center_id"]
        expected_frame_id = inputs["expected_dsk_frame_id"]
        resolved_expected_center_id = int(inputs["expected_dsk_center_id"])
        resolved_expected_frame_id = int(inputs["expected_dsk_frame_id"])
    else:
        expected_center_id = _v2_json_cpp_int(
            inputs["expected_dsk_center_id"], "expected_dsk_center_id"
        )
        expected_frame_id = _v2_json_cpp_int(
            inputs["expected_dsk_frame_id"], "expected_dsk_frame_id"
        )
        resolved_expected_center_id = expected_center_id
        resolved_expected_frame_id = expected_frame_id
    expected_metadata = {
        "center_id": expected_center_id,
        "frame_id": expected_frame_id,
        "frame_name": inputs["expected_dsk_frame_name"],
        "segment_count": 1,
        "data_type": 2,
    }
    for field, expected in expected_metadata.items():
        if metadata.get(field) != expected:
            raise ValueError(
                f"DSK {field}: expected {expected!r}, got {metadata.get(field)!r}"
            )
    if int(metadata.get("vertex_count", 0)) <= 0 or int(metadata.get("plate_count", 0)) <= 0:
        raise ValueError("DSK vertex_count and plate_count must be positive")

    conda_prefix = Path(os.environ.get("CONDA_PREFIX", "")).resolve()
    if conda_prefix.name != "asp360_new":
        raise ValueError("CONDA_PREFIX must resolve to asp360_new")
    tools = {
        name: _executable(
            (
                Path(executables_raw[name])
                if schema_version == 1
                else _v2_absolute_path(executables_raw[name], name)
            ),
            name,
        )
        for name in tool_names
    }
    protected_root = Path(os.environ.get("ISISROOT", conda_prefix)).resolve()
    native = tools["native"]
    if native.name != "smallbody_dsk_render":
        raise ValueError(
            "native executable must be the repository-built smallbody_dsk_render"
        )
    if native.is_relative_to(protected_root):
        raise ValueError("native executable must be repository-built outside ISISROOT")
    for name in tool_names[1:]:
        if not tools[name].is_relative_to(conda_prefix):
            raise ValueError(f"{name} executable must resolve inside asp360_new")

    version: str | None = None
    if schema_version == 1:
        version = subprocess.run(
            [str(tools["asp_version"]), "--version"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        if not version:
            raise ValueError("asp_version produced an empty version string")
    package_records: dict[str, Path] = {}
    for package in ("isis", "bullet", "cspice", "embree"):
        matches = sorted((conda_prefix / "conda-meta").glob(f"{package}-*.json"))
        if len(matches) != 1:
            raise ValueError(f"conda-meta must contain one {package} package record")
        package_records[package] = matches[0].resolve()

    paths_by_role: dict[str, Path] = {
        "raw_config_source": config_source,
        "attitudes_csv": attitudes_path,
        "frames_csv": frames_path,
        "dsk": dsk_path,
        **(
            {"illumination_csv": illumination_path}
            if illumination_path is not None
            else {}
        ),
        **(
            {"plate_albedo_csv": plate_albedo_path}
            if plate_albedo_path is not None
            else {}
        ),
        **{f"frame_kernel[{i}]": path for i, path in enumerate(kernel_paths)},
        **{f"camera[{identifier}]": Path(str(frames[identifier]["camera_path"]))
           for identifier in frames},
        **{f"executable[{name}]": path for name, path in tools.items()},
        **{f"conda_record[{name}]": path for name, path in package_records.items()},
    }
    fingerprints = {
        role: (
            illumination_fingerprint
            if role == "illumination_csv"
            else plate_albedo_fingerprint
            if role == "plate_albedo_csv"
            else _fingerprint(path)
        )
        for role, path in paths_by_role.items()
    }
    _assert_fingerprints_unchanged(fingerprints, "after hashing")

    frame_payload = []
    for identifier in frames:
        frame = frames[identifier]
        attitude = attitudes[identifier]
        camera_path = Path(str(frame["camera_path"]))
        frame_payload.append(
            {
                **frame,
                "camera_sha256": sha256_file(camera_path),
                "q_dsk_to_j2000_wxyz": attitude["q_dsk_to_j2000_wxyz"],
                **(
                    {
                        "target_to_sun_j2000_m": illumination[identifier][
                            "target_to_sun_j2000_m"
                        ]
                    }
                    if illumination is not None
                    else {}
                ),
            }
        )
    resolved_render: dict[str, object]
    if schema_version == 1:
        resolved_render = {
            "maximum_range_m": maximum_range,
            "tile_width_px": tile_width,
            "tile_height_px": tile_height,
            "photometry": {"ambient": ambient, "diffuse": diffuse},
        }
    else:
        assert v2_render is not None
        resolved_render = json.loads(json.dumps(v2_render))
        if illumination_fingerprint is not None:
            resolved_illumination = resolved_render["illumination"]
            assert isinstance(resolved_illumination, dict)
            resolved_illumination["state_source"] = "illumination_csv"
            resolved_illumination["state_sha256"] = illumination_fingerprint[
                "sha256"
            ]
        if plate_albedo_fingerprint is not None:
            resolved_surface = resolved_render["surface_appearance"]
            assert isinstance(resolved_surface, dict)
            resolved_surface.pop("plate_albedo_csv")
            resolved_surface["plate_albedo_path"] = str(plate_albedo_path)
            resolved_surface["plate_albedo_sha256"] = plate_albedo_fingerprint[
                "sha256"
            ]
    resolved_payload = {
        "schema_version": schema_version,
        "dsk": {
            "path": str(dsk_path),
            "sha256": sha256_file(dsk_path),
            "expected_center_id": resolved_expected_center_id,
            "expected_frame_id": resolved_expected_frame_id,
            "expected_frame_name": str(inputs["expected_dsk_frame_name"]),
            "frame_kernel_paths": [str(path) for path in kernel_paths],
        },
        "render": resolved_render,
        "isis_stretch": {"minpercent": minpercent, "maxpercent": maxpercent},
        "batch": {"jobs": jobs},
        "executables": {name: str(path) for name, path in tools.items()},
        "frames": frame_payload,
    }
    raw_snapshot_bytes = _json_bytes(raw)
    resolved_bytes = _json_bytes(resolved_payload)
    fingerprint_payload = {
        "schema_version": schema_version,
        **(
            {"asp_version": version}
            if schema_version == 1
            else {"asp": dict(_V2_ASP_PROVENANCE)}
        ),
        "inputs": fingerprints,
        "snapshots": {
            "raw_config.json": hashlib.sha256(raw_snapshot_bytes).hexdigest(),
            "resolved_config.json": hashlib.sha256(resolved_bytes).hexdigest(),
        },
    }
    run_dir.mkdir()
    raw_snapshot = run_dir / "raw_config.json"
    resolved_path = run_dir / "resolved_config.json"
    fingerprints_path = run_dir / "input_fingerprints.json"
    atomic_write_bytes(raw_snapshot, raw_snapshot_bytes)
    _assert_fingerprints_unchanged(fingerprints, "before resolved-config publication")
    atomic_write_bytes(resolved_path, resolved_bytes)
    atomic_write_bytes(fingerprints_path, _json_bytes(fingerprint_payload))
    return ResolvedRun(
        raw_config=raw_snapshot,
        resolved_config=resolved_path,
        run_dir=run_dir,
        frame_ids=tuple(frames),
        native_executable=native,
        std2isis_executable=tools["std2isis"],
        isis2std_executable=tools["isis2std"],
        stats_executable=tools["stats"],
        tiffinfo_executable=tools["tiffinfo"],
        gdalinfo_executable=tools["gdalinfo"],
        asp_version_executable=tools.get("asp_version"),
        parallel_executable=tools["parallel"],
        jobs=jobs,
        input_fingerprints=fingerprints,
    )


def build_parallel_render_command(
    resolved: ResolvedRun,
    frame_ids: tuple[str, ...],
    attempts_root: Path,
    joblog: Path,
) -> list[str]:
    if not frame_ids:
        raise ValueError("parallel render requires at least one frame_id")
    identifiers = tuple(_frame_id(value, "frame_id") for value in frame_ids)
    attempts = Path(attempts_root)
    return [
        str(resolved.parallel_executable),
        "--jobs",
        str(resolved.jobs),
        "--halt",
        "soon,fail=1",
        "--joblog",
        str(joblog),
        "--results",
        str(attempts / "parallel-results"),
        str(resolved.native_executable),
        "--resolved-config",
        str(resolved.resolved_config),
        "--frame-id",
        "{}",
        "--output-dir",
        str(attempts / "{}" / "frame_staging"),
        ":::",
        *identifiers,
    ]


def _load_json_object(path: Path, role: str) -> dict[str, Any]:
    source = Path(path)
    if source.is_symlink() or not source.is_file():
        raise ValueError(f"{role}: missing regular file: {source}")
    try:
        payload = json.loads(source.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"{role}: invalid JSON: {error}") from error
    if not isinstance(payload, dict):
        raise ValueError(f"{role}: JSON root must be an object")
    return payload


def _verify_published_frame(
    resolved: ResolvedRun, frame_id: str
) -> dict[str, object]:
    frame_dir = resolved.run_dir / "frames" / frame_id
    resolved_payload = _load_json_object(
        resolved.resolved_config, "resolved_config"
    )
    illumination_mode = _illumination_mode_from_resolved(resolved_payload)
    surface_appearance_mode = _surface_appearance_mode_from_resolved(
        resolved_payload
    )
    formal_product_names = _formal_product_names(
        illumination_mode, surface_appearance_mode
    )
    _require_exact_regular_files(
        frame_dir, set(formal_product_names), "published frame"
    )
    frame_manifest = _load_json_object(
        frame_dir / "frame.json", f"published frame {frame_id}"
    )
    frame = frame_manifest.get("frame")
    hashes = frame_manifest.get("output_sha256")
    expected_names = set(formal_product_names - {"frame.json"})
    manifest_schema_version = frame_manifest.get("schema_version")
    if (
        type(manifest_schema_version) is not int
        or manifest_schema_version != 1
        or frame_manifest.get("status") != "passed"
        or not isinstance(frame, dict)
        or frame.get("frame_id") != frame_id
        or not isinstance(hashes, dict)
        or set(hashes) != expected_names
    ):
        raise RuntimeError(f"published frame {frame_id} manifest is invalid")
    for name, expected in hashes.items():
        if (
            not isinstance(expected, str)
            or sha256_file(frame_dir / name) != expected
        ):
            raise RuntimeError(
                f"published frame {frame_id} product hash changed: {name}"
            )
    return {
        "frame_id": frame_id,
        "frame_manifest_sha256": sha256_file(frame_dir / "frame.json"),
        "products": hashes,
    }


def _published_rows(resolved: ResolvedRun) -> list[dict[str, object]]:
    frames_dir = resolved.run_dir / "frames"
    if frames_dir.is_symlink() or not frames_dir.is_dir():
        raise ValueError(f"frames directory is missing: {frames_dir}")
    actual = {entry.name for entry in frames_dir.iterdir()}
    unknown = actual - set(resolved.frame_ids)
    if unknown:
        raise ValueError(f"frames directory contains unknown entries: {sorted(unknown)}")
    return [
        _verify_published_frame(resolved, identifier)
        for identifier in resolved.frame_ids
        if (frames_dir / identifier).exists()
    ]


def verify_resume_state(run_dir: Path) -> ResolvedRun:
    source = Path(run_dir)
    if source.is_symlink() or not source.is_dir():
        raise ValueError(f"resume run directory is missing or a symlink: {source}")
    run = source.resolve()
    manifest = run / "manifest.json"
    if manifest.exists() or manifest.is_symlink():
        raise FileExistsError(f"batch manifest already exists: {manifest}")
    raw_path = run / "raw_config.json"
    resolved_path = run / "resolved_config.json"
    fingerprints_path = run / "input_fingerprints.json"
    raw = _load_json_object(raw_path, "raw_config")
    resolved_payload = _load_json_object(resolved_path, "resolved_config")
    fingerprint_payload = _load_json_object(
        fingerprints_path, "input_fingerprints"
    )
    snapshots = fingerprint_payload.get("snapshots")
    if not isinstance(snapshots, dict):
        raise ValueError("input_fingerprints snapshots are missing")
    for name, path in (
        ("raw_config.json", raw_path),
        ("resolved_config.json", resolved_path),
    ):
        if snapshots.get(name) != sha256_file(path):
            raise RuntimeError(f"{name.removesuffix('.json')} changed since preflight")
    if Path(str(raw.get("output_run_dir", ""))).expanduser().resolve() != run:
        raise ValueError("raw_config output_run_dir does not match resume directory")
    inputs = fingerprint_payload.get("inputs")
    if not isinstance(inputs, dict):
        raise ValueError("input_fingerprints inputs are missing")
    _assert_fingerprints_unchanged(inputs, "resume verification")

    frames = resolved_payload.get("frames")
    batch = resolved_payload.get("batch")
    executables = resolved_payload.get("executables")
    if (
        not isinstance(frames, list)
        or not frames
        or not isinstance(batch, dict)
        or not isinstance(executables, dict)
    ):
        raise ValueError("resolved_config batch contract is invalid")
    frame_ids = tuple(
        _frame_id(row.get("frame_id"), "resolved frame_id")
        for row in frames
        if isinstance(row, dict)
    )
    if len(frame_ids) != len(frames) or len(set(frame_ids)) != len(frame_ids):
        raise ValueError("resolved_config frame IDs are invalid or duplicated")
    schema_version = resolved_payload.get("schema_version")
    if type(schema_version) is not int or schema_version not in (1, 2):
        raise ValueError("resolved_config schema_version is invalid")
    tool_names = _TOOL_NAMES_V1 if schema_version == 1 else _TOOL_NAMES_V2
    if set(executables) != set(tool_names):
        raise ValueError("resolved_config executable contract is invalid")
    if schema_version == 2 and (
        "asp" in resolved_payload
        or fingerprint_payload.get("asp") != _V2_ASP_PROVENANCE
        or "asp_version" in fingerprint_payload
    ):
        raise ValueError("schema-2 ASP provenance contract is invalid")
    tools = {
        name: Path(str(executables.get(name, ""))).resolve()
        for name in tool_names
    }
    resolved = ResolvedRun(
        raw_config=raw_path,
        resolved_config=resolved_path,
        run_dir=run,
        frame_ids=frame_ids,
        native_executable=tools["native"],
        std2isis_executable=tools["std2isis"],
        isis2std_executable=tools["isis2std"],
        stats_executable=tools["stats"],
        tiffinfo_executable=tools["tiffinfo"],
        gdalinfo_executable=tools["gdalinfo"],
        asp_version_executable=tools.get("asp_version"),
        parallel_executable=tools["parallel"],
        jobs=_positive_int(str(batch.get("jobs")), "resolved batch.jobs"),
        input_fingerprints=inputs,
    )
    _published_rows(resolved)
    return resolved


def _next_attempt_root(run_dir: Path) -> Path:
    attempts = run_dir / "attempts"
    attempts.mkdir(exist_ok=True)
    if attempts.is_symlink() or not attempts.is_dir():
        raise ValueError(f"attempts path is not a regular directory: {attempts}")
    numbers: list[int] = []
    for entry in attempts.iterdir():
        match = re.fullmatch(r"attempt-([0-9]{4})", entry.name)
        if match:
            if entry.is_symlink() or not entry.is_dir():
                raise ValueError(f"invalid attempt entry: {entry}")
            numbers.append(int(match.group(1)))
    root = attempts / f"attempt-{(max(numbers, default=0) + 1):04d}"
    root.mkdir()
    return root


def _parse_parallel_joblog(path: Path) -> dict[str, dict[str, str]]:
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"GNU parallel joblog is missing: {path}")
    rows: dict[str, dict[str, str]] = {}
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        required = {"Exitval", "Signal", "Command"}
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            raise RuntimeError("GNU parallel joblog header is invalid")
        for row in reader:
            try:
                argv = shlex.split(row["Command"])
                index = argv.index("--frame-id")
                identifier = _frame_id(argv[index + 1], "joblog frame_id")
            except (ValueError, IndexError) as error:
                raise RuntimeError(
                    "GNU parallel joblog command has no valid --frame-id"
                ) from error
            if identifier in rows:
                raise RuntimeError(f"GNU parallel joblog duplicates {identifier}")
            rows[identifier] = row
    return rows


def _failure_payload(attempt_root: Path, frame_id: str) -> dict[str, object]:
    path = attempt_root / frame_id / "frame_staging" / "failure.json"
    if path.is_file() and not path.is_symlink():
        payload = _load_json_object(path, f"native failure {frame_id}")
        payload.setdefault("frame_id", frame_id)
        return payload
    return {
        "schema_version": 1,
        "status": "failed",
        "category": "NATIVE_PROCESS_FAILED",
        "frame_id": frame_id,
    }


def _write_batch_manifest(
    resolved: ResolvedRun, rows: list[dict[str, object]]
) -> None:
    _assert_fingerprints_unchanged(
        resolved.input_fingerprints, "before batch manifest publication"
    )
    manifest = {
        "schema_version": 1,
        "status": "passed",
        "frame_ids": list(resolved.frame_ids),
        "frame_count": len(resolved.frame_ids),
        "jobs": resolved.jobs,
        "coordinate_contract": {
            "origin": "DSK shape-model reference center",
            "axes": "J2000-parallel",
            "time_scale": "ET/TDB seconds",
            "internal_length_unit": "metre",
            "quaternion_order": "wxyz",
        },
        "input_fingerprints": resolved.input_fingerprints,
        "frames": rows,
    }
    atomic_write_bytes(resolved.run_dir / "manifest.json", _json_bytes(manifest))


def _run_scheduled_frames(
    resolved: ResolvedRun, frame_ids: tuple[str, ...]
) -> dict[str, object]:
    attempt_root = _next_attempt_root(resolved.run_dir)
    for identifier in frame_ids:
        (attempt_root / identifier).mkdir()
    joblog = attempt_root / "parallel-joblog.tsv"
    command = build_parallel_render_command(
        resolved, frame_ids, attempt_root, joblog
    )
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
    )
    atomic_write_bytes(
        attempt_root / "parallel-driver.stdout",
        result.stdout.encode("utf-8"),
    )
    atomic_write_bytes(
        attempt_root / "parallel-driver.stderr",
        result.stderr.encode("utf-8"),
    )
    try:
        job_rows = _parse_parallel_joblog(joblog)
    except RuntimeError as error:
        return {
            "schema_version": 1,
            "status": "failed",
            "exit_code": 3,
            "attempt_dir": str(attempt_root),
            "scheduled_frame_ids": list(frame_ids),
            "published_frame_ids": [],
            "failures": [
                {"category": "PARALLEL_JOBLOG_INVALID", "message": str(error)}
            ],
        }

    native_failures: list[dict[str, object]] = []
    published: list[str] = []
    for identifier in frame_ids:
        row = job_rows.get(identifier)
        if row is None:
            native_failures.append(
                {
                    "schema_version": 1,
                    "status": "failed",
                    "category": "UNSTARTED_AFTER_PARALLEL_HALT",
                    "frame_id": identifier,
                }
            )
            continue
        if int(row["Exitval"]) != 0 or int(row["Signal"]) != 0:
            native_failures.append(_failure_payload(attempt_root, identifier))
            continue
        _assert_fingerprints_unchanged(
            resolved.input_fingerprints,
            f"before frame {identifier} conversion",
        )
        try:
            validate_and_publish_frame(
                resolved, identifier, attempt_root / identifier
            )
        except ExternalCommandError as error:
            return {
                "schema_version": 1,
                "status": "failed",
                "exit_code": 4,
                "attempt_dir": str(attempt_root),
                "scheduled_frame_ids": list(frame_ids),
                "published_frame_ids": published,
                "failures": [
                    {
                        "category": "ISIS_CONVERSION_FAILED",
                        "frame_id": identifier,
                        "message": str(error),
                    }
                ],
            }
        except PublishedDurabilityUnconfirmedError as error:
            return {
                "schema_version": 1,
                "status": "failed",
                "exit_code": 5,
                "attempt_dir": str(attempt_root),
                "scheduled_frame_ids": list(frame_ids),
                "published_frame_ids": published,
                "failures": [
                    {
                        "category": "PUBLISHED_DURABILITY_UNCONFIRMED",
                        "frame_id": identifier,
                        "destination": str(error.destination),
                        "message": str(error),
                    }
                ],
            }
        except Exception as error:
            return {
                "schema_version": 1,
                "status": "failed",
                "exit_code": 5,
                "attempt_dir": str(attempt_root),
                "scheduled_frame_ids": list(frame_ids),
                "published_frame_ids": published,
                "failures": [
                    {
                        "category": "OUTPUT_VALIDATION_FAILED",
                        "frame_id": identifier,
                        "message": str(error),
                    }
                ],
            }
        published.append(identifier)

    if native_failures or result.returncode != 0:
        return {
            "schema_version": 1,
            "status": "failed",
            "exit_code": 3,
            "attempt_dir": str(attempt_root),
            "scheduled_frame_ids": list(frame_ids),
            "published_frame_ids": published,
            "failures": native_failures,
        }
    rows = _published_rows(resolved)
    if len(rows) != len(resolved.frame_ids):
        return {
            "schema_version": 1,
            "status": "failed",
            "exit_code": 5,
            "attempt_dir": str(attempt_root),
            "scheduled_frame_ids": list(frame_ids),
            "published_frame_ids": published,
            "failures": [{"category": "INCOMPLETE_FRAME_SET"}],
        }
    _write_batch_manifest(resolved, rows)
    return {
        "schema_version": 1,
        "status": "passed",
        "exit_code": 0,
        "attempt_dir": str(attempt_root),
        "scheduled_frame_ids": list(frame_ids),
        "published_frame_ids": published,
        "manifest": str(resolved.run_dir / "manifest.json"),
    }


def run_render_batch(config_path: Path) -> dict[str, object]:
    resolved = preflight_render_run(config_path)
    (resolved.run_dir / "frames").mkdir()
    return _run_scheduled_frames(resolved, resolved.frame_ids)


def resume_render_batch(run_dir: Path) -> dict[str, object]:
    resolved = verify_resume_state(run_dir)
    published = {
        row["frame_id"] for row in _published_rows(resolved)
    }
    missing = tuple(
        identifier
        for identifier in resolved.frame_ids
        if identifier not in published
    )
    if not missing:
        rows = _published_rows(resolved)
        _write_batch_manifest(resolved, rows)
        return {
            "schema_version": 1,
            "status": "passed",
            "exit_code": 0,
            "scheduled_frame_ids": [],
            "published_frame_ids": [],
            "manifest": str(resolved.run_dir / "manifest.json"),
        }
    return _run_scheduled_frames(resolved, missing)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("--config", type=Path)
    selection.add_argument("--resume", type=Path)
    try:
        arguments = parser.parse_args(argv)
        if arguments.config is not None:
            summary = run_render_batch(arguments.config)
        else:
            summary = resume_render_batch(arguments.resume)
    except SystemExit as error:
        if error.code == 0:
            return 0
        print(json.dumps({"status": "failed", "exit_code": 2}))
        return 2
    except Exception as error:
        print(
            json.dumps(
                {
                    "schema_version": 1,
                    "status": "failed",
                    "exit_code": 2,
                    "message": str(error),
                },
                sort_keys=True,
            )
        )
        return 2
    print(json.dumps(summary, sort_keys=True))
    return int(summary["exit_code"])
