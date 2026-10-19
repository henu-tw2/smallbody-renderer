<!--
Modified by: gengxun + Codex
Date: 2026-10-19
Summary: State the standalone renderer coordinate, time, unit, and invalid-value contract.
-->
# Coordinate and input contract

Let `D` be the DSK body-fixed frame, `J` the target-centred axes parallel to J2000, and `C` the calibrated camera frame. `R_JD` maps D coordinates into J coordinates and `R_JC` maps camera coordinates into J coordinates. Camera positions are vectors from the DSK shape centre to the optical centre, expressed in J, in metres.

Attitude rows join frame rows by an exact `frame_id` and an ET/TDB value within the configured tolerance. The renderer never selects a nearest epoch or interpolates a missing state. Quaternions are finite active `wxyz` rotations and must pass the proper-rotation checks. Invalid pixels are outside the valid hit mask or are non-finite.

DSK vertices and the SPICE DSK API use kilometres. The renderer converts at that boundary and keeps ray origins, hit points, depths, and tolerances in metres. A Type-2 DSK must have one validated segment, one target/frame identity, finite non-degenerate plates, and a stable plate index. A PLY/OBJ point cloud is not silently substituted for this contract.
