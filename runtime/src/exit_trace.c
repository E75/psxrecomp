/*
 * exit_trace.c — one named line for every deliberate, un-dumped death.
 * See exit_trace.h for why this exists.
 *
 * Grep targets:
 *   psx_exit_trace.log        one JSON line per termination (always written)
 *   psx_exit_trace_stack.txt  native return addresses + breadcrumbs
 *                             (only with PSX_EXIT_TRACE=1)
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "exit_trace.h"
#include "psx_cycles.h"
#include "psx_memory.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

/* Same declarations starvation_ring.c uses: the guest-state globals are
 * plain words in the runtime, and pulling cpu_state.h in here would turn
 * g_debug_last_store_pc into its accessor macro. */
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;
extern int psx_get_in_exception(void);

#define EXIT_TRACE_LOG   "psx_exit_trace.log"
#define EXIT_TRACE_STACK "psx_exit_trace_stack.txt"

/* Runtime memory API (declared by each consumer; psx_memory.h does not export
 * the pointer, only the geometry helpers). */
extern unsigned char *memory_get_ram_ptr(void);
extern uint32_t memory_get_ram_bytes(void);
extern uint32_t psx_read_word(uint32_t a);

#define NOTE_CAP 16

static const char *s_notes[NOTE_CAP];
static unsigned    s_note_count;

int psx_exit_trace_active(void) {
    static int active = -1;
    if (active < 0) {
        const char *e = getenv("PSX_EXIT_TRACE");
        active = (e && *e && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
    }
    return active;
}

void psx_exit_trace_note(const char *note) {
    s_notes[s_note_count % NOTE_CAP] = note;
    s_note_count++;
}

static void write_notes(FILE *f) {
    unsigned n = s_note_count < NOTE_CAP ? s_note_count : NOTE_CAP;
    unsigned first = s_note_count - n;
    for (unsigned i = 0; i < n; i++) {
        const char *s = s_notes[(first + i) % NOTE_CAP];
        if (s) fprintf(f, "note[%u] %s\n", i, s);
    }
}

static void write_termination_line(const char *origin, int code,
                                   const char *detail,
                                   const char *file, int line) {
    FILE *f = fopen(EXIT_TRACE_LOG, "ab");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char ts[32];
    ts[0] = '\0';
    if (tm) strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);
    /* Kernel interrupt-hook pair (see docs/BOOT_BLOCKERS.md 9.8): SaveState
     * records its caller's $ra/$sp here and the game installs it as its entry.
     * A C body reaches the BIOS through a host call, whose $ra is the
     * 0x8F000000 sentinel - so this word is the cheapest test for "the hook
     * holds a host-call sentinel instead of a guest PC". */
    const uint32_t hook_ra = psx_read_word(0x8005FF38u);
    const uint32_t hook_sp = psx_read_word(0x8005FF3Cu);
    /* How many sentinel words sit in guest RAM at all, and where: the same
     * defect can land in any struct, and 0x8F000000 is never a legal guest
     * pointer. */
    int sentinels = 0;
    uint32_t sent_first[4] = {0, 0, 0, 0};
    {
        const unsigned char *ram = memory_get_ram_ptr();
        const uint32_t bytes = memory_get_ram_bytes();
        if (ram) {
            for (uint32_t off = 0; off + 4u <= bytes; off += 4u) {
                uint32_t w;
                memcpy(&w, ram + off, sizeof w);
                if (w == 0x8F000000u) {
                    if (sentinels < 4) sent_first[sentinels] = 0x80000000u + off;
                    sentinels++;
                }
            }
        }
    }
    fprintf(f,
            "{\"origin\":\"%s\",\"exit_code\":%d,\"detail\":\"%s\","
            "\"site\":\"%s:%d\",\"pid\":%lu,\"timestamp\":\"%s\","
            "\"current_func\":\"0x%08X\",\"last_store_pc\":\"0x%08X\","
            "\"cycle\":%llu,\"in_exception\":%d,\"trace\":%d,"
            "\"hook_ra\":\"0x%08X\",\"hook_sp\":\"0x%08X\","
            "\"sentinel_words\":%d,\"sentinel_at\":[\"0x%08X\",\"0x%08X\","
            "\"0x%08X\",\"0x%08X\"]}\n",
            origin ? origin : "(null)", code, detail ? detail : "",
            file ? file : "(unknown)", line,
            (unsigned long)_getpid(), ts,
            g_debug_current_func_addr, g_debug_last_store_pc,
            (unsigned long long)psx_get_cycle_count(),
            psx_get_in_exception(), psx_exit_trace_active(),
            hook_ra, hook_sp, sentinels,
            sent_first[0], sent_first[1], sent_first[2], sent_first[3]);
    fclose(f);
}

static void write_stack_file(const char *origin, int code) {
#if defined(_WIN32)
    void *frames[48];
    USHORT n = CaptureStackBackTrace(0, (DWORD)(sizeof(frames) / sizeof(frames[0])),
                                     frames, NULL);
    FILE *f = fopen(EXIT_TRACE_STACK, "wb");
    if (!f) return;
    fprintf(f, "origin %s exit_code %d\n", origin ? origin : "(null)", code);
    fprintf(f, "module_base %p\n", (void *)GetModuleHandleA(NULL));
    for (USHORT i = 0; i < n; i++)
        fprintf(f, "frame[%u] %p\n", (unsigned)i, frames[i]);
    write_notes(f);
    fclose(f);
#else
    (void)origin;
    (void)code;
#endif
}

void psx_exit_trace_record(const char *origin, int code,
                           const char *detail,
                           const char *file, int line) {
    write_termination_line(origin, code, detail, file, line);
}

void psx_exit_trace_hard_exit(int code) {
    if (psx_exit_trace_active()) write_stack_file("hard_exit", code);
    fprintf(stderr, "exit_trace: terminating(%d)\n", code);
    fflush(stderr);
    fflush(NULL);
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    _exit(code);
}

void psx_exit_trace_terminate(const char *origin, int code,
                              const char *detail,
                              const char *file, int line) {
    write_termination_line(origin, code, detail, file, line);
    if (psx_exit_trace_active()) write_stack_file(origin, code);
    fprintf(stderr, "exit_trace: terminating(%d) origin=%s at %s:%d%s%s\n",
            code, origin ? origin : "(null)", file ? file : "?", line,
            detail && *detail ? " — " : "", detail ? detail : "");
    fflush(stderr);
    fflush(NULL);
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    _exit(code);
}

/* ---- opt-in SIGABRT tracer ------------------------------------------- */
/* The crash-trace install (crash_trace.c:960) already routes SIGABRT into a
 * full report. This tracer exists for the window BEFORE that install — the
 * one where an abort() leaves a bare exit code 3 and an untouched report,
 * which is exactly the 2026-11-18 signature. It names the abort, then chains
 * to whatever handler was installed before it. */
#if defined(_WIN32)
#include <signal.h>

static void abort_trace_handler(int sig) {
    static char detail[96];
    snprintf(detail, sizeof(detail), "signal=%d (abort before crash_trace install)",
             sig);
    /* The CRT would end us with code 3 anyway; end with the same code, now
     * named. Terminating here (instead of returning into the CRT's abort
     * path) also skips the driver teardown that a wedged thread may block. */
    psx_exit_trace_terminate("sigabrt", 3, detail, __FILE__, __LINE__);
}

__attribute__((constructor))
static void exit_trace_install(void) {
    if (!psx_exit_trace_active()) return;
    /* Replaced later by crash_trace.c's handler (which writes the full run
     * report); this only covers the window before that install. */
    signal(SIGABRT, abort_trace_handler);
    psx_exit_trace_note("exit_trace: SIGABRT tracer installed");
}
#endif
