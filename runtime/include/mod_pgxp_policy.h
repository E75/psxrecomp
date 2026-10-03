#ifndef PSX_MOD_PGXP_POLICY_H
#define PSX_MOD_PGXP_POLICY_H

/* Session overrides are independent of the player's persistent video config.
 * Renderer initialization resolves them again after activation, because GTE
 * and GPU initialization may reset the live correction flags. */
typedef struct PSXModPgxpConfig {
    int geometry, textures, cpu;
} PSXModPgxpConfig;
typedef struct PSXModPgxpPolicy {
    int enabled, cpu; /* -1: no mod selection */
} PSXModPgxpPolicy;
static inline void psx_mod_pgxp_reset(PSXModPgxpPolicy* policy) {
    policy->enabled = policy->cpu = -1;
}
static inline void psx_mod_pgxp_select(PSXModPgxpPolicy* policy, int enabled, int cpu) {
    policy->enabled = enabled != 0;
    policy->cpu = cpu != 0;
}
static inline PSXModPgxpConfig psx_mod_pgxp_resolve(
        const PSXModPgxpPolicy* policy, PSXModPgxpConfig base) {
    if (policy->enabled >= 0) {
        base.geometry = base.textures = policy->enabled;
        base.cpu = policy->cpu;
    }
    return base;
}
#endif
