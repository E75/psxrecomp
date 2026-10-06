#!/usr/bin/env python3
"""Keep the monotonic game-time gate on every GL interpolation present path."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GL = (ROOT / "src" / "gpu_gl_renderer.c").read_text(encoding="utf-8")


def body(signature):
    start = GL.index(signature)
    return GL[start:GL.index("\n}", start)]


draw = body("static int present_choice_draw(const PresentChoice *c) {")
check = draw.index("frame_interpolation_present_emit(")
reject = draw.index("s_present_time_rejects++;")
assert "present_choice_swap" in draw and check < reject, \
    "route actual GL swaps through the tested emission gate"

fallback = body("static void present_choose(uint64_t deadline, float alpha,")
assert "frame_interpolation_present_history_time(" in fallback

gen = body("static int pass_gen_choose(uint64_t deadline, PresentChoice *c) {")
assert "c->source_frame = g->source_frame;" in gen
assert "frame_interpolation_present_pass_phase(" in gen

promotion = body("static void pass_apply_promotion(void) {")
assert "g->source_frame = s_interp_captures;" in promotion

reset = body("static void interp_reset_history_unlocked(void) {")
assert "frame_interpolation_present_time_reset(&s_present_time);" in reset

# The game-time gate refuses presents only for a title that asked for
# changed-only presents; every other title presents as before.
assert "if (s_present_changed) {" in draw, \
    "the monotonic gate is opt-in (PSX_MOD_FRAME_PRESENT_CHANGED)"

# Any other swap (stock present, hold, stereo, FMV) makes the record of the
# picture on screen stale, so a later duplicate check cannot skip a change.
swap = body("static void gl_swap_with_osd(void) {")
assert "s_last_choice.kind = 0;" in swap, \
    "every swap clears the presenter's record of the picture on screen"

print("frame interpolation present-time wiring guard passed")
