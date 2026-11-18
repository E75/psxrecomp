/*
 * exit_trace.h — make a deliberate, un-dumped process death name itself.
 *
 * Why this exists
 * ---------------
 * 2026-11-18: an INIT-enabled hybrid build died with process exit code 3,
 * wrote no WER event and left `psx_last_run_report.json` untouched. That
 * combination has exactly two producers on this runtime:
 *
 *   1. `starvation_ring.c`'s watchdog — `TerminateProcess(..., 3)` /
 *      `_exit(3)`, which by design skips atexit, so the run report is never
 *      rewritten; and
 *   2. any `abort()` — the UCRT terminates with code 3 after raising
 *      SIGABRT, and an abort that lands before the crash-trace handlers are
 *      installed skips the report as well.
 *
 * A death with no named producer costs a whole investigation cycle, so every
 * deliberate termination now writes one JSON line to `psx_exit_trace.log`
 * before it ends the process. The line is written unconditionally (it is a
 * few hundred bytes and only ever on the way out); the *diagnostic* extras
 * (native backtrace, breadcrumb history) are opt-in:
 *
 *   PSX_EXIT_TRACE=1   write `psx_exit_trace_stack.txt` (raw return
 *                      addresses, module base, breadcrumbs) and install a
 *                      SIGABRT tracer that names the abort before chaining
 *                      to the previous handler.
 *
 * Nothing here runs on a healthy path except one getenv() at install time.
 */
#ifndef PSXRECOMP_EXIT_TRACE_H
#define PSXRECOMP_EXIT_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when PSX_EXIT_TRACE is set to a non-empty value other than "0". */
int psx_exit_trace_active(void);

/* Breadcrumb with no I/O (last 16 are kept, for the stack file). */
void psx_exit_trace_note(const char *note);

/* Record the termination and end the process with `code`.
 *
 * `origin` is the stable name a reader greps for (e.g.
 * "starvation_watchdog"); `detail` is free-form context and may be NULL;
 * `file`/`line` are the calling site. Never returns: the same hard exit the
 * caller would have used (TerminateProcess on Windows, _exit elsewhere), so
 * atexit handlers and driver teardown are still skipped — that is the point
 * of the callers that use it. */
void psx_exit_trace_terminate(const char *origin, int code,
                              const char *detail,
                              const char *file, int line);

/* Same line, but return instead of exiting. A caller that wants a heavier
 * artifact written in between (the crash report) records first, dumps, then
 * calls psx_exit_trace_hard_exit — so the name is on disk even if the dump
 * wedges. */
void psx_exit_trace_record(const char *origin, int code,
                           const char *detail,
                           const char *file, int line);

/* The exit half of psx_exit_trace_terminate (no second log line). */
void psx_exit_trace_hard_exit(int code);

#define PSX_EXIT_TRACE_TERMINATE(origin, code, detail)                     \
    psx_exit_trace_terminate((origin), (code), (detail), __FILE__, __LINE__)

#define PSX_EXIT_TRACE_RECORD(origin, code, detail)                        \
    psx_exit_trace_record((origin), (code), (detail), __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_EXIT_TRACE_H */
