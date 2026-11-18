/* psx_static_guest.c - see psx_static_guest.h.
 *
 * Why this exists (docs/STATIC_PORT.md): the port can only claim "guest code
 * is Sony/PsyQ only" from measurement, not from inspection. The funnel table
 * in that doc is produced by this file. Keep it inert: the only cost in a
 * normal build is one int test at each call site.
 */
#include "psx_static_guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int g_psx_static_guest_mode = 0;

#define SG_TABLE_CAP   8192u
#define SG_RING_CAP    512u
#define SG_STRANGERS   64u      /* unique non-BIOS targets printed per run */

typedef struct SgEntry {
    uint32_t target;
    uint32_t last_caller;
    uint64_t count;
    uint64_t insns;       /* reserved: filled by the caller's own counters */
    uint8_t  funnel;
    uint8_t  klass;
    uint8_t  used;
} SgEntry;

typedef struct SgRingEntry {
    uint64_t seq;
    uint32_t target;
    uint32_t caller;
    uint8_t  funnel;
    uint8_t  klass;
} SgRingEntry;

static SgEntry  s_table[SG_TABLE_CAP];
static uint32_t s_used;
static SgRingEntry s_ring[SG_RING_CAP];
static uint64_t s_seq;
static uint64_t s_totals[PSX_SG_FUNNEL_COUNT];
static uint64_t s_nonbios;
static int      s_inited;
static const char *s_dump_path;
static int      s_abort_mode;
static int      s_allow_bios = 1;
static uint32_t s_strangers;

static void sg_dump(void);
static void sg_init(void);

/* Initialise at load, not on first use: the call sites test
 * g_psx_static_guest_mode as a plain int and only call in when it is nonzero,
 * so a lazy init inside those functions could never run. */
#if defined(_MSC_VER)
static void sg_ctor(void) { sg_init(); }
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static void (*sg_ctor_p)(void) = sg_ctor;
#else
__attribute__((constructor)) static void sg_ctor(void) { sg_init(); }
#endif

static void sg_init(void) {
    if (s_inited) return;
    s_inited = 1;
    {
        const char *e = getenv("PSX_STATIC_GUEST");
        if (e && *e) {
            if (strcmp(e, "abort") == 0) {
                g_psx_static_guest_mode = 2;
                s_abort_mode = 1;
            } else {
                g_psx_static_guest_mode = 1;
            }
        }
        e = getenv("PSX_STATIC_GUEST_ALLOW_BIOS");
        if (e && *e) s_allow_bios = atoi(e) != 0;
        e = getenv("PSX_STATIC_GUEST_DUMP");
        if (e && *e) s_dump_path = e;
    }
    if (s_dump_path) atexit(sg_dump);
    if (g_psx_static_guest_mode)
        fprintf(stderr, "psx_static_guest: mode=%s dump=%s\n",
                g_psx_static_guest_mode == 2 ? "abort" : "log",
                s_dump_path ? s_dump_path : "(none)");
}

int psx_static_guest_mode(void) { sg_init(); return g_psx_static_guest_mode; }

const char *psx_static_guest_funnel_name(int funnel) {
    switch (funnel) {
    case PSX_SG_FUNNEL_DISPATCH_CALL: return "dispatch_call";
    case PSX_SG_FUNNEL_MOD_CALL_GUEST: return "mod_call_guest";
    case PSX_SG_FUNNEL_INTERP: return "interp";
    case PSX_SG_FUNNEL_SPAN: return "span";
    case PSX_SG_FUNNEL_NATIVE_HANDOFF: return "native_handoff";
    default: return "?";
    }
}

/* Sony/PsyQ class. The kernel window [0,0x10000) holds the BIOS's relocated
 * part-2 image plus the install-at-runtime stubs the BIOS library writes
 * there; the ROM window is the BIOS itself; 0xA0/0xB0/0xC0 are the syscall
 * vectors a PsyQ stub jumps to. Everything else is game code until proven
 * otherwise (overlay images, boot EXE text, 0x0F000000 mod code). */
int psx_static_guest_class(uint32_t target) {
    const uint32_t phys = target & 0x1FFFFFFFu;
    if (phys == 0xA0u || phys == 0xB0u || phys == 0xC0u) return PSX_SG_CLASS_BIOS;
    if (phys < 0x00010000u) return PSX_SG_CLASS_BIOS;
    if (phys >= 0x1FC00000u && phys < 0x1FC80000u) return PSX_SG_CLASS_BIOS;
    return PSX_SG_CLASS_GAME;
}

static SgEntry *sg_find(uint32_t target, int funnel, int create) {
    uint32_t h = (target * 2654435761u) ^ ((uint32_t)funnel * 40503u);
    uint32_t i = h & (SG_TABLE_CAP - 1u);
    for (uint32_t probe = 0; probe < SG_TABLE_CAP; probe++) {
        SgEntry *e = &s_table[(i + probe) & (SG_TABLE_CAP - 1u)];
        if (!e->used) {
            if (!create) return 0;
            e->used = 1;
            e->target = target;
            e->funnel = (uint8_t)funnel;
            e->klass = (uint8_t)psx_static_guest_class(target);
            s_used++;
            return e;
        }
        if (e->target == target && e->funnel == (uint8_t)funnel) return e;
    }
    return 0;
}

void psx_static_guest_note(int funnel, uint32_t target, uint32_t caller) {
    SgEntry *e;
    sg_init();
    if (!g_psx_static_guest_mode) return;
    if (funnel < 0 || funnel >= PSX_SG_FUNNEL_COUNT) return;
    s_totals[funnel]++;
    e = sg_find(target, funnel, 1);
    if (!e) return;
    if (e->count == 0) {
        /* first hit for this (funnel,target) */
        if (e->klass != PSX_SG_CLASS_BIOS) {
            s_nonbios++;
            if (s_strangers < SG_STRANGERS) {
                s_strangers++;
                fprintf(stderr,
                        "psx_static_guest: %s -> 0x%08X (%s) caller 0x%08X\n",
                        psx_static_guest_funnel_name(funnel), target,
                        e->klass == PSX_SG_CLASS_GAME ? "game" : "unknown",
                        caller);
                fflush(stderr);
            }
        }
        if (s_dump_path) sg_dump();
    }
    e->count++;
    e->last_caller = caller;
    if (s_abort_mode && !(s_allow_bios && e->klass == PSX_SG_CLASS_BIOS)) {
        fprintf(stderr,
                "psx_static_guest: ABORT static-guest violation: %s -> 0x%08X "
                "caller 0x%08X (docs/STATIC_PORT.md)\n",
                psx_static_guest_funnel_name(funnel), target, caller);
        fflush(stderr);
        if (s_dump_path) sg_dump();
        abort();
    }
    {
        SgRingEntry *r = &s_ring[(uint32_t)(s_seq & (SG_RING_CAP - 1u))];
        s_seq++;
        r->seq = s_seq;
        r->target = target;
        r->caller = caller;
        r->funnel = (uint8_t)funnel;
        r->klass = (uint8_t)e->klass;
    }
    if (s_dump_path && (s_totals[funnel] & 0xFFFu) == 0u) sg_dump();
}

uint64_t psx_static_guest_total(int funnel) {
    sg_init();
    return (funnel >= 0 && funnel < PSX_SG_FUNNEL_COUNT) ? s_totals[funnel] : 0u;
}

uint64_t psx_static_guest_nonbios_total(void) { sg_init(); return s_nonbios; }
uint32_t psx_static_guest_distinct(void) { sg_init(); return s_used; }

static int sg_cmp(const void *a, const void *b) {
    const SgEntry *x = (const SgEntry *)a, *y = (const SgEntry *)b;
    if (x->klass != y->klass) return (int)x->klass - (int)y->klass;
    if (x->funnel != y->funnel) return (int)x->funnel - (int)y->funnel;
    if (x->target < y->target) return -1;
    if (x->target > y->target) return 1;
    return 0;
}

static void sg_dump(void) {
    FILE *f;
    static SgEntry rows[SG_TABLE_CAP];
    char tmp[1024];
    uint32_t n = 0;
    if (!s_dump_path) return;
    /* Write-then-replace: a reader (the measure tool) must never see the file
     * truncated by the next rewrite. */
    if (snprintf(tmp, sizeof tmp, "%s.tmp", s_dump_path) >= (int)sizeof tmp) return;
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# psx_static_guest dump: mode=%d distinct=%u\n",
            g_psx_static_guest_mode, s_used);
    for (int k = 0; k < PSX_SG_FUNNEL_COUNT; k++)
        fprintf(f, "# total %s = %llu\n", psx_static_guest_funnel_name(k),
                (unsigned long long)s_totals[k]);
    fprintf(f, "# total nonbios_distinct = %llu\n", (unsigned long long)s_nonbios);
    fprintf(f, "funnel,target,class,count,last_caller\n");
    for (uint32_t i = 0; i < SG_TABLE_CAP; i++)
        if (s_table[i].used) rows[n++] = s_table[i];
    qsort(rows, n, sizeof rows[0], sg_cmp);
    for (uint32_t i = 0; i < n; i++)
        fprintf(f, "%s,0x%08X,%s,%llu,0x%08X\n",
                psx_static_guest_funnel_name(rows[i].funnel), rows[i].target,
                rows[i].klass == PSX_SG_CLASS_BIOS ? "bios" : "game",
                (unsigned long long)rows[i].count, rows[i].last_caller);
    fclose(f);
#if defined(_WIN32)
    MoveFileExA(tmp, s_dump_path, MOVEFILE_REPLACE_EXISTING);
#else
    remove(s_dump_path);
    rename(tmp, s_dump_path);
#endif
}

void psx_static_guest_dump(void) { sg_init(); if (s_dump_path) sg_dump(); }
