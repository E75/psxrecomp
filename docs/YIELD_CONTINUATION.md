# C-body yield: the host-continuation contract

Status: **runtime side implemented + proved offline, config-gated, default off.**
The live A/B cleared the resume-PC half and found a depth leak; that is fixed
here and pinned by the harness. Only a real boot can prove the frame-rate / CD
command effect.

Branch: `yield-continuation` (worktree `psxrecomp-yield`), on top of
`3528af93` (which the live `psxrecomp` already carries).

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
`$ra`, which is a *stale guest PC*, and the delivery made it the architectural
EPC.

## 2. What the delivery actually does (the hypothesis, corrected)

Hypothesis in the brief: *"`psx_check_interrupts_at(cpu, pc)` runs the guest
until it reaches `pc`; hand it a host sentinel and it will stop when
`pc == resume_pc`."*

**That is not how the code works.** There is no "run until pc" anywhere in the
interrupt path. `psx_check_interrupts_at()` (`runtime/src/interrupts.c`
2311-2341) is a wrapper:

```c
uint32_t prev = s_compiled_interrupt_resume_pc;
/* (host-call boundary substitution) */
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
anywhere. **The stop condition is not `pc == resume_pc`.**

It is the legacy host sentinel, and it already exists:

1. The ISR saves `COP0.EPC` into the thread's TCB and `ReturnFromException`
   loads it into `$k0` and jumps. With `EPC = 0x80000048` the jump lands on the
   host sentinel, and `psx_unknown_dispatch()` (`runtime/src/traps.c`
   1173-1176) does `cpu->pc = 0; psx_exception_longjmp();` — back to the
   `setjmp` in `psx_check_interrupts` (`interrupts.c` 1902-1928).
2. The epilogue then restores the interrupted code's registers, because a
   `LEGACY_SENTINEL` delivery always does (`interrupts.c` 2100-2135).
3. `psx_check_interrupts` returns; `psx_check_interrupts_at` restores its latch
   and returns; `we_yield_until_vblank` returns into the C body.

So what makes `psx_check_interrupts_at` return is either **"nothing
deliverable"** (the ~12 `PSX_CHECK_INTERRUPTS_RETURN()` gates), a **completed
delivery**, or an escape that does not return here (thread switch via
`g_scheduler_jmpbuf`, savestate load, `RestoreState`).

What consumes `cpu->pc` between the RFE and the return: the same-thread
discriminator (2044-2063), the legacy-sentinel same-thread block (2081-2098),
the irqctx ring `exit_pc` (2106), and the thread-switch block (2204-2296). With
`cpu->pc == 0` after the sentinel longjmp none of them misfires.

**Correction to the hypothesis:** a *new* sentinel (e.g. `0x8E000000`) is
unnecessary and slightly harmful. It would be rejected by the same plausibility
test (so it produces the same legacy path) but it would sit in
`s_compiled_interrupt_resume_pc` / `s_last_interrupt_check_pc` during the
delivery, and those latches feed `savestate_poll()` and the netplay edge gate.
The cheap, correct way to select the legacy stop condition is to hand the
delivery **no guest resume PC at all** (`real_pc == 0`).

## 3. The contract

`PSX_C_BODY_YIELD=1` makes `we_yield_until_vblank()`:

1. withhold every resume latch for the delivery (`psx_check_interrupts_at(cpu, 0)`
   for the compiled latch, `g_dirty_safe_resume_pc` saved/zeroed/restored), so
   the delivery cannot select a stale guest PC from *any* backend;
2. arm `g_psx_host_continuation` so `psx_check_interrupts_at` does not mistake
   the call for a host-call return boundary and substitute the enclosing call's
   continuation (`interrupts.c` 2328-2341). Consumed on read, at the top of the
   function, before anything can `longjmp`;
3. deliver exactly once (one `psx_advance_cycles(to_edge)`, one delivery);
4. restore the state an escape unwound, so **depth out == depth in** (section 4);
5. put `cpu->pc` back to the value the caller had.

| property | mechanism |
|---|---|
| ISR runs once per yield | one delivery per call; `WeYieldStats.deliveries` is the `g_irq_deliver_count` delta |
| cycles credited once, like a real spin | `psx_advance_cycles(interrupts_cycles_to_vblank())` (pre-existing); the ISR's own cycles are charged by the code it runs |
| `$ra` / `$sp` / `$v0` unchanged on return | the LEGACY epilogue restores all GPRs + HI/LO (`interrupts.c` 2131-2135) |
| `cpu->pc` unchanged on return | explicit restore (`stats.pc_restored`) |
| `g_psx_guest_call_depth` unchanged | explicit restore (`stats.call_depth_restored`) |
| snapshot-safety latch unchanged | explicit restore (`stats.snapshot_depth_restored`) |
| no nesting | `s_yield_depth` + `psx_get_in_exception()` |
| stock unchanged | flag off by default; with no yield call site the runtime is byte-identical |

## 4. The depth leak the live boot found (fixed)

Live boot, flag on, same window as the A/B:

| flag | result | trace |
|---|---|---|
| 0 | wedge at 33.6 s | 4 enter, 4 return, **2 pc mismatch** (`enter pc=0x0` -> `return pc=0x80019F94`) |
| 1 | wedge at 33.6 s | 4 enter, 4 return, **0 pc mismatch** — the resume pc is restored |

but `g_psx_guest_call_depth` grew by one across every yield:
`#2 enter depth=2 -> return depth=3`, `#3 4 -> 5`, `#4 7 -> 8`, then the log
stopped with the debug server dead.

**Site.** Nothing on the delivery path increments the depth. The increment is
`psx_guest_call_enter()` (`mod_guest_services.c` 101-105) called from
`psx_mod_call_guest_ex()` (180), and the matching decrement is
`psx_guest_call_leave(pushed)` (221) — in the *epilogue*, after
`psx_dispatch_call()` (206). The ISR runs guest code that makes a host call
(the kernel's own call chain, `we_gcall`); the handler's
`ReturnFromException`/sentinel exit `longjmp`s back to
`psx_check_interrupts`'s `setjmp` (`interrupts.c` 1903), which unwinds every
host frame between — including that `psx_mod_call_guest_ex` — so line 221 never
runs and the counter stays incremented. One per delivery, exactly as measured.

This is the same class `psx_check_interrupts` already handles for its own
counter, in the same place: *"a ReturnFromException longjmp skips the handler
frames' `--g_psx_dispatch_depth` decrements — so we cannot leave the counter at
0. Save the interrupted code's nesting here and RESTORE it after the handler"*
(`interrupts.c` 1885-1891). The call depth had no such guard.

**Sibling latch, same cause, invisible.** `psx_mod_call_guest_ex` also raises
the snapshot-safety latch (`psx_snapshot_host_call_begin()` at 205, `end()` at
220). The `longjmp` skips `end()` too, so `s_snapshot_host_call_depth` grows by
one per yield and `psx_irq_resume_context_snapshot_safe_at()` (`interrupts.c`
663-695) returns 0 forever — savestates would silently stop being taken. Two
accessors were added (`psx_snapshot_host_call_depth()` /
`..._set()`) so the yield can save and restore it.

**Fix.** In the flag-on branch only:

```c
const int saved_call_depth = g_psx_guest_call_depth;
const unsigned saved_snap_depth = psx_snapshot_host_call_depth();
... delivery ...
if (g_psx_guest_call_depth != saved_call_depth) {
    g_psx_guest_call_depth = saved_call_depth;      /* stats.call_depth_restored++ */
}
if (psx_snapshot_host_call_depth() != saved_snap_depth) {
    psx_snapshot_host_call_depth_set(saved_snap_depth);
}
```

Why the abandoned frame's *data* is safe to leave behind: `psx_guest_call_enter`
always writes `s_call_resume[depth]` / `s_sentinel[depth]` **before**
incrementing, and every reader loops `i < depth`. So the slots the abandoned
frame wrote (`index == restored depth`) are never read and are overwritten by
the next `enter` at that index. Only the counter has to be put back.

What this does **not** do:

- It does not re-zero an inherited leak. The restore is to the value at yield
  entry; if some other path already leaked depth, that stays (and must, because
  a non-zero entry depth can be legitimate — the C body may itself be inside a
  host call).
- It cannot help when the yielded ISR switches threads: that escape leaves via
  `g_scheduler_jmpbuf`, never returns to the yield, and the C frame is gone. The
  trace shows `enter` without `return`.
- It does not touch the block-leader delivery path. The same leak exists there
  today (it is what made the live entry depth 2/4/7 rather than 0); fixing that
  would change stock behaviour and is out of scope. `docs/BOOT_BLOCKERS.md`
  material, not this change.

## 5. Diff

`runtime/src/psx_guest_yield.c` — the contract, with the depth save/restore:

```c
const uint32_t saved_dirty_pc = g_dirty_safe_resume_pc;
const int saved_call_depth = g_psx_guest_call_depth;
const unsigned saved_snap_depth = psx_snapshot_host_call_depth();
g_dirty_safe_resume_pc = 0;
g_psx_host_continuation = 1;
psx_check_interrupts_at(cpu, 0u);
g_psx_host_continuation = 0;
g_dirty_safe_resume_pc = saved_dirty_pc;
if (g_psx_guest_call_depth != saved_call_depth) { restore; count }
if (psx_snapshot_host_call_depth() != saved_snap_depth) { restore; count }
if (cpu->pc != entry_pc) { cpu->pc = entry_pc; count }
```

`runtime/src/interrupts.c` — 2 things:

```c
/* consume-on-read guard, so a C-body yield is not read as a host-call boundary */
extern int g_psx_host_continuation;
const int host_continuation = g_psx_host_continuation;
g_psx_host_continuation = 0;
if (!host_continuation && g_psx_guest_call_depth != 0 && !psx_guest_pc_plausible(resume_pc)) { ... }

/* the sibling latch becomes saveable */
unsigned psx_snapshot_host_call_depth(void) { return s_snapshot_host_call_depth; }
void psx_snapshot_host_call_depth_set(unsigned depth) { s_snapshot_host_call_depth = depth; }
```

`runtime/include/psx_guest_yield.h` — contract text, flag setters, `WeYieldStats`
(now with `call_depth_restored` / `snapshot_depth_restored`).
`runtime/include/interrupts.h` — the two accessors.
`runtime/tests/test_yield_continuation.c` — depth assertions on every case +
the 8-yield loop (186 checks).
`runtime/CMakeLists.txt` — `yield_continuation_test`.

No change to generated code, the recompiler, or any guest-visible state.

## 6. Config flag

- `PSX_C_BODY_YIELD=1` (any value not starting with `0`) turns the contract on;
  unset or `0` keeps the historical behaviour. Latched on first query, like
  `PSX_DEFER_SWITCH` / `PSX_MIXED_OWNER`.
- `we_yield_set_c_body_yield(1)` overrides it, for a `[runtime] c_body_yield`
  key wired by the host (`main.cpp` owns the `game.toml` `[runtime]` section).
- The flag does nothing on its own: the yield still needs a call site
  (`config/yield_sites.txt` in the live lane).

## 7. Offline proof

```
clang -std=gnu89 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
      -Wno-gcc-compat -Iruntime/include runtime/tests/test_yield_continuation.c \
      -o build/test_yield_continuation.exe
./build/test_yield_continuation.exe
```

Output (`stderr` trace interleaved; `call_depth` is `g_psx_guest_call_depth`,
the same field as the live log):

```
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
T12 8 consecutive yields: depth flat at 4, GPRs intact each time
186 checks, 0 failure(s)
we_yield: enter #1 pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 call_depth=0
we_yield: return #1 pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 call_depth=0
we_yield: enter #2 pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 call_depth=0
we_yield: return #2 pc=0x8004C288 ra=0x8004C288 sp=0x801FFEE0 call_depth=1
we_yield: enter #3 pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 call_depth=0
we_yield: return #3 pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 call_depth=0
we_yield: enter #4 pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 call_depth=0
we_yield: return #4 pc=0x00000000 ra=0x8004C288 sp=0x801FFEE0 call_depth=0
```

`#2` is the flag-off run: `enter call_depth=0 -> return call_depth=1` — the
leak, on the historical path. `#1`/`#3`/`#4` are the flag-on runs: flat.

What each test pins down:

- **T1** — one delivery entry, `candidate == 0`, `LEGACY_SENTINEL`,
  `EPC == PSX_EXC_SENTINEL_PC`, ISR **once**, its RFE stops on the sentinel,
  `pc == 0`, `$ra`/`$sp` restored, `$v0 = 1` undone, watched word cleared,
  cycle delta **exactly** one frame (564480), and `call_depth 0 -> 0` with both
  restores counted.
- **T2** — flag off: `0x8004C288` becomes EPC and the caller gets
  `cpu->pc == 0x8004C288`; and the depth/snapshot latches are left at 1, i.e.
  the historical path is pinned byte-for-byte (it must not silently become the
  fixed path; that would destroy the A/B baseline).
- **T3** — two consecutive yields, flat depth after each.
- **T4/T5/T6** — nested yield, yield from inside the ISR, forced
  `s_yield_depth = 1`: all return 0, deliver nothing, and leave the depth
  untouched.
- **T7** — nothing unmasked: cycles credited once, no ISR, depth untouched.
- **T8** — frozen / in-exception: refuse **without** advancing time, depth
  untouched, no restore counted.
- **T9/T10** — `we_wait_guest_word()` success and timeout, depth flat across the
  deliveries it made.
- **T12** — 8 consecutive yields with the C body at depth 4 and **two** host
  calls abandoned per delivery: after every iteration the depth is 4, the
  snapshot latch 0, `cpu->pc` 0, `$ra`/`$sp` intact, `$v0` cleared, the ISR ran
  this iteration, and the delivery was still handed no resume PC (the guard
  holds at depth 4, where `interrupts.c` 2328 would otherwise substitute
  `0x80100000`). Without the restore this would be `4 + 8*2 = 20`.
- **T11** — `PSX_EXC_SENTINEL_PC != 0x8F000000`, and the delivery would never
  accept `0x8F000000` as an EPC either.

`clang -std=gnu89 -fsyntax-only -Iruntime/include runtime/src/psx_guest_yield.c`
is clean. `runtime/src/interrupts.c` needs `-std=gnu99` (the project standard)
because of a pre-existing gnu89 break at HEAD in `interrupts_cosim_hash()` (two
`for (int i …)` in one block), verified against `git show HEAD:runtime/src/interrupts.c`.

## 8. Failure modes ruled out

- **"`cpu->pc` is consumed between the RFE and the return."** Only the four
  consumers listed in section 2; none misfires at `pc == 0`.
- **"The host sentinel gets persisted into a TCB."** The ISR does store
  `0x80000048` architecturally — the same thing every trampoline-boundary
  delivery does — and `psx_assert_no_sentinel_pc()` is fail-closed on it for a
  *saved* context. A yielded ISR that switches threads is therefore a named
  crash, not a silent smear.
- **"The host-call substitution hijacks the delivery."** It would, so the yield
  arms `g_psx_host_continuation`; T12 at depth 4 proves the guard holds.
- **"The stale latches pick a guest PC anyway."** `g_dirty_safe_resume_pc` takes
  priority in the EPC selection, so it is withheld for the delivery; a `longjmp`
  out leaves it 0, the state the exception epilogue already restores to.
- **"Two deliveries / double cycle credit."** One advance, one delivery;
  T1/T7 assert the exact deltas.
- **"Nested yields recurse."** `s_yield_depth` + `psx_get_in_exception()`; T4-T6.
- **"The depth leak is just the yield's own counter."** It is not: the yield
  never increments it. Pinned by T2 (flag off leaks) and T12 (two abandoned
  calls per delivery, still flat).
- **"Restoring the counter corrupts the call arrays."** Every index `< depth` is
  rewritten by the next `enter`; readers never look above `depth` (section 4).
- **"The post-delivery cooldown starves the next yield."** Claimed = 128 cycles,
  unclaimed = 8192, against a 564480-cycle frame.
- **"Stock changes."** Flag off, or no call site: the new code is unreachable or
  byte-identical; the only additions on the shared path are counters and a
  diagnostic line.

## 9. What only a real boot can prove

- That the wait in the live path is *this* wait. 9.9's next step says the Pause
  storm comes from the **A0h 03h read path**, not `cd_unk_80042d28`, so the
  yield must sit on the loop that actually spins.
- That WE2002's VBlank ISR does not switch threads (else the C frame is
  abandoned; trace shows `enter` without `return`).
- That the VBlank/RCNT phase the yield produces is the one this driver's spin
  would have produced — i.e. that the CD command mix becomes a normal
  Setloc/ReadN cycle.
- The frame rate.

## 10. The A/B the live lane re-runs

Same build, disc, frame window and probe; only the flag changes. `PSX_C_BODY_YIELD=0`
first (baseline), then `=1`. Keep the `config/yield_sites.txt` row identical
between the runs (regenerate + rebuild once; the flag alone changes nothing
without a call site).

**Gate 1 — flat depth (new pass condition).** From the yield trace, for every
`#N`: `enter … call_depth=D` must be followed by `return … call_depth=D` with
the same `D`. No growth, no `enter` without a `return`.
- Flat depth + wedge → the delivery is running the wrong handler, or the word
  being waited on is not the one that handler sets. Stop and let the boot lane's
  instrumentation decide (log what the handler writes vs what the C body reads);
  do not tune the yield further.
- Growing depth → the restore did not take effect (stale binary), or a *second*
  leak path exists; the trace names the depth, the `psx_guest_call_stats`
  counters name the calls.
- `enter` without `return` → the yielded ISR switched threads; that is the
  documented non-resumable case.

**Gate 2 — the 9.9 acceptance numbers**, only after gate 1 passes:
- `hyb_cd_cmp.py` over frames 120-144: `0x09` retries collapse from 128 towards
  stock's 15, with Setloc/Setmode/ReadN/Pause back in the mix.
- fps moves from 1.3 towards 34.
- `irq_exc_stats` deliveries grow one per yield; `WeYieldStats.deliveries`
  matches the number of yields; `call_depth_restored` equals the number of
  deliveries that leaked.
- No `psx_guest_call: bad ISR target`, no `exception sentinel … persisted in
  TCB`, no `DISPATCH FATAL`.

If gate 1 is flat and gate 2 does not move, then the delivery is correct and the
boot's problem is not the missing interrupt boundary at that site — the CD
command mix, not the yield, is then the thing to explain.

## 11. Files

| file | change |
|---|---|
| `runtime/src/psx_guest_yield.c` | contract: flag, stats, trace, latch withholding, call/snapshot depth restore, `cpu->pc` restore |
| `runtime/include/psx_guest_yield.h` | contract text, flag setters, `WeYieldStats` |
| `runtime/src/interrupts.c` | host-continuation guard in `psx_check_interrupts_at`; snapshot-latch accessors |
| `runtime/include/interrupts.h` | the snapshot-latch accessors |
| `runtime/tests/test_yield_continuation.c` | the offline harness (186 checks, incl. the 8-yield loop) |
| `runtime/CMakeLists.txt` | `yield_continuation_test` (ctest) |
| `docs/YIELD_CONTINUATION.md` | this document |
