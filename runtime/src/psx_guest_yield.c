/* psx_guest_yield.c — see psx_guest_yield.h.
 *
 * The runtime owns guest time (psx_cycles.c + the exact VBlank clock), so a
 * wait that has no guest instruction boundary to return to is served by
 * advancing to the next edge and delivering the interrupt there. That is the
 * same sequence psx_check_interrupts() performs for a compiled block leader,
 * which is why the guest sees the identical VBlank/RCNT values a real spin
 * would have produced.
 */

#include "psx_guest_yield.h"

#include "cpu_state.h"
#include "interrupts.h"
#include "psx_cycle_freeze.h"
#include "psx_cycles.h"

extern uint32_t psx_read_word(uint32_t addr);

/* Guest code lives in RAM (0x80010000..) and the BIOS (0xA0000000..). Anything
 * else - notably the 0x8F000000 sentinel a host-initiated call uses as $ra - is
 * not a usable exception return address. */
static int guest_resume_pc(uint32_t pc)
{
    return pc >= 0x1000u && pc < 0x80200000u && (pc & 3u) == 0u;
}

static int s_yield_depth;

uint32_t we_yield_until_vblank(struct CPUState* cpu)
{
    if (!cpu || s_yield_depth) return 0;
    if (g_psx_guest_time_frozen) return 0;     /* render pass / uncharged call */
    if (psx_get_in_exception()) return 0;      /* handler must not re-yield */

    s_yield_depth = 1;
    const uint64_t before = psx_get_cycle_count();
    uint32_t to_edge = interrupts_cycles_to_vblank();
    if (to_edge == 0u) to_edge = 1u;           /* already at the edge: cross it */
    psx_advance_cycles(to_edge);

    /* Deliver what the advance raised. The VBlank handler is what clears the
     * game's flip-request word, so this is the step the C spin was missing. */
    const uint32_t resume = cpu->gpr[31];
    if (guest_resume_pc(resume)) psx_check_interrupts_at(cpu, resume);
    else psx_check_interrupts(cpu);

    s_yield_depth = 0;
    return psx_get_cycle_count() != before ? 1u : 0u;
}

int we_wait_guest_word(struct CPUState* cpu, uint32_t addr, uint32_t mask,
                       uint32_t want, uint32_t max_vblanks)
{
    if ((psx_read_word(addr) & mask) == want) return 1;
    for (uint32_t i = 0; i < max_vblanks; i++) {
        if (!we_yield_until_vblank(cpu)) return 0;
        if ((psx_read_word(addr) & mask) == want) return 1;
    }
    return 0;
}
