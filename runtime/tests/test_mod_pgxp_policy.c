#include "mod_pgxp_policy.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    PSXModPgxpPolicy policy;
    PSXModPgxpConfig configured = {0, 0, 1};
    psx_mod_pgxp_reset(&policy);
    PSXModPgxpConfig live = psx_mod_pgxp_resolve(&policy, configured);
    assert(!live.geometry && !live.textures && live.cpu);

    /* Activation before renderer initialization must survive re-resolution. */
    psx_mod_pgxp_select(&policy, 1, 0);
    live = psx_mod_pgxp_resolve(&policy, configured);
    assert(live.geometry && live.textures && !live.cpu);
    assert(!configured.geometry && !configured.textures && configured.cpu);

    /* A later vanilla/netplay session inherits the player's config only. */
    psx_mod_pgxp_reset(&policy);
    live = psx_mod_pgxp_resolve(&policy, configured);
    assert(!live.geometry && !live.textures && live.cpu);
    configured.geometry = 1;
    live = psx_mod_pgxp_resolve(&policy, configured);
    assert(live.geometry && !live.textures && live.cpu);

    psx_mod_pgxp_select(&policy, 0, 1);
    live = psx_mod_pgxp_resolve(&policy, configured);
    assert(!live.geometry && !live.textures && live.cpu);
    psx_mod_pgxp_select(&policy, 1, 1);
    live = psx_mod_pgxp_resolve(&policy, configured);
    assert(live.geometry && live.textures && live.cpu);
    puts("PGXP session policy passed");
    return 0;
}
