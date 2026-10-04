#ifndef PSXRECOMP_FREEZE_DUMP_POLICY_H
#define PSXRECOMP_FREEZE_DUMP_POLICY_H

#include <stddef.h>
#include <stdint.h>

/* Full freeze dumps contain several large trace rings. Keep automatic
 * watchdog output bounded while leaving deliberate fatal dumps independent. */
#define FREEZE_DUMP_ORDINARY_SLOTS 1u
#define FREEZE_DUMP_HARD_SLOTS 1u
#define FREEZE_DUMP_AUTO_LIMIT \
    (FREEZE_DUMP_ORDINARY_SLOTS + FREEZE_DUMP_HARD_SLOTS)
#define FREEZE_DUMP_MAX_FAILED_ATTEMPTS 3u
/* Policy hysteresis, independent of the detector's sample window. The total
 * time before rearm can therefore be longer than this tick count alone. */
#define FREEZE_DUMP_REARM_HEALTHY_TICKS 20u

typedef struct FreezeDumpPolicy {
    uint32_t automatic_dumps;
    uint32_t failed_dumps;
    uint32_t suppressed_events;
    uint32_t healthy_ticks;
    uint32_t wedge_kind;
    uint32_t pending_kind;
    uint32_t ordinary_failed_attempts;
    uint32_t hard_failed_attempts;
    int ordinary_slot_consumed;
    int hard_slot_consumed;
    int in_wedge;
} FreezeDumpPolicy;

/* A repeated poll-site sample is not a spin freeze while new GPU command
 * submissions advance. In particular, a healthy VSync wait can occupy the
 * same function/store PCs at both ends of the watchdog window. */
typedef struct FreezeGuestProgress {
    uint32_t function_pc;
    uint32_t store_pc;
    uint64_t interpreted_insns;
    uint64_t gpu_writes;
} FreezeGuestProgress;

static inline int freeze_guest_progress_pinned(FreezeGuestProgress before,
                                              FreezeGuestProgress after) {
    return before.function_pc == after.function_pc &&
           before.store_pc == after.store_pc &&
           before.interpreted_insns == after.interpreted_insns &&
           before.gpu_writes == after.gpu_writes;
}

/* Observe one watchdog sample. Returns nonzero when the caller must write an
 * automatic full dump. A new event requires a sustained healthy interval.
 * Fatal state bypasses this automatic policy and does not change its budget. */
int freeze_dump_policy_observe(FreezeDumpPolicy *policy, uint32_t wedge_kind,
                               int fatal_active);

/* Record whether the authorized full dump reached disk. A slot permits a
 * bounded number of failed attempts before the policy consumes that slot. */
void freeze_dump_policy_record_result(FreezeDumpPolicy *policy, int written);

/* Format a unique dump path. The sequence prevents same-second truncation. */
int freeze_dump_format_path(char *out, size_t cap, const char *backend,
                            long long wall, uint32_t sequence);

#endif /* PSXRECOMP_FREEZE_DUMP_POLICY_H */
