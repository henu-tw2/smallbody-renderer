# Modified by: gengxun + Codex
# Date: 2026-10-19
# Summary: Expose the standalone renderer's Python preflight and batch API.

from .dsk_attitude_render import main, preflight_render_run, run_render_batch

__all__ = ["main", "preflight_render_run", "run_render_batch"]
