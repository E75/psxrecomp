# Render passes: true in-between frames

Frame blending ([FRAME_RATE.md](FRAME_RATE.md)) crossfades finished frames; it
cannot show objects in positions the game never drew. A **render pass** lets a
trusted game plugin redraw its scene between two logic ticks with the game's
own draw code, with objects and camera placed part of the way from one tick
to the next. The pass never changes the game: guest time is frozen while it
runs and the machine is restored bit-for-bit afterwards. Its only product is
an image the presenter shows between the game's own frames.

Default off. Nothing happens unless a plugin calls the API below.

## API (mod_plugins.h)

```c
uint32_t psx_mod_render_pass_plan(uint32_t period_vblanks,
                                  uint32_t shown_after_vblanks,
                                  uint32_t *alpha_q16, uint32_t max);
int psx_mod_render_pass(struct CPUState *cpu, const PSXModRenderPass *pass,
                        PSXModRenderPassFn fn, void *user);
uint32_t psx_mod_render_pass_status(void);
```

Call both from an emulation-thread function-entry hook at the point where
the game has finished the logic of tick n+1 but the display still has to
flip to frame n. For a PsyQ double-buffered loop that is the entry of the
`VSync(0)` that precedes `PutDispEnv`.

Register that hook with `psx_mod_register_function_entry_plugin()` under the
plugin's manifest `[[plugin]]` id (see [MOD_PACKAGES.md](MOD_PACKAGES.md)):
it then runs only while the resolved mod plan activates the plugin, so never
while the package is disabled or in netplay. Register once, for example from
the plugin's constructor; a repeated id and address returns 0. Entry hooks
also fire for the guest functions a pass itself calls, the plugin's own
included, so a hook that plans passes must ignore entries made inside one.

1. `psx_mod_render_pass_plan(P, D, a, max)` returns the phases (Q16, in
   (0, 1)) at which the presenter will show frame n: its output deadlines
   during the P VBlanks the frame stays on screen, starting after D more
   VBlank presents. When the host cannot afford them all, an evenly spread
   subset is returned. 0 means "no passes this frame" (see Gates).
2. For each phase, `psx_mod_render_pass()` with the display rect the next
   flip shows (the DISPENV rect). `fn(cpu, user, alpha)` may write guest RAM
   and call guest functions with `psx_dispatch_call()`; it returns nonzero to
   keep the image. The first pass after a plan also captures frame n's own
   image (phase 0).

The presenter uses `PSX_MOD_FRAME_SOURCE_FLIP` to see the
flip, and a plugin that supplies passes normally selects
`PSX_MOD_FRAME_INTERPOLATION_HOLD`, so wherever no pass image applies the
newest game frame is repeated rather than crossfaded one frame late.

### When passes are unavailable

`psx_mod_render_pass_status()` says why a plan would be empty, the host-time
budget aside (an empty plan while it says `READY` was shed for time):

| Status | Meaning | Lasts |
|---|---|---|
| `READY` (0) | passes can run | |
| `NO_PRESENTER` (1) | not OpenGL, interpolation off or suspended (FMV), or not the FLIP source | until the presenter changes |
| `BACKEND` (2) | the renderer declines passes in its current mode | while that mode lasts |
| `DISABLED` (3) | switched off after repeated faults | the session |
| `SESSION` (4) | netplay, rollback, rewind, load/save replay, self-check resimulation | transient |
| `FAST_FORWARD` (5) | manual fast-forward, turbo-through-loads, FMV auto-skip, TCP turbo | transient |
| `BUSY` (6) | inside an exception, a pass or a GPU DMA walk, or no frame captured since the presenter's history restarted (a display mode change) | transient |

A plugin that relies on passes should not leave the player on stock-rate
frames while a lasting reason holds: it can switch the presenter to a
crossfade with `psx_mod_set_frame_interpolation_blend()`, which may be called
from a hook at any time and takes effect at the next present, and switch
back to `HOLD` when the status is `READY` again. A backend mode that cannot
host passes (for example a renderer path whose VRAM is not one texture) should
report `BACKEND` from `gl_renderer_pass_unavailable()`.

## Flip modes

`psx_mod_set_render_pass_flip()` says how the game flips relative to the pass
point (default `PENDING`, reset at every session start):

- `PENDING`: frame N is drawn and waits; the next flip shows the pass rect.
  This is the PsyQ `VSync(0)`-then-`PutDispEnv` loop, and also any game that
  flips only after the next frame is submitted (Vigilante 8: 2nd Offense
  flips from a VBlank callback armed by the next submit). Phase 0 is
  re-captured when the flip is seen, so a game that adds a last layer to the
  shown buffer after the pass point (V8:2 draws its HUD at the next submit)
  shows its own final image at phase 0.
- `SHOWN`: the game flips each frame as soon as it is drawn, so at the pass
  point frame N is already on screen. The rect is that on-screen rect; frame
  N and its in-betweens are shown from the next flip, one game frame later
  than stock (the latency of any interpolator with one frame of history).

Decide by decoding the game's DISPENV / DRAWENV tables and where it calls
`PutDispEnv` and `DrawOTag`, not by assumption: in V8:2 `DISPENV[i]` shows
the buffer `DRAWENV[1-i]` draws.

## Replaying a span of frame code

Re-implementing a game's per-frame draw call sequence in a plugin copies one
path through it (HUD, messages, pause menu, replay mode each branch
differently) and drifts from it. Instead, a plugin records the registers at
the start of the game's draw section during the real frame (an instruction
hook, `mod_instruction_sites`) and, inside a pass, replays that section:

```c
int psx_mod_run_guest_span(struct CPUState *cpu, uint32_t start_pc, uint32_t stop_pc);
```

Every PC in `[start_pc, stop_pc)` is interpreted from its RAM bytes, even
where a compiled body or a compiled resume point (the return address of a
call) begins, so the span may start in the middle of a compiled function.
Calls the span makes leave the range as plain transfers and run with the
full dispatch trampoline back to their return address (compiled code is CPS:
a callee's own calls and returns go through the trampoline). A call may be
the span's last instruction. Anything else that leaves the range ends the
span with failure. Only inside a pass: the sandbox rolls a failed or runaway
span back, and the watchdog landing closes an open span. The span is a
replay of the game's own instructions, never a substitute for code the
recompiler could not translate; it stays out of capture, alias seeding and
per-PC entry statistics, and fires instruction hooks as compiled code does.
`render_pass_stats` counts `spans` / `span_failures` and keeps the last
failure (`span_fail`: reason, exit PC, `$ra`, post-call PC).

## Blending game state (render_pass_motion.h)

What makes a pass an in-between image is the state the draw code reads,
placed part of the way between two logic ticks. `render_pass_motion.h` holds
that for any title:

```c
psx_motion_begin(set, tick);                       /* each frame, capture point */
psx_motion_track(set, PSX_MOTION_MATRIX, obj + 0x20, psx_motion_identity(type, fn, mesh));
psx_motion_prepare(set, &limits, &stats);          /* pair with the previous frame */
/* in each pass */
psx_motion_apply(set, alpha);                      /* write blends to guest RAM */
psx_motion_blend_at(set, kind, addr, alpha, &out); /* a value a hook hands on */
```

Kinds: PsyQ `MATRIX` (slerped rotation, linear translation), its translation
only (billboards), `VECTOR`, `SVECTOR`, 12-bit `ANGLES` (shorter arc) and a
`SCALAR`. Values pair by address and identity, so a heap block reused by
another object never blends between the two. `limits.move_per_tick` (game
units) and `limits.turn_radians` separate motion from placement: a value
past them was respawned, teleported or cut to and keeps its current value.
Measure the game's real motion first; V8:2's world spans about 2^27 units
per axis and moves up to 2^16 per frame.

## Per-frame driver

```c
int psx_mod_activate_render_pass_rate(const char *pkg, const char *feature,
                                      const char *option, uint32_t flip_mode);
uint32_t psx_mod_render_pass_frame(struct CPUState *cpu,
                                   const PSXModRenderPassFrame *frame,
                                   PSXModRenderPassFn fn, void *user);
```

The first reads a choice option (`display` or a rate) and selects the FLIP
source, HOLD blend, the flip mode and the rate. The second plans one game
frame, runs `fn` at each phase into the frame's rect, and switches the
presenter to a crossfade while passes are unavailable for a lasting reason
(back to HOLD when they return).

## Porting a title

1. Find the main loop's draw section: the range from camera setup through
   OT build and HUD to the submit call, and every call in it.
2. Decode DISPENV/DRAWENV and the submit/flip path: which buffer the next
   flip shows at the pass point, and in what order the game draws into it
   (immediate `DrawOTag`, VBlank callback). Pick the flip mode.
3. Find the pass point (a function entry after the section's OT build, before
   submit) and the logic tick counter.
4. Find what the draw code reads that moves: the camera input (often the
   argument of a set-camera function: track it with `blend_at` from an entry
   hook), object transforms and their identity, particles.
5. Measure motion per frame with histograms (mod counters) before setting
   limits.
6. Plugin: instruction hook at the section start (registers), capture at the
   pass point, `psx_mod_render_pass_frame` with a pass function that applies
   the blend, replays the span and submits as the screen gets it.
7. Verify in `--hidden-window` (below): `render_pass_stats` passes with no
   span failures, `render_pass_dump` images between their neighbours,
   `PSX_RENDER_PASS_VERIFY=1` with `verify_mismatch` 0.

`Vigilante82PSXRecomp/src/mods/v82_frame_rate_plugin.cpp` is the worked
example.

## Testing without a window

`--hidden-window` runs the full windowed runtime (presenter, interpolation,
render passes) in a window that is never shown and never opens a modal or a
file picker. `--headless` skips the presenter, so plans there are always
empty. Drive it over TCP; set `SDL_AUDIODRIVER=dummy` to keep it silent.

## What a pass may and may not do

While `fn` runs (`g_psx_render_pass_active`):

| Area | Behaviour | Where |
|---|---|---|
| Guest clock | cycles are counted (GTE and mult/div deadlines work) but no device is serviced, no VBlank or device event fires | `psx_cycles.c` freeze (`psx_cycle_freeze.h`, runtime-only: the codegen-hashed `psx_cycles.h` is untouched) |
| Interrupts | never delivered | `interrupts.c` |
| GPU DMA | linked lists and delayed completions finish synchronously | `dma.c` |
| RAM / scratchpad stores | written directly, bypassing code-page tracking, overlay watch, write traces and fingerprints; RAM addresses fold through the live geometry (2 MiB mirrored, or 8 MiB with the 8 MB RAM mod), as outside a pass | `memory.c` `render_pass_store` |
| MMIO stores | allowed: GP0, GP1 DMA mode / info, GPU and OTC DMA channels, DPCR/DICR, I_STAT/I_MASK. Dropped and counted: SPU (key-ons), CD, timers, SIO, MDEC, other DMA channels, memory control | `memory.c` |
| VRAM | only the declared rect; writes that bypass the scissor elsewhere (fills, copies, uploads, pokes: never a native-wide surface) are journaled and rolled back | `gpu_gl_renderer.c` |
| Runaway code | an 8 M guest-cycle watchdog rolls the pass back | `render_pass.c` |

After `fn` (success or not) everything is restored: CPU state with the GTE,
all of main RAM at its live size (2 MiB, or 8 MiB with the opt-in 8 MB RAM
mod; `psx_memory.h`), scratchpad, I-cache tags, I_STAT/I_MASK, timers, DMA
and GPU registers (without the widescreen side effects of a savestate load), the
VRAM rect (hr colour, mask stencil, raw 16-bit mirror, native-wide band, CPU
VRAM rows), the renderer's coherency bookkeeping, and every clock value.
A watchdog abort leaves by longjmp from inside guest code, skipping the
exits of the frames it leaves, so the host nesting those frames own is put
back from the checkpoint too: the cycle-deferral depth, the native overlay
unit depth, shard stack and cycle-flush hook, the DMA execution depth, and
the interpreter's active/phase/precise flags, resume latch and pending load
(`render_pass_abort_test`). A plugin must likewise not keep state that only
its callback's normal return resets.
After 8 faults (watchdog or refused writes) passes stay off for the session.

## Gates

The plan returns 0 in netplay, rollback resimulation, self-check, rewind,
lockstep, an exception, while a GPU DMA list is in flight, during manual
fast-forward, turbo-through-loads, FMV auto-skip and TCP turbo, on anything
but the OpenGL renderer with FLIP-source interpolation, while the presenter
is suspended (FMV), after repeated faults, and when no VBlank was presented
since the last plan (headless). `psx_mod_render_pass()` refuses under the
same conditions.

## Presentation and budget

The presenter keeps two generations of images: the frame on screen and the
one being built for the next flip. A generation becomes current when the
FLIP source sees the display flip to its rect; from that interval's start
its phases map onto host time. At each output deadline the newest image at
or before the deadline's phase is shown, crossfaded into the next one when
passes were shed (with `HOLD` and `PSX_MOD_FRAME_PRESENT_CHANGED`,
[FRAME_RATE.md](FRAME_RATE.md), held until the next one instead, and an
image older in game time than the one on screen is never shown). Deadlines that fall due while passes run are presented
between passes, so the frame on screen keeps moving. A frame that stays on
screen longer than the plan's period (a lagging tick: three VBlanks instead
of two) holds its newest image until the next flip, as a late stock frame
holds; it never steps back to the game's own image of the frame, which is
older (`late_presents` in `render_pass_stats`). Only a game that stops
flipping for four frame lengths expires the frame's images (`expired`), and
the presenter then shows its own newest capture.

Consecutive passes of one frame reuse the VRAM backup the first one took:
each pass's restore leaves the rect exactly as backed up, and no guest code
runs in between (the clock and guest store count are unchanged), so only the
first pass copies the rect out (`backups_reused` in `render_pass_stats`).

Passes cost host time inside the game's frame: they run on the emulation
thread. The budget per frame is `PSX_RENDER_PASS_BUDGET` percent (default 80)
of the presenter's idle time plus the presents beyond two per frame, learnt
from the previous frame, over a smoothed per-pass cost. When the budget runs
out, fewer passes are rendered, and a pass that costs more than the budget is
not planned at all. The cost is measured per presented image size: until
three passes have been measured at the current size (the first plans, and
after an aspect or internal-resolution change) a plan asks for one pass, so
an expensive size costs at most one pass per frame while it is learnt. The
three seed the average with their median, so one slow pass on a busy host
does not price passes out. A pass that creates pass textures or
framebuffers (the first frames at a size, or a slot filled for the first
time) is not a cost sample, at most eight in a row. Only passes that run
are measured, and the first passes of a race can all run in a transient (a
busy host, code run for the first time) that costs several times the steady
state; an estimate that high prices every plan out, so no pass would ever
correct it. An estimate that no pass has been measured against for 30
plans that wanted passes (about a second of a 30 Hz game) is measured again:
the warm-up restarts, with one pass per plan (`cost_rewarms` in
`render_pass_stats`). When the new median is not at least a quarter below
the old estimate, the old one was right (a size that is truly too
expensive, or a host at its limit), and the next wait doubles, up to 960
plans, so these one-pass warm-ups stay rare; a re-measure that finds the
old estimate stale keeps the wait at 30. A plugin can show a crossfade while
passes are shed (see "When passes are unavailable").

That is the default (`PSX_MOD_RENDER_PASS_IDLE`). It budgets last frame's
idle time over the whole frame, so nothing stops a pass from delaying the
game's own next frame.

### Leftover-time budget (opt-in)

`psx_mod_set_render_pass_budget(PSX_MOD_RENDER_PASS_LEFTOVER)` from
activation (reset at every session start; `#ifdef
PSX_MOD_RENDER_PASS_LEFTOVER` tells a plugin the runtime has it) makes passes
use only time the game leaves free, so they never delay its frame:

- **Budget.** A plan's budget is the host time left before the frame is
  first presented (`frame_start`), less the work the emulation thread still
  has to do for it (the reserve) and a margin of max(1 ms, 1/32 VBlank),
  times `render_pass_admission_pct` (65% up to 3x internal scale, 50% above;
  `PSX_RENDER_PASS_BUDGET` overrides it; with dynamic resolution, see
  below). The reserve is measured every
  frame -- the busy time between the end of the frame's passes and its
  start -- and errs late: a larger sample is taken at once, a smaller one
  moves it down by 3% of the difference. A frame that resumed more than
  1 ms later than the no-pass baseline adds the difference to it.
- **Deadline.** `psx_mod_render_pass()` does not start a pass that would end
  after `frame_start - reserve - margin` by its estimated cost (`skipped`),
  and the freeze polls host time at every tick (at most 16K guest cycles
  apart): a pass that runs into the deadline, less what its capture and
  restore still take, is stopped and rolled back like a watchdog abort, but
  it is not a fault (`cut`; never counts towards disabling passes, never
  sets `last_failure` or `last_abort_detail`). A pass stopped before its
  guest code ran is closed without a restore. Stereo eye pairs have no
  deadline.
- **Copies as draws.** Every copy a pass makes -- the rect backup and
  restore (hr, raw mirror, native-wide band), the out-of-rect journal, the
  image capture, a game VRAM copy's staging -- is a texel-for-texel draw
  (`pass_copy`), never `glBlitFramebuffer` or `glCopyTexSubImage2D`: on
  macOS GL every blit ends and submits the command buffer and waits for the
  GPU on the emulation thread (R4 on an M4: a pass's backup took 1 ms at
  native and 35 ms at 4K as blits, 0.2 ms as draws at both). The mask
  stencil is not copied; the restore re-derives it from the restored alpha,
  as `rebuild_mask_stencils` does, which is the same stencil wherever it is
  current (`PSX_RENDER_PASS_VERIFY` compares it whenever it was current at
  the pass's start). A presenter set up for passes (HOLD, FLIP) makes one
  4x4 copy with stencil at its first VBlank, so no pass pays the driver's
  pipeline build for the copy programs.
- **Cost.** Learnt only from passes that fit: the first completed pass sets
  the average and later ones move it; plans use the average plus twice its
  smoothed deviation. A cut pass is a lower bound. A pass of unknown cost is
  tried only when the budget is at least the thread's own work this
  interval plus the reserve (twice that above 3x), and at least a quarter
  VBlank. Costs are kept per presented image size (up to 8 sizes, least
  recently used replaced): returning to a size restores its estimate instead
  of measuring from nothing. An estimate that prices every plan out is probed with one pass
  after 30 such plans that had that much leftover time; a probe that
  confirms it, or is cut, doubles the wait up to 960 (`probes`).
- **Holds.** No passes while the GPU is still on work queued before the
  frame (a fence after each VBlank's presents; `gpu_busy`), for 30 VBlanks
  after one that left the thread no idle time without passes (`behind`),
  and per the pace guard: a game frame in which every guest VBlank arrived
  more than 1/8 VBlank late (the GPU fell behind where the thread's own
  timing does not show it) halves the passes allowed per frame; plans that
  keep pace let it grow by one every 8 plans, a growth that fails soon
  doubles that wait (up to 1024).
- **Dynamic resolution (one budget).** With `[video] dynamic_resolution`
  on, the dynamic-resolution controller is the only thing that changes the
  internal scale; passes are a per-frame consumer of the time it leaves.
  The scale moves, so the two thresholds above that are keyed on it would
  be a second resolution controller: instead the GPU counts as the limit
  (50% share, twice the probe minimum) from the start and after any plan
  the GPU fence refused or the pace guard cut, until 60 plans in a row saw
  neither. Each plan reports the estimated cost of the passes it wanted but
  shed (and of passes not started before their deadline) to the
  controller, and the plans in which the pace guard cut passes (the swap's
  block in that window is then counted as work, not vsync wait). With
  `dynamic_resolution_priority = "frame_rate"` the controller steps down a
  level after 2 s of shedding (never below `dynamic_resolution_min`) and
  steps up only when the game's and the passes' whole demand fit at the next
  level; the default `"resolution"` leaves passes what the resolution
  leaves. A step resets the present-time guard and the pass generations,
  skips the reserve sample and pace judgement it stalls, and holds cost
  probes and re-measures for 1 s.

`render_pass_stats` reports all of it under `leftover`: `skipped`, `cut`,
`budget_ms_avg`, `reserve_ms`, `probes`, how late the emulation thread
resumed at planned frames with and without passes (`resume_late_*`),
`frames_delayed`, `gpu_busy`, `behind`, `pace_cuts`, `pace_cap`,
`pace_vblanks`, `pace_late`. With passes on, `resume_late_pass_*` should
match `resume_late_nopass_*` and `frames_delayed` stay near zero.

Pass images are kept at internal resolution (the size the presenter
captures). Their textures are made as the slots fill, never more slots than
two generations fit in 256 MiB, which limits passes per frame at very high
internal resolutions; a size change frees the old set.

## Verifying a title

- `render_pass_stats` (TCP): passes, shedding, faults, dropped device
  stores, timing split (backup / guest code / capture / restore), presents
  made from pass images, and `status`.
  `refused` counts empty plans with wanted phases; pass-call refusals are
  separate: `pass_attempts`, `argument_refused`, `status_refused`,
  `begin_refused`, `checkpoint_refused`. `last_failure` is null until a refusal,
  then retains the failure-site reason, attempt/plan, guest cycle, rect/alpha,
  and GL begin inputs (actual requested/capture dimensions, scales, generation,
  source path and resource stage). FBO status and GL errors are numeric enums;
  `gl_error_before` is distinct from errors produced during that allocation.
  Success does not erase the record; a new mod session clears it.
- `render_pass_refuse on=1` (TCP) or `PSX_RENDER_PASS_REFUSE=1`: the backend
  declines passes (`BACKEND`), to test a plugin's fallback.
- `render_pass_dump path=<dir> count=<n>`: PNGs of the next n frames' images
  (the game's own frame, then each pass in phase order).
- `PSX_RENDER_PASS_VERIFY=1`: hash CPU, RAM, scratchpad, I-cache, interrupt,
  timer, DMA and GPU state and read back the VRAM rect before and after every
  pass -- hr colour, the raw mirror, the native-wide band, and the hr and
  wide mask stencils when they were current at the pass's start (stderr
  names the surface that differs)
  -- and check that a pass that returned normally left the host nesting
  balanced; `verify_mismatch` must stay 0. A pass closed before its guest
  code ran is checked too (the rect must be untouched).
- `PSX_RENDER_PASS_FORCE_MEASURE=1` (leftover planning only): one pass per
  eligible plan with a 200 ms deadline, past the pace guard and the GPU and
  slack holds, even where it cannot fit in spare time. It slows the game on
  purpose and must stay off for player runs: it is for measuring the pass
  and copy cost and checking `verify_mismatch` at an internal resolution
  where the planner correctly admits no passes.
- `PSX_RENDER_PASS_WATCHDOG=<guest cycles>`: lower the watchdog (default
  8 M) below a title's pass size to drive real passes through the rollback
  path; with fingerprints (below) the run must still match one without
  passes.
- `frame_fingerprint reset_on_load=1` then a savestate load: the per-frame
  write/MMIO/cycle fingerprints of a run with passes must equal a run
  without them.

Tests (runtime ctest unless noted):

- `render_pass_plan_test`: phase planning per rate, shedding (one pass while
  the cost is unknown), the cost warm-up and allocation rule, re-measuring a
  stale estimate (and backing off when it stays too expensive), selection,
  holding the newest image when the next flip is late, the MMIO allow-list.
- `render_pass_freeze_test`: 10^6 frozen cycles, exact clock restore,
  watchdog.
- `render_pass_sandbox_test`: the store policy `memory.c` routes every pass
  store through (SPU, CD, timer, other-DMA, GP1 and other device stores
  dropped and counted per class, never delivered; GP0, I_STAT/I_MASK and
  GPU/OTC DMA delivered); a real pass over `render_pass.c`, `psx_cycles.c`
  and `timers.c` that makes those stores and runs 10^6 cycles while a timer
  is armed to interrupt every 1000 (no device advances, no interrupt, RAM,
  scratchpad, I_STAT/I_MASK, timers and clock restored); status gates; the
  same store folding and restore with 8 MiB RAM live (unique addresses up to
  0x7FFFFF, restored after a pass, and a retail pass after it folding again);
  the out-of-rect VRAM journal policy and its exact rollback of the CPU VRAM
  rows.
- `render_pass_abort_test`: a watchdog abort from nested frames.
- `render_pass_guards` (source guard, recompiler ctest): the choke points
  above, and that the codegen-hashed headers do not carry the pass API.

The GPU half of the VRAM transaction (hr colour, stencil, raw mirror blits)
needs an OpenGL context and is checked at runtime by
`PSX_RENDER_PASS_VERIFY=1`.

### Nested mod callbacks and watchdog rollback

The host checkpoint includes the mod function-entry depth and current plugin
owner. A pass can interrupt an outer callback; restoring to zero would lose
that caller's context. Watchdog longjmp restores the saved depth/owner instead,
and normal-return verification checks their balance. This context stays local
to the committed session and is never part of a guest savestate.
`render_pass_stats.last_abort_detail` names skipped host exits and remains
latched across successful passes until session reset.

### Nested mod callback rollback

The host checkpoint includes function-entry depth and current plugin owner.
A pass can interrupt an outer callback; watchdog rollback restores its exact
saved context rather than resetting depth to zero. Normal-return verification
also checks balance. This host context is never serialized in guest savestates.
TCP render_pass_stats.last_abort_detail names skipped exits and remains latched
across successes until session reset.
