#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <filesystem>
#include <string>
#include <vector>
#include "mod_packages.h"
#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

namespace PSXRecompV4 {

bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            uint32_t game_entry_pc,
                            const std::filesystem::path& exe_path = {},
                            std::string* error = nullptr);
bool mod_runtime_commit(const std::filesystem::path& disc_path = {},
                        std::string* error = nullptr);
/* Drop the in-session mod plan for a netplay launch without rewriting the
 * user's persisted offline selection on disk. Netplay is always vanilla for
 * now (no synced mod plans). */
bool mod_runtime_clear_for_netplay(std::string* error = nullptr);
/* Netplay: clear the plan, then keep only the player's own features whose
 * every contribution is a [[plugin]] with netplay = "local_view" (no writes,
 * overlays or derived discs). They activate as usual, but their hooks run
 * only inside psx_mod_render_local_view and their vblank/savestate callbacks
 * not at all, so the shared simulation stays stock. Never saves state.toml. */
bool mod_runtime_commit_netplay_view(const std::filesystem::path& disc_path,
                                     std::string* error = nullptr);
/* 1 while the session plan is such an own-view plan. */
bool mod_runtime_netplay_view_active();
/* "package/feature" keys of `plan` that qualify as own-view features. */
std::vector<std::string> mod_runtime_netplay_view_features(const ModResolution& plan);
const std::string& mod_runtime_fingerprint();
const std::filesystem::path& mod_runtime_effective_disc_path();
/* Read an effective-disc file as whole sectors (true end-of-file tail bytes
 * included). max_bytes = 0 allows a full CD. Emulation-thread only. */
bool mod_runtime_read_disc_file_sectors(const std::string& path, uint32_t max_bytes,
                                        std::vector<uint8_t>& padded, uint32_t& lba,
                                        uint32_t& size, std::string* error = nullptr);

#if defined(RECOMP_LAUNCHER)
const ::RecompLauncherCModProvider* mod_runtime_launcher_provider();
/* [runtime] hide_hidden_mod_features: the launcher never presents a hidden
 * feature (recomp-ui RecompLauncherCModProvider::hide_hidden_features). */
void mod_runtime_set_hide_hidden_features(bool hide);
#endif

} // namespace PSXRecompV4
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Called before a guest dispatch. Applies the complete main-EXE plan
 * transactionally on the first dispatch to the configured entry point. */
void mod_runtime_on_dispatch(uint32_t target);
/* A full-machine savestate restores guest RAM after the initial entry-point
 * application. Reapply the already-validated main-EXE plan so the current
 * enabled mod selection remains authoritative after the restore. */
void mod_runtime_on_savestate_loaded(void);
/* Invokes activation callbacks for the committed plan. Call after the final
 * launcher commit and before renderer/window initialization. */
void mod_runtime_activate_plugins(void);
void mod_runtime_on_vblank(void);
/* Host-only context for a nested render transaction. Restore the interrupted
 * callback's depth/owner after longjmp; never serialize this into guest saves. */
typedef struct ModFunctionEntryContext {
    uint32_t depth;
    const void *plugin;
} ModFunctionEntryContext;
void mod_runtime_function_entry_context_save(ModFunctionEntryContext *out);
void mod_runtime_function_entry_context_restore(const ModFunctionEntryContext *in);
void mod_runtime_patch_disc_sector(uint32_t lba, int raw_sector,
                                   uint8_t* bytes, uint32_t size);
void mod_runtime_enable_disc_patches(void);
int mod_runtime_read_disc_extent(uint32_t lba, int raw_sector,
                                 uint8_t* bytes, uint32_t size);
uint32_t mod_runtime_disc_extent_start(void);
uint32_t mod_runtime_disc_sector_count(uint32_t base_count);

#ifdef __cplusplus
}
#endif
