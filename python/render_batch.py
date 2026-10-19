#!/usr/bin/env python3
# Modified by: gengxun + Codex
# Date: 2026-10-19
# Summary: Provide the standalone command-line entry point for DSK render batches.
"""Thin command-line entry point for new or resumed DSK render batches."""

from pathlib import Path
import sys

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from smallbody_renderer.dsk_attitude_render import main


if __name__ == "__main__":
    raise SystemExit(main())
