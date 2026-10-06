# Frame rate: presentation above the guest's cadence

A game-owned mod can present a PS1 title at a higher rate than it renders
(60, 120, 240 Hz, the display's refresh, ...) without changing how fast the
game runs. Guest VBlanks, timers, CD, SPU and game logic stay at their stock
cadence; only what the OpenGL presenter shows between guest VBlanks changes.
Never use `psx_mod_set_native_vblank_rate` for this: it speeds up the whole
machine.

## Enabling it (mod_plugins.h)

From a trusted plugin's activation callback:

```c
psx_mod_set_frame_interpolation_source(PSX_MOD_FRAME_SOURCE_FLIP);   /* optional */
psx_mod_set_frame_interpolation_blend(PSX_MOD_FRAME_INTERPOLATION_LINEAR);
psx_mod_set_frame_interpolation(120);   /* 0 = follow the display refresh */
```

`psx_mod_set_frame_interpolation` accepts 0 or 60..1000, selects the OpenGL
renderer and turns vsync off (the presenter paces itself with a sleep+spin
schedule on the emulation thread). Blend, source and present mode are reset
to their defaults at every session start (first boot and the lobby rematch),
so a netplay session or a later offline one never inherits them.

`PSX_MOD_FRAME_INTERPOLATION_UNLIMITED` (1000, the top rate) is for a plugin
that supplies in-between images with render passes and wants as many as the
host affords; it is meant for `PSX_MOD_FRAME_PRESENT_CHANGED` (below), under
which the rate is a ceiling, not a workload.

## Blend modes

| Mode | Output between two source frames |
|---|---|
| `LINEAR` (default) | crossfade `mix(prev, cur, a)` |
| `MOTION_ADAPTIVE` | crossfade small changes, switch large changes at a = 0.5 |
| `HOLD` | no crossfade: repeat the newest frame (for titles whose plugin supplies in-between images with render passes) |

Blending never invents motion: moving objects ghost, and the image is one
source frame behind the newest one. True in-between frames need the game's
own draw code: see [RENDER_PASSES.md](RENDER_PASSES.md).

## Blend source: every VBlank or real flips

The presenter plans one output schedule per guest VBlank present.

- `PSX_MOD_FRAME_SOURCE_VBLANK` (default, and the historical behaviour):
  every guest VBlank is a new source frame. Right for games that flip every
  VBlank.
- `PSX_MOD_FRAME_SOURCE_FLIP`: a VBlank is a new source frame only when the
  game really flipped -- the displayed VRAM origin moved, or the displayed
  rect was redrawn. The presenter tracks the flip period P (VBlanks between
  flips, clamped 1..4) and gives VBlank k after a flip the blend window
  [k/P, (k+1)/P], so one crossfade spans the whole game frame. A 30 Hz game
  (P = 2) would otherwise blend for one VBlank and hold for the next. A frame
  that arrives late holds at a = 1; one that arrives early restarts from the
  frame that was fully shown.

`gl_interp` (TCP) reports `source`, `flip_period`, `captures` (new frames)
and `duplicates` (VBlanks that re-presented the same frame); a 30 Hz game in
FLIP mode shows about 30 captures and 30 duplicates per second.

## Presents: every output, or only changes

`psx_mod_set_frame_interpolation_present()` chooses which outputs are drawn.

- `PSX_MOD_FRAME_PRESENT_EVERY` (default): one present per output deadline,
  as before.
- `PSX_MOD_FRAME_PRESENT_CHANGED` (opt-in): at each output deadline the
  presenter works out what it would show (a pass image, the held newest
  frame, or a crossfade weight). When that is the picture already on screen
  it presents nothing and does not wait for that deadline: vsync is off, so
  the screen keeps the picture, and the host time is left to the game and to
  render passes. A picture that never changes is still presented every
  fourth guest VBlank, and every VBlank while the on-screen display is
  showing something, so overlays keep updating. A crossfade changes at every
  output, so `LINEAR` and `MOTION_ADAPTIVE` still present at the full rate;
  `HOLD` presents only new frames and new pass images, holds each pass image
  until the next one where a pass was shed (no crossfade), and copies the
  hold-last image only for a new frame. Every present carries a game-time
  stamp (source frame, phase); one older than the picture on screen -- a
  render-pass generation that arrives late -- is refused before it is drawn.
  Any other swap (a stock present, hold-last, stereo) clears the record of
  the picture on screen, so the next output is drawn.

`gl_interp` reports skipped presents as `present_dups`; `render_pass_stats`
reports refused stale presents as `nonmonotonic_rejects`.
`PSX_GL_PRESENT_TIME_TRACE=1` prints the game-time stamp of every
interpolation swap.

So with render passes and `CHANGED`, the rate is a ceiling, not a workload:
at 1000 (`PSX_MOD_FRAME_INTERPOLATION_UNLIMITED`) a 30 Hz game presents 30
times a second plus once per in-between image that fit.

## Catching up

A host that falls behind (a stall, a busy machine) runs the next intervals
without waiting until it is back on schedule, as the stock frame pacer does,
for up to 12 source periods (`FRAME_INTERP_CATCHUP_MAX_PERIODS`, the stock
pacer's `FRAME_PACER_CATCHUP_MAX_PERIODS`). Only past that is the schedule
re-anchored and the lost time forgiven, so interpolation never costs guest
time a run without it would keep. A VBlank that arrives after all of its
output deadlines still presents once, at once, as the stock presenter does,
so a new game frame is never skipped. Both hold in either present mode.

## Limits

- OpenGL only; a command-line `--renderer` override after activation drops
  it. Netplay sessions run without mods, so without interpolation.
- FMV (MDEC / 24-bit) frames suspend it, as does rewind.
- Vsync is off: on a fixed-refresh panel pick the display refresh rate.
  Above the refresh rate a monitor shows only some of the images: under a
  desktop compositor (macOS, a windowed or borderless window on Windows) it
  shows the newest at each refresh; without one (exclusive fullscreen on
  Windows, some Linux setups) a present in mid-scan tears.

Tests: `frame_interpolation_schedule_test` (runtime ctest) covers the phase
windows, the flip tracker, the new-frame decision, output counts per rate
at 30 Hz and the 12-period catch-up window;
`frame_interpolation_present_time_test` and
`frame_interpolation_present_emit_test` the game-time gate and the emission
seam the presenter swaps through; `frame_interpolation_present_time_wiring_test`
(source guard) that every present goes through it, opt-in, and that any
other swap clears the record of the picture on screen.
`frame_interpolation_flip_source` (source guard, recompiler ctest)
pins the wiring no unit test reaches: the session-start hand-off of the
source to the renderer, and `interp_capture`'s FLIP branch (origin and redraw
into the decision, an early return on a duplicate).
