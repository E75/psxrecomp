#!/usr/bin/env python3
"""Pin the render-pass sandbox (docs/RENDER_PASSES.md) at its choke points.

A render pass runs guest draw code in frozen time and restores the machine
afterwards. That only holds if every path that could let time pass, deliver an
interrupt, touch a device the restore does not cover, or record the pass into
the live timeline checks g_psx_render_pass_active first. These are the choke
points; each guard below names the hole it closes.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "runtime" / "src"


def body(text, signature):
    """Return the first ~40 lines of the function whose definition starts with
    `signature` (enough to see its early-return gates)."""
    i = text.index(signature)
    return "\n".join(text[i:].splitlines()[:40])


cycles = (SRC / "psx_cycles.c").read_text(encoding="utf-8")
b = body(cycles, "void psx_devices_service_to_now(void) {")
assert re.search(r"if \(g_psx_render_pass_active\)[^\n]*return;", b), (
    "device servicing must stop during a pass (time would advance)")
b = body(cycles, "void psx_devices_mmio_sync(void) {")
assert "g_psx_render_pass_active" in b, (
    "MMIO sync must not catch devices up during a pass")
b = body(cycles, "void psx_advance_cycles_slow(uint32_t cycles) {")
assert "g_psx_render_pass_active" in b, (
    "the slow/conservative cycle path must not advance devices during a pass")

irq = (SRC / "interrupts.c").read_text(encoding="utf-8")
for sig in ("int psx_interrupt_delivery_needed(const CPUState* cpu) {",
            "void psx_check_interrupts(CPUState* cpu) {"):
    lines = body(irq, sig).splitlines()[:6]
    assert any("g_psx_render_pass_active" in l and "return" in l for l in lines), (
        "no interrupt may be delivered inside a pass: " + sig)

dma = (SRC / "dma.c").read_text(encoding="utf-8")
assert "while (g_psx_render_pass_active && gpu_linked_list.active)" in dma, (
    "GPU linked-list DMA must complete synchronously in a pass")
b = body(dma, "static void schedule_delayed_complete(int ch, uint32_t total_words,")
assert "g_psx_render_pass_active" in b, (
    "delayed DMA completion would never arrive in frozen time")

mem = (SRC / "memory.c").read_text(encoding="utf-8")
for fn in ("void psx_write_word(uint32_t addr, uint32_t val) {",
           "void psx_write_half(uint32_t addr, uint16_t val) {",
           "void psx_write_byte(uint32_t addr, uint8_t val) {"):
    lines = body(mem, fn).splitlines()[:6]
    assert any("render_pass_store" in l for l in lines), (
        "pass stores must bypass live-timeline observers: " + fn)
b = body(mem, "static void render_pass_store(")
assert "render_pass_store_to(&t, addr, val, width)" in b and \
    "g_render_pass_dropped_writes[cls]++" in b, (
    "pass stores must go through the tested store policy "
    "(render_pass_store_to, render_pass_sandbox_test) and count drops")
assert "t.ram_size = psx_ram_live_bytes();" in b, (
    "pass stores must fold RAM through the LIVE 2/8 MiB geometry, as "
    "psx_ram_map_write does (not the 8 MiB backing size)")
rp = (SRC / "render_pass.c").read_text(encoding="utf-8")
assert "uint32_t ram_bytes = memory_get_ram_bytes();" in body(
    rp, "static int checkpoint_save(") and \
    "memcpy(memory_get_ram_ptr(), s_ram_copy, s_ck.ram_bytes);" in body(
    rp, "static void checkpoint_restore(") and \
    "memory_get_ram_bytes()" in body(rp, "static uint64_t state_hash("), (
    "the pass checkpoint, restore and verify hash must cover the live RAM "
    "size (8 MiB with the 8 MB RAM mod)")
plan = (SRC / "render_pass_plan.c").read_text(encoding="utf-8")
assert "render_pass_mmio_class(phys, val, width)" in body(
    plan, "int render_pass_store_to("), (
    "pass MMIO stores must go through the MMIO allow-list")
gl = (SRC / "gpu_gl_renderer.c").read_text(encoding="utf-8")
assert "render_pass_vram_policy(&s_pj_cpu" in body(
    gl, "static int pass_refuse_write("), (
    "out-of-rect VRAM writes must go through the tested journal policy")
assert "render_pass_journal_rollback(&s_pj_cpu" in body(
    gl, "static void pass_journal_rollback("), (
    "the journal rollback must restore the CPU VRAM rows")
# The journal backs up the hr surface, the raw mirror and the CPU rows. That
# is complete in native-wide too only while the out-of-rect write paths never
# touch a wide surface; R4 copies 2x1 pixels outside the display every frame,
# so refusing them under native-wide rolled back every widescreen pass.
def definition(text, name):
    """The whole body of function `name` (its definition, not a prototype)."""
    m = re.search(r"^static [\w ]+\b" + name + r"\([^;{]*\)\s*\{", text, re.M)
    assert m, "no definition of " + name
    depth, i = 0, m.end() - 1
    while True:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[m.start():i + 1]
        i += 1


assert "g_wide" not in definition(gl, "pass_journal_protect"), (
    "out-of-rect writes are journaled in native-wide as well")
for name in ("gpu_fill", "gpu_copy_rect", "flush_cpu_upload",
             "glb_vram_transfer_in", "glb_vram_write"):
    assert "wide" not in definition(gl, name), (
        "journaled writes must not reach a native-wide surface: " + name)
# One-time pass allocations are not cost samples (render_pass_plan_test):
# every path that makes a pass texture or framebuffer must count it, and the
# cost average must go through the tested sampling rule.
for name in ("pass_make_color_fbo", "pass_gen_reserve"):
    assert "s_pass_allocs++" in definition(gl, name), (
        "pass allocations must be counted: " + name)
assert "render_pass_cost_add(" in body(
    gl, "void gl_renderer_pass_note_cost(uint64_t ticks, int cut) {"), (
    "the pass-cost average must leave allocating passes out")
assert "s_pass_allocs_begin = s_pass_allocs;" in body(
    gl, "static int transaction_begin("), (
    "each pass must mark where its allocations start")
# The average belongs to one presented image size. A plan at another size
# passes an unknown cost (0), for which render_pass_plan_phases plans a single
# measuring pass (render_pass_plan_test): passes run on the emulation thread.
nc = body(gl, "void gl_renderer_pass_note_cost(uint64_t ticks, int cut) {")
assert "s_pass_cost_w != s_interp_w || s_pass_cost_h != s_interp_h" in nc and \
    "memset(&s_pass_cost, 0, sizeof s_pass_cost);" in nc, (
    "a new image size must start the pass-cost average over")
assert re.search(r"in\.pass_cost = \(s_pass_cost_w == s_interp_w && "
                 r"s_pass_cost_h == s_interp_h\)\s*"
                 r"\? render_pass_cost_estimate\(&s_pass_cost\) : 0\.0;",
                 gl[gl.index("uint32_t gl_renderer_pass_plan("):][:4000]), (
    "a plan must not use a cost measured at another image size")
# Only passes that run are measured: an estimate that prices every plan out
# (measured in a transient) must be measured again, or passes stay shed and a
# plugin's crossfade fallback never ends (render_pass_plan_test).
gp = gl[gl.index("uint32_t gl_renderer_pass_plan("):]
gp = gp[:gp.index("\n}\n")]
assert re.search(r"if \(want && s_pass_cost_w == s_interp_w && "
                 r"s_pass_cost_h == s_interp_h &&\s*"
                 r"render_pass_cost_note_plan\(&s_pass_cost\)\) \{", gp) and \
    re.search(r"in\.pass_cost = 0\.0;\s*n = render_pass_plan_phases\(&in, alpha_q16, NULL\);",
              gp), (
    "every plan that wants passes must let a stale estimate be re-measured, "
    "and the re-measuring plan must ask for one pass")
# Leftover-time planning (PSX_MOD_RENDER_PASS_LEFTOVER) is opt-in: the
# idle-time planner stays the default and holds passes to no deadline.
gpp = gl[gl.index("uint32_t gl_renderer_pass_plan("):]
gpp = gpp[:gpp.index("\n}\n")]
assert "if (s_pass_leftover)\n        return pass_plan_leftover(" in gpp, (
    "the leftover planner runs only for a title that opted in")
assert "static int      s_pass_leftover = 0;" in gl, "leftover planning is off by default"
assert "if (!s_pass_leftover) return 1;" in body(gl, "int gl_renderer_pass_may_start(void) {"), (
    "the idle-time planner never skips a pass for time")
assert "if (!s_pass_leftover || !(s_pass_deadline > 0.0)) return 1e18;" in \
    body(gl, "double gl_renderer_pass_time_left(void) {"), (
    "the idle-time planner never stops a pass at a deadline")
assert "gl_renderer_pass_set_leftover(0);" in body(rp, "void render_pass_reset_session(void) {"), (
    "every session starts with the default planner")
# Passes never delay the game's frame (docs/RENDER_PASSES.md): the leftover
# budget is the time left before the frame is first presented, less the work
# the emulation thread still has to do (measured), and the deadline the
# passes are held to is that same point.
lp = gl[gl.index("static uint32_t pass_plan_leftover("):]
lp = lp[:lp.index("\n}\n")]
assert "render_pass_leftover(now, in.frame_start, s_pass_reserve, margin)" in lp \
    and "s_pass_deadline = in.frame_start - s_pass_reserve - margin;" in lp, (
    "leftover plans must budget only the time before the frame's start")
assert "in.probe_min = render_pass_probe_min(" in lp, (
    "an unmeasured pass is tried only with the frame's own cost to spare")
assert "if (n && !pass_gpu_caught_up()) {" in lp, (
    "passes wait for the GPU to finish the game's frame")
assert "if (n && s_since_tight < PASS_TIGHT_HOLDOFF) {" in lp, (
    "no passes while the game itself has no slack")
assert "if (n > pace_cap) {" in lp and "pace_cap = pass_pace_cap(sp);" in lp, (
    "the pace guard limits passes after a frame that slipped")
assert re.search(r"render_pass_leftover_cost_probe_due\(&s_pass_lcost\)\) \{", lp) and \
    "in.budget >= in.probe_min" in lp, (
    "a stale estimate is probed only in leftover time, with one pass")
# Each pass is held to the deadline: not started when it would end after it,
# stopped (not a fault) when it runs into it (render_pass_sandbox_test).
rpt = rp[rp.index("static int render_transaction("):]
assert rpt.index("gl_renderer_pass_may_start()") < rpt.index("gl_renderer_pass_begin("), (
    "the start check must come before the pass opens its VRAM transaction")
assert "psx_cycle_freeze_set_poll(deadline_near, deadline_overrun);" in rpt and \
    "gl_renderer_pass_time_left() < s_end_ticks" in rp, (
    "a running pass must be polled against its deadline, less what its "
    "capture and restore still take")
assert "note_fault" not in definition(rp, "deadline_overrun"), (
    "a pass stopped at its deadline is not a fault")
# A leftover-planned pass copies its rect out in bands with the deadline
# checked between them (slow in some drivers: stencil, first use, large
# scales); stereo and the idle-time planner copy in one blit.
tb = gl[gl.index("static int transaction_begin("):]
tb = tb[:tb.index("\nbacked_up:")]
assert tb.count("pass_backup_blit(") == 2 and "banded = !stereo && s_pass_leftover;" in tb, (
    "leftover-planned backups must be banded copies with deadline checks")
assert "if (begun < 0) {" in rpt, "a copy that ran out of time is a cut, not a refusal"
assert "volatile int ran = 0;" in rpt and "gl_renderer_pass_abandon();" in rpt, (
    "a pass whose guest code never ran has nothing to restore")
# A frame on screen longer than planned (a lagging tick) must hold its newest
# pass image, never fall back to the older capture (render_pass_plan_test).
pgp = definition(gl, "pass_gen_choose")
assert "render_pass_gen_select(g->phase, g->n, p, &lo, &hi, &t)" in pgp, (
    "pass images must be selected through the tested late-flip rule")
assert "render_pass_select(" not in pgp and "0.5 / (double)g->period" not in pgp, (
    "no early expiry of a late frame's images")
# HOLD never crossfades between two pass images, but only for a title that
# asked for changed-only presents (PSX_MOD_FRAME_PRESENT_CHANGED).
assert "if (s_present_changed && s_interp_hold && lo != hi) {" in pgp, (
    "HOLD holds the earlier pass image only under PSX_MOD_FRAME_PRESENT_CHANGED")
# A present that would show the picture already on screen is skipped only
# under PSX_MOD_FRAME_PRESENT_CHANGED, with a periodic refresh.
psi = definition(gl, "interp_present_source_interval")
assert "if (s_present_changed && present_choice_same(&c, &s_last_choice) &&" in psi \
    and "!(force && !presented)" in psi and \
    "int force = !s_present_changed || host_osd_needs_present() ||" in psi, (
    "duplicate presents are skipped only when the title opted in")
cls = body(plan, "int render_pass_mmio_class(")
for dev in ("RENDER_PASS_DROP_SPU", "RENDER_PASS_DROP_CD",
            "RENDER_PASS_DROP_TIMER"):
    assert dev in cls, "a pass must never reach " + dev

dbg = (SRC / "debug_server.c").read_text(encoding="utf-8")
for fn in ("void debug_server_trace_write_check(",
           "void debug_server_trace_mmio_write("):
    assert "g_psx_render_pass_active" in body(dbg, fn), (
        "rolled-back pass writes must stay out of the frame fingerprints: " + fn)

rp = (SRC / "render_pass.c").read_text(encoding="utf-8")
for gate in ("psx_netplay_active()", "psx_rewind_is_open()",
             "psx_selfcheck_resim_active()", "psx_get_in_exception()",
             "dma_gpu_linked_list_active()",
             "psx_presentation_fast_forward()"):
    assert gate in rp, "passes must refuse to run when " + gate
main = (SRC / "main.cpp").read_text(encoding="utf-8")
ff = main[main.index("const int present_every ="):]
ff = ff[:ff.index("turbo_skip = (turbo_skip + 1) % present_every;")]
assert "s_presentation_fast_forward = 1;" in ff, (
    "manual fast-forward must refuse render passes (it presents every 2nd "
    "VBlank, so the plan would otherwise still see presents)")

b = body(rp, "static void checkpoint_restore(CPUState *cpu) {")
assert "nesting_restore(&s_ck.nest);" in b, (
    "a watchdog abort longjmps past the exits of the frames it leaves: the "
    "restore must put the host nesting back (render_pass_abort_test)")

gl = (SRC / "gpu_gl_renderer.c").read_text(encoding="utf-8")
b = body(gl, "static int pass_gen_reserve(int gi, uint32_t need, int w, int h) {")
assert "need > pass_slot_cap(w, h)" in b, (
    "pass image textures must stay inside the slot cap's memory budget")
assert "pass_gen_reserve(gi, g->n + 1u, g->tex_w, g->tex_h)" in gl, (
    "pass image textures are made as slots fill, not all PASS_SLOTS up front")
assert "for (uint32_t i = 0; i < PASS_SLOTS; i++) {\n        if (!s_pgen_tex" not in gl, (
    "no eager allocation of every slot texture")
assert "pass_resources_release();" in body(gl, "void gl_renderer_shutdown(void) {"), (
    "pass textures die with the context: forget them at shutdown")

# Default off means no cost for other titles: the headers folded into the
# overlay codegen hash (runtime/codegen_hash_sources.cmake) must not carry the
# render-pass API, or every title's overlay cache and savestates would be
# invalidated by a feature they never use. The freeze lives in the
# runtime-only psx_cycle_freeze.h.
INC = ROOT / "runtime" / "include"
hash_list = (ROOT / "runtime" / "codegen_hash_sources.cmake").read_text(
    encoding="utf-8")
for header in re.findall(r"runtime/include/([\w.]+)", hash_list):
    text = (INC / header).read_text(encoding="utf-8")
    for name in ("g_psx_render_pass_active", "PsxCycleFreeze",
                 "psx_cycle_freeze", "render_pass"):
        assert name not in text, (
            f"{header} is in the codegen hash and must not mention {name}")
assert "psx_cycle_freeze.h" not in hash_list, (
    "psx_cycle_freeze.h is runtime-only; keep it out of the codegen hash")

print("render-pass sandbox guards passed")
