# Frame generation (`[video] frame_generation`)

Opt-in, OpenGL with the render thread only, off by default
(`PSX_FRAME_GEN=0/1` overrides for one run). Off, nothing in this document
runs and the renderer is the render thread of docs/RENDER_THREAD.md.

```toml
[video]
render_thread = true
frame_generation = true
```

## Rule

Real frames at the guest's rate come first and resolution is the only
quality lever for them (dynamic resolution). Frames above that are a bonus,
drawn by the render thread **from recorded data only**: no guest code runs
again, nothing is snapshotted or rolled back, and only when the render thread
has measured time to spare.

## How

- **Lists.** The render thread keeps a copy of every replayed state and
  drawing record (not transfers, copies, presents, steps). Display buffers are
  learned from presents; a list is one game frame's drawing into one buffer and
  closes when drawing moves to another buffer, or when the game flips to the
  buffer being drawn. (R4 starts drawing the next frame a VBlank before it
  flips, so a flip alone is not a frame boundary.)
- **Cadence.** A flip (a present of a different display address) is a new
  game frame; a present of the same buffer is the same frame again. R4
  updates every other VBlank, so generation interpolates between flips (30 Hz
  game frames), not between VBlanks.
- **Matching** (`frame_gen.c`). Triangles are keyed by op, texture page,
  CLUT, texture coordinates and the draw area they were clipped to
  (relative to their buffer); positions are relative to the buffer. Each
  newer triangle pairs, in draw order, with the nearest older one of the same
  key whose three vertices moved at most 96 native px and alike (within
  24 px of each other). R4 races pair 87-97 % of their triangles.
- **Drawing.** An in-between frame redraws the newer list into separate
  surfaces (a copy of the displayed buffer's hr rect and wide rows) with each
  matched triangle at `lerp(older, newer, t)`, sub-pixel via the precise
  triangle path; unmatched triangles, rects, lines, fills (2D, HUD) draw as
  the newer frame has them. The raw texture mirror is not packed from the
  generated surfaces; all bookkeeping the draws touch is restored, so the
  real stream is unaffected.
- **Schedule.** With `n` in-between frames per game frame, at the flip the
  real frame is composed and kept (its buffer may be drawn over before it is
  shown), frames `t = k/(n+1)` are presented `flip/(n+1)` apart, then the
  kept real frame. Timed work runs from the render-thread core's `tick`
  hook between records and while idle.
- **Latency.** Interpolation shows a real frame after the in-between frames
  that lead to it: up to `n/(n+1)` of a game frame later than without
  generation (R4 at 30 Hz on 120 Hz: 25 ms at n = 3, 17 ms at n = 1).
  Extrapolation (no delay, guessed motion) is not offered.
- **Plan.** `n = fg_plan(...)`: slots = round(flip interval × refresh)
  (R4 on 120 Hz: 4), limited to what fits in 85 % of the interval after the
  real frames' render-thread CPU time and one swap, at the measured cost of a
  generated frame (CPU, GPU via `GL_TIME_ELAPSED`, plus a swap). On macOS a
  real frame's `TIME_ELAPSED` span reads close to the whole interval (it
  counts the GPU waiting for records), so it is not used here; GPU overload
  shows as backpressure. The measured frame cost dynamic resolution uses
  excludes generation (its query is paused, segments summed).
- **Breaker.** Off for 3 s (doubling to 24 s on repeats within 10 s) after
  the queue backed up (the guest waited), the render thread fell two frames
  behind, or a guest frame took over 1.5 intervals. While dynamic resolution
  is over budget or stepping down, generation pauses (1 s, no escalation).
  One frame behind just shows the waiting real frame at once.
- **Sync points** show the waiting real frame and restart the lists at the
  next flip.

## Limits

- Only triangles move; sprites, lines and fills are the newer frame's.
- A texture the frame drew earlier into its own displayed buffer is sampled
  as the raw mirror holds it when the in-between frame is drawn.
- Single-buffered games never flip, so nothing is generated.
- Windowed high-resolution mode and depth24 frames are not generated.

## Debug

`{"cmd":"frame_gen"}` (not a sync point): generated / real presents, flips,
dups, plan (`last_n`, `slots`), costs (`real_ms`, `gen_ms`, `swap_ms`), last
match counts, breaker, last hold, total swaps.

## Tests

- `frame_gen_test`: matching (moved scenes, keys, move and deformation
  limits, duplicates, pair-once), interpolation endpoints, the plan, the
  breaker.
- `render_thread_test`: the tick hook runs on the render thread with the
  context, never early, and wakes an idle thread.
- `gl_frame_gen_test` (real GL, `gpu;opengl;hardware`): a double-buffered
  30 Hz scene, VRAM and native-wide presents, flipping right after drawing
  and a VBlank late: the distinct real presented images are identical with
  generation off and on, generated frames are presented, and an in-between
  frame composed at t = 1 / t = 0 equals the newer / older real frame.
