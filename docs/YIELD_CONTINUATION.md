# C-body yield: the host-continuation contract

Status: **runtime side implemented + proved offline, config-gated, default off.**
Only a real boot can prove the frame-rate/CD-command effect. This document is the
design, the evidence, the offline proof, and the exact A/B the live lane runs.

Branch: `yield-continuation` (worktree `psxrecomp-yield`), based on
`psxrecomp@354e9de0` ("recompiler: hand every registered hybrid PC to the
decompiled C").

---

## 1. The measured problem

A decompiled C body that waits on a word its ISR sets cannot progress: as host C
it has no instruction boundary, so the interrupt never runs and the wait burns
its timeout. `WE2002Recomp/docs/BOOT_BLOCKERS.md` 9.9 measures the consequence:

| at ~frame 120-144 | stock | C path |
|---|---|---|
| wall clock | 144 frames in 4.2 s = **34.0 fps** | 121 frames in 92 s = **1.3 fps** |
| command mix | `0x09`x15, `0x06`x14, `0x0E`x14, `0x02`x14, ... | **`0x09`x128 (Pause only)** |

`we_yield_until_vblank()` was written for exactly that wait, and enabling it
wedged the boot. The yield's own diagnostic at the wedge:

```
pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 depth=0
```

`depth=0` is `g_psx_guest_call_depth`: the body is dispatcher-entered, **not**
inside a `psx_mod_call_guest()`. So the yield handed the delivery the body's
`$ra`, which is a *stale guest PC* (the caller frame's return address, from
before the C body was entered), and the delivery made it the architectural EPC.

## 2. What the delivery actually does (the hypothesis, corrected)

Hypothesis in the brief: *"`psx_check_interrupts_at(cpu, pc)` runs the guest until
it reaches `pc`; hand it a host sentinel and it will stop when `pc == resume_pc`."*

**That is not how the code works.** There is no "run until pc" anywhere in the
interrupt path. `psx_check_interrupts_at()` (`runtime/src/interrupts.c`
2311-2341) is a wrapper:

```c
uint32_t prev = s_compiled_interrupt_resume_pc;
/* (host-call boundary substitution, see below) */
s_compiled_interrupt_resume_pc = resume_pc;
psx_check_interrupts(cpu);
s_compiled_interrupt_resume_pc = prev;
```

and `resume_pc` is used for exactly one thing: as a **candidate EPC**. The
delivery's EPC selection (`interrupts.c` 1678-1724):

```c
uint32_t real_pc = g_dirty_safe_resume_pc ? g_dirty_safe_resume_pc
                                          : s_compiled_interrupt_resume_pc;
...
if (real_pc != 0u && (real_pc & 3u) == 0u &&
    (resume_in_ram || resume_in_rom)) {
    cpu->cop0[COP0_EPC]  = real_pc;          /* the guest resumes THERE */
    g_exception_real_epc = real_pc;
    g_exc_escape_reason  = PSX_EXC_ESCAPE_NONE;
} else {
    cpu->cop0[COP0_EPC]  = PSX_EXC_SENTINEL_PC;   /* 0x80000048 */
    g_exception_real_epc = PSX_EXC_SENTINEL_PC;
    g_exc_escape_reason  = PSX_EXC_ESCAPE_LEGACY_SENTINEL;
}
```

`grep resume_pc runtime/src/interrupts.c` shows no comparison against `cpu->pc`
anywhere: the value is only stored, logged (`take_pc`), and tested for
plausibility. **The stop condition is not `pc == resume_pc`.**

It is the legacy host sentinel, and it already exists:

1. The ISR saves `COP0.EPC` into the thread's TCB and `ReturnFromException` loads
   it into `$k0` and jumps. With `EPC = 0x80000048` the jump lands on the host
   sentinel, and `psx_unknown_dispatch()` (`runtime/src/traps.c` 1173-1176) does
   `cpu->pc = 0; psx_exception_longjmp();` — back to the `setjmp` in
   `psx_check_interrupts` (`interrupts.c` 1902-1928).
2. The epilogue then restores the interrupted code's registers, because a
   `LEGACY_SENTINEL` delivery always does (`interrupts.c` 2100-2135):
   `do_restore = (reason == PSX_EXC_ESCAPE_LEGACY_SENTINEL || same_thread_resume)`.
3. `psx_check_interrupts` returns; `psx_check_interrupts_at` restores its latch
   and returns; `we_yield_until_vblank` returns into the C body.

So the real answer to *"what makes `psx_check_interrupts_at` return"* is:

- **nothing pending / not deliverable** — the ~12 `PSX_CHECK_INTERRUPTS_RETURN()`
  gates (frozen time, no `i_stat & i_mask`, `IEc`/`IM2` clear, cooldown, nested
  exception gates, netplay edge holds);
- **a delivery completed** — handler returned, or the handler RFE'd to the host
  sentinel and the sentinel detector longjmp'd back to the `setjmp` loop;
- **an escape that does not return here** — a thread switch (`g_scheduler_jmpbuf`,
  `interrupts.c` 2265), a savestate load, `RestoreState`.

And what else consumes `cpu->pc` between the RFE and the return: the same-thread
discriminator (`interrupts.c` 2044-2063, `same_guest_pc(cpu->pc, real_epc)`), the
legacy-sentinel same-thread block (2081-2098), the irqctx ring `exit_pc` (2106),
and the thread-switch block (2204-2296). With `cpu->pc == 0` after the sentinel
longjmp: the discriminator is false, `do_restore` is true via the LEGACY reason,
the sentinel same-thread block is skipped (it requires
`reason == RFE_RETURN`), and the thread-switch block only fires if the ISR moved
`PCB[0]` (a real switch). Nothing else reads `cpu->pc` before the return.

**Correction to the hypothesis:** a *new* sentinel (e.g. `0x8E000000`) is
unnecessary and slightly harmful. It would be rejected by the same plausibility
test (so it produces the same legacy path) but it would sit in
`s_compiled_interrupt_resume_pc` / `s_last_interrupt_check_pc` during the
delivery, and those latches feed `savestate_poll()` and the netplay edge gate. The
cheap, correct way to select the legacy stop condition is to hand the delivery
**no guest resume PC at all** (`real_pc == 0`), which is exactly what the code
already treats as "no mid-function resume point".

## 3. The contract (what the C body gets)

`PSX_C_BODY_YIELD=1` changes `we_yield_until_vblank()` to:

1. withhold every resume latch for the delivery
   (`s_compiled_interrupt_resume_pc = 0` via `psx_check_interrupts_at(cpu, 0)`,
   and `g_dirty_safe_resume_pc` saved/zeroed/restored), so the delivery cannot
   select a stale guest PC from *any* backend;
2. arm `g_psx_host_continuation` so `psx_check_interrupts_at` does not mistake the
   call for a host-call return boundary and substitute the enclosing call's
   continuation (`interrupts.c` 2320-2337). Consumed on read, at the top of the
   function, before anything can `longjmp`;
3. deliver exactly once (the same `psx_advance_cycles(to_edge)` as before, then one
   `psx_check_interrupts_at`);
4. put `cpu->pc` back to the value the caller had, so a dispatcher-entered C body
   (pc = 0) never leaves a host token or a guest PC behind.

Guarantees, and where they come from:

| property | mechanism |
|---|---|
| ISR runs once per yield | one delivery per call; `WeYieldStats.deliveries` is the `g_irq_deliver_count` delta |
| cycles credited once, like a real spin | `psx_advance_cycles(interrupts_cycles_to_vblank())`, unchanged from the pre-existing code; the ISR's own cycles are charged by the code it runs |
| `$ra` / `$sp` / `$v0` unchanged on return | the LEGACY epilogue restores all GPRs + HI/LO (`interrupts.c` 2131-2135) |
| `cpu->pc` unchanged on return | explicit restore in the yield (`pc_restored` counter) |
| no nesting | `s_yield_depth` (pre-existing) + `psx_get_in_exception()` |
| stock unchanged | the flag is off by default; with no yield call site the runtime is byte-identical |

## 4. Diff

`runtime/src/psx_guest_yield.c` — the contract (flag, stats, trace, latch
withholding, `cpu->pc` restore) and the flag-off path kept byte-for-byte:

```c
    const uint64_t deliv_before = g_irq_deliver_count;
    if (we_yield_c_body_yield_enabled()) {
        const uint32_t saved_dirty_pc = g_dirty_safe_resume_pc;
        g_dirty_safe_resume_pc = 0;
        g_psx_host_continuation = 1;
        psx_check_interrupts_at(cpu, 0u);
        g_psx_host_continuation = 0;
        g_dirty_safe_resume_pc = saved_dirty_pc;
        if (cpu->pc != entry_pc) { cpu->pc = entry_pc; s_stats.pc_restored++; }
    } else {
        const uint32_t resume = cpu->gpr[31];          /* historical path */
        if (guest_resume_pc(resume)) psx_check_interrupts_at(cpu, resume);
        else psx_check_interrupts(cpu);
    }
    s_stats.deliveries += g_irq_deliver_count - deliv_before;
```

`runtime/src/interrupts.c` — one guard, because a C-body yield is not a host-call
boundary:

```c
    extern int g_psx_host_continuation;
    const int host_continuation = g_psx_host_continuation;
    g_psx_host_continuation = 0;
    if (!host_continuation && g_psx_guest_call_depth != 0 &&
        !psx_guest_pc_plausible(resume_pc)) {
        psx_guest_call_note_boundary();
        const uint32_t host = psx_guest_call_resume_pc();
        if (host != 0u) resume_pc = host;
    }
```

`runtime/include/psx_guest_yield.h` — the contract text, the flag
(`we_yield_c_body_yield_enabled` / `we_yield_set_c_body_yield`) and
`WeYieldStats`.

`runtime/tests/test_yield_continuation.c` + one `add_test` block in
`runtime/CMakeLists.txt` — the offline proof (section 6).

No change to the generated code, the recompiler, or any guest-visible state.

## 5. Config flag

- `PSX_C_BODY_YIELD=1` (any value not starting with `0`) turns the contract on;
  unset or `0` keeps the historical behaviour. Latched on first query, like
  `PSX_DEFER_SWITCH` / `PSX_MIXED_OWNER` in the same runtime.
- `we_yield_set_c_body_yield(1)` overrides it, for a `[runtime] c_body_yield`
  key wired by the host (`main.cpp` owns the `game.toml` `[runtime]` section).
- The flag does nothing on its own: the yield still needs a call site. In the
  live lane that is the `config/yield_sites.txt` row that was disabled after it
  wedged.

## 6. Offline proof

```
clang -std=gnu89 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
      -Wno-gcc-compat -Iruntime/include runtime/tests/test_yield_continuation.c \
      -o build/test_yield_continuation.exe
./build/test_yield_continuation.exe
```

Output (`stderr` trace interleaved; `call_depth` is `g_psx_guest_call_depth`,
the same field as the wedge log):

```
we_yield: enter #1 pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 call_depth=0
test_yield_continuation: C-body yield host-continuation contract
T0  default is off (env unset, no override)
T1  C-body yield: sentinel delivery runs the ISR once and returns
T2  flag off: the stale $ra becomes EPC and the C body loses control
T3  a second yield after the return still delivers and returns
T4  a nested yield (re-entered from the delivery) returns 0
T5  a yield from inside the ISR returns 0 as well
T6  the depth counter is clear again after every return
T7  nothing unmasked: cycles credited once, no ISR, pc untouched
T8  frozen guest time and in-exception refuse without advancing
T9  we_wait_guest_word: the ISR clears the word, the helper returns 1
T10 we_wait_guest_word: an unset word times out after max_vblanks
T11 the host stop token is not the host-call sentinel
71 checks, 0 failure(s)
we_yield: return #1 pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 call_depth=0
we_yield: enter #2 ... 
we_yield: return #2 pc=0x8004C288 ra=0x8004C288 sp=0x801FFEE0 call_depth=0
```

`#1` is the flag-on yield: it returns with `pc` still 0. `#2` is the flag-off
yield in T2: it returns with `pc=0x8004C288` — the stale `$ra` — which is the
wedge, reproduced offline.

What each test pins down:

- **T1** — one delivery entry, `candidate == 0` (no guest resume PC), the
  delivery is `LEGACY_SENTINEL`, `EPC == PSX_EXC_SENTINEL_PC`, the ISR runs
  **once**, its RFE leaves `cpu->pc == PSX_EXC_SENTINEL_PC` (the stop token),
  the C body then sees `pc == 0`, `$ra`/`$sp` restored, the ISR's `$v0 = 1`
  undone, the watched word cleared, and the cycle delta **exactly**
  `interrupts_cycles_to_vblank()` (564480, one frame), not two.
- **T2** — with the flag off the same machine hands `0x8004C288` to the delivery,
  it becomes EPC, and the caller gets `cpu->pc == 0x8004C288`: guest code, not
  the C body. That is the measured wedge, now a regression test.
- **T3** — the depth counter is clear after a return, so the next yield delivers
  again (the `0x09` retry storm needs many yields in a row).
- **T4/T5/T6** — a yield re-entered from the delivery, a yield from inside the
  ISR, and a forced `s_yield_depth = 1` all return 0 and deliver nothing.
- **T7** — with nothing unmasked, cycles are still credited once, no ISR runs,
  `cpu->pc`/`$ra` untouched.
- **T8** — frozen guest time and in-exception refuse **without** advancing time.
- **T9/T10** — `we_wait_guest_word()` returns 1 when the ISR clears the word and
  0 after `max_vblanks`.
- **T11** — `PSX_EXC_SENTINEL_PC != 0x8F000000` (host-call sentinel), and the
  delivery would never accept `0x8F000000` as an EPC either.

`clang -std=gnu89 -fsyntax-only -Iruntime/include runtime/src/psx_guest_yield.c`
is clean (only the repo-wide `getenv` deprecation and C89 loop-declaration style
warnings). `runtime/src/interrupts.c` **does not** pass `-std=gnu89` at HEAD
either — `interrupts_cosim_hash()` declares `for (int i ...)` twice in one block
(line 765/767), which clang rejects under gnu89. That is pre-existing (verified
against `git show HEAD:runtime/src/interrupts.c`), so the file is checked with
`-std=gnu99`, which is the project's standard (`CMAKE_C_STANDARD 99`).

## 7. Failure modes ruled out

- **"`cpu->pc` is consumed between the RFE and the return."** No: the four
  consumers are the same-thread discriminator, the legacy same-thread block, the
  ring record, and the thread-switch block. With `cpu->pc == 0` none of them
  misfires (the first two require an RFE reason or a PC match; the switch block
  only when `PCB[0]` moved).
- **"A host sentinel would be persisted into a TCB."** The ISR does store
  `0x80000048` in the TCB EPC slot architecturally — that is the same thing every
  trampoline-boundary delivery already does, and `traps.c`
  `psx_assert_no_sentinel_pc()` is fail-closed on it for a *saved* (cross-thread)
  context. A yielded ISR that switches threads is therefore a hard, named crash,
  not a silent smear. (See the limit in section 8.)
- **"The host-call resume substitution will hijack the delivery."** It would
  (`interrupts.c` 2320-2337 substitutes `psx_guest_call_resume_pc()` for a
  non-plausible resume PC when `g_psx_guest_call_depth != 0`), so the yield arms
  `g_psx_host_continuation` and the wrapper skips the substitution. Measured
  `depth == 0` today, so this is a guard against the nested case, not the fix.
- **"The stale latches pick a guest PC anyway."** `g_dirty_safe_resume_pc` takes
  priority over the compiled latch in the EPC selection, so it is saved/zeroed
  around the delivery and restored after. A `longjmp` out of the delivery leaves
  it 0, which is the same state the exception epilogue already restores to.
- **"Two deliveries per yield / double cycle credit."** One `psx_advance_cycles`
  and one `psx_check_interrupts_at`; T1/T7 assert the exact deltas.
- **"Nested yields recurse."** `s_yield_depth` plus `psx_get_in_exception()`; T4-T6.
- **"Stock changes."** The flag is off by default and the off path is the
  original code; no yield call site means the functions are never entered.
- **"The post-delivery cooldown starves the next yield."** Claimed = 128 cycles,
  unclaimed = 8192, against a 564480-cycle frame — a full frame always clears it.

## 8. What only a real boot can prove

- That the wait in the live path is *this* wait. 9.9's next step says the Pause
  storm comes from the **A0h 03h read path**, not `cd_unk_80042d28`, so the yield
  must be placed at the loop that actually spins. The runtime fix is a
  precondition, not a placement.
- That the WE2002 VBlank ISR does not switch threads. If it does, the yielded
  delivery escapes to the scheduler and the C frame is abandoned (the trace prints
  `enter` without `return`). This is the one failure mode the harness cannot
  model, because it needs the real TCB/scheduler.
- That the guest-visible VBlank/RCNT phase the yield produces is the same one the
  spin would have produced for *this* title's driver — i.e. that the CD command
  mix becomes a normal Setloc/ReadN cycle rather than shifting somewhere else.
- The frame rate. 1.3 -> 34 fps is the hypothesis; the A/B measures it.

## 9. The A/B the live lane runs

Same build, same disc, same frame window, same probe — only the flag changes.

```
# off (baseline, today's behaviour)
set PSX_C_BODY_YIELD=0
<run>  ; tools/we/hyb_cd_cmp.py --frames 120-144

# on
set PSX_C_BODY_YIELD=1
<run>  ; tools/we/hyb_cd_cmp.py --frames 120-144
```

Also re-enable the `config/yield_sites.txt` row (the one with the RE-TRIED note)
in `WE2002DeCompiled`, regenerate, rebuild — the flag alone changes nothing
without a call site. Keep the row identical between the two runs.

Record, per run:

1. **the yield log line** (stderr, first 4 yields then every 1024th):
   `we_yield: enter #N pc=... ra=... sp=... call_depth=...` followed by
   `we_yield: return #N ...` with the same `#N`.
   - `enter` **without** `return` = the delivery did not come back to the C body
     (thread switch, scheduler escape) — that is the wedge, and it is now named.
   - `call_depth` > 0 means the yield fired inside a host call: expected to work
     through the guard, but note it.
   - `pc` on `return` must equal `pc` on `enter` (0 for a dispatcher-entered body).
2. **the CD command mix** (`hyb_cd_cmp.py`): `0x09`x128 must collapse towards
   stock's `0x09`x15 + Setloc/Setmode/ReadN/Pause cycle. A `0x09` storm *with*
   `return` lines means the yield works but the wrong loop is instrumented.
3. **the frame rate** for the same window: 1.3 fps must approach 34 fps. Expect
   the 26x gap to close in proportion to the removed timeouts.
4. **delivery count**: `irq_exc_stats` deliveries must grow with the yields
   (one per yield), and `WeYieldStats.deliveries` (or the `g_irq_deliver_count`
   delta) must match the number of yields.
5. **no new class**: no `psx_guest_call: bad ISR target`, no
   `exception sentinel ... persisted in TCB`, no `DISPATCH FATAL`.

Pass = the command mix becomes a normal Setloc/ReadN cycle, every `enter` has a
`return`, fps moves towards stock, and none of the named failures appear. Anything
else is a real finding: keep the log, it names the field.

## 10. Files

| file | change |
|---|---|
| `runtime/src/psx_guest_yield.c` | the contract: flag, stats, trace, latch withholding, `cpu->pc` restore |
| `runtime/include/psx_guest_yield.h` | contract text, flag setters, `WeYieldStats` |
| `runtime/src/interrupts.c` | 8 lines: `g_psx_host_continuation` consume-on-read guard in `psx_check_interrupts_at` |
| `runtime/tests/test_yield_continuation.c` | the offline harness (71 checks) |
| `runtime/CMakeLists.txt` | `yield_continuation_test` (ctest) |
| `docs/YIELD_CONTINUATION.md` | this document |
