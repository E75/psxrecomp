#!/usr/bin/env python3
"""Match display's cap ([video] match_display_max_lines) reaches every site.

internal_resolution_test checks the pure rule (psx_ir_match_display_lines)
and video_enhancement_settings_test the game.toml parse. This test checks the
runtime between them, where a lost line would leave the cap inert:

1. main() copies gc.runtime.video_match_display_max_lines into the global
   that ir_display_lines() reads.
2. Every Match display consumer (launcher seed and adopt, in both launcher
   paths; apply at boot; the GL context's per-monitor measurement) is fed
   ir_display_lines(), and the monitor height is read raw only inside
   ir_display_lines() and for the context's log line.
3. ir_display_lines() itself, compiled against internal_resolution.h with a
   stub monitor, turns a 2338 px Retina panel into 4x under a 960-line cap,
   6x under 1440, and 10x with no cap.
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

RUNTIME = Path(__file__).resolve().parents[1]
MAIN = (RUNTIME / "src" / "main.cpp").read_text(encoding="utf-8")


def call_args(text, start):
    """Text of the balanced (...) that opens at or after `start`."""
    i = text.index("(", start)
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                return text[i + 1:j]
    raise AssertionError("unbalanced call at %d" % start)


def check_wiring():
    assert re.search(r"g_video_match_display_max_lines\s*=\s*"
                     r"gc\.runtime\.video_match_display_max_lines\s*;", MAIN), (
        "main() must copy game.toml [video] match_display_max_lines into "
        "g_video_match_display_max_lines")

    start = MAIN.index("static int ir_display_lines(SDL_Window* window) {")
    end = MAIN.index("\n}\n", start) + 3
    helper = MAIN[start:end]
    assert "psx_ir_match_display_lines(" in helper
    assert "g_video_match_display_max_lines" in helper

    # The monitor height is read raw in two places only: the helper, and the
    # context block's log line, which prints it beside the capped height.
    raw = [m.start() for m in re.finditer(r"psx_sdl_display_pixel_height\(", MAIN)]
    outside = [p for p in raw if not start <= p < end]
    assert len(outside) == 1, (
        "Match display sites must use ir_display_lines(); raw "
        "psx_sdl_display_pixel_height() calls outside it at offsets %r" % outside)
    tail = MAIN[outside[0]:outside[0] + 200]
    assert re.match(r"psx_sdl_display_pixel_height\(sdl_window\);\s*\n\s*"
                    r"const int dh = ir_display_lines\(sdl_window\);",
                    tail), "the raw read must be the context block's log value"
    block = MAIN[outside[0]:MAIN.index("gr_set_scale(s);", outside[0])]
    assert re.search(r"psx_resolve_internal_scale\(PSX_IR_DISPLAY,\s*"
                     r"g_video_ref_lines,\s*dh,", block), (
        "the GL context's Match display scale must come from the capped dh")

    consumers = {
        "psx_ir_launcher_seed_supersampling(": 2,
        "psx_ir_adopt_launcher(": 2,
        "apply_internal_resolution(": 1,
    }
    for name, want in consumers.items():
        sites = [m.start() for m in re.finditer(re.escape(name), MAIN)
                 if not MAIN[max(0, m.start() - 12):m.start()].rstrip().endswith(
                     ("void", "static", "PsxIrAdopted", "int"))]
        assert len(sites) == want, "%s: %d call sites, expected %d" % (
            name, len(sites), want)
        for p in sites:
            assert "ir_display_lines(" in call_args(MAIN, p), (
                "%s at offset %d is not fed ir_display_lines()" % (name, p))
    return helper


PROBE = r'''
#include <cstdio>
#include "internal_resolution.h"
struct SDL_Window;
static int g_stub_monitor_h = 0;
static int psx_sdl_display_pixel_height(SDL_Window*) { return g_stub_monitor_h; }
static int g_video_match_display_max_lines = 0;
@HELPER@
int main() {
    const int cases[][3] = {
        /* monitor px, cap, expected Match display scale (240 lines) */
        {2338, 960, 4}, {2338, 1440, 6}, {2338, 0, 10},
        {1080, 960, 4}, {1080, 1440, 5}, {720, 960, 3}, {0, 960, 1},
    };
    int bad = 0;
    for (const auto& c : cases) {
        g_stub_monitor_h = c[0];
        g_video_match_display_max_lines = c[1];
        const int s = psx_resolve_internal_scale(PSX_IR_DISPLAY, 240,
                                                 ir_display_lines(nullptr), 32);
        if (s != c[2]) {
            std::printf("FAIL monitor %d cap %d: %dx, expected %dx\n",
                        c[0], c[1], s, c[2]);
            bad++;
        }
    }
    /* A fixed preset never reads the cap. */
    g_stub_monitor_h = 2338; g_video_match_display_max_lines = 960;
    if (psx_resolve_internal_scale(2160, 240, ir_display_lines(nullptr), 32) != 9) {
        std::printf("FAIL the 4K preset moved under a Match display cap\n");
        bad++;
    }
    return bad ? 1 : 0;
}
'''


def run_probe(helper, compiler):
    with tempfile.TemporaryDirectory(prefix="match display cap ") as tmp:
        work = Path(tmp)
        src = work / "probe.cpp"
        src.write_text(PROBE.replace("@HELPER@", helper), encoding="utf-8")
        exe = work / ("probe.exe" if os.name == "nt" else "probe")
        cc = shutil.which(compiler) or compiler
        subprocess.run([cc, "-std=c++17", "-O1", "-I" + str(RUNTIME / "include"),
                        str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--skip-probe", action="store_true",
                    help="text checks only (no GNU/Clang C++ compiler)")
    args = ap.parse_args()
    helper = check_wiring()
    if not args.skip_probe:
        run_probe(helper, args.compiler)
    print("match display cap wiring: config copied, 5 consumers fed "
          "ir_display_lines()%s" % ("" if args.skip_probe else
                                    ", helper scales checked"))


if __name__ == "__main__":
    main()
