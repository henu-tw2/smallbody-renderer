# Modified by: gengxun + Codex
# Date: 2026-10-19
# Summary: Verify the standalone Python package accepts explicit generic attitude and camera rows.

import csv
from pathlib import Path

from smallbody_renderer.dsk_attitude_render import (
    load_attitudes,
    load_frames_v2,
)


def test_explicit_attitude_and_camera_rows_are_loaded(tmp_path: Path) -> None:
    attitude_path = tmp_path / "attitudes.csv"
    with attitude_path.open("w", newline="", encoding="ascii") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "frame_id", "et_tdb_s", "q_dsk_to_j2000_w",
                "q_dsk_to_j2000_x", "q_dsk_to_j2000_y", "q_dsk_to_j2000_z",
            ]
        )
        writer.writerow(["f0001", "10.0", "1", "0", "0", "0"])

    camera_path = tmp_path / "camera.tsai"
    camera_path.write_text(
        "VERSION_4\nPINHOLE\nfu = 100\nfv = 100\ncu = 2\ncv = 2\n"
        "u_direction = 1 0 0\nv_direction = 0 1 0\n"
        "w_direction = 0 0 1\nR = 1 0 0 0 1 0 0 0 1\n"
        "C = 0 0 10\npitch = 1\n", encoding="ascii"
    )
    frames_path = tmp_path / "frames.csv"
    with frames_path.open("w", newline="", encoding="ascii") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "frame_id", "et_tdb_s", "camera_path", "camera_model_format",
                "width_px", "height_px", "target_to_camera_j2000_x_m",
                "target_to_camera_j2000_y_m", "target_to_camera_j2000_z_m",
                "q_camera_to_j2000_w", "q_camera_to_j2000_x",
                "q_camera_to_j2000_y", "q_camera_to_j2000_z",
            ]
        )
        writer.writerow(["f0001", "10.0", camera_path, "asp_tsai_null", 4, 4,
                         0, 0, 10, 1, 0, 0, 0])

    attitudes = load_attitudes(attitude_path)
    frames = load_frames_v2(frames_path)
    assert attitudes["f0001"]["et_tdb_s"] == 10.0
    assert frames["f0001"]["camera_model_format"] == "asp_tsai_null"
    assert frames["f0001"]["target_to_camera_j2000_m"] == [0.0, 0.0, 10.0]
