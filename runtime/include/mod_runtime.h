#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <filesystem>
#include <string>
#include <vector>
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
                        std::string* error = nullptr,
                        bool save_selection = true);
/* Audited preboot only: load and verify a plan using existing receipts.
 * A miss never invokes a media converter or computes a disc digest. */
bool mod_runtime_try_prepare_cached(const std::filesystem::path& disc_path);

/* Prepare enabled media without installing a guest plan or saving choices. */
bool mod_runtime_prepare_resources(const std::filesystem::path& disc_path,
                                   std::string* error = nullptr);
/* Apply a host-published online/LAN plan without rewriting persisted offline
 * choices. An explicit empty host plan means vanilla; direct launches without
 * a published plan retain local choices and require transport consensus. */
bool mod_runtime_commit_for_netplay(const std::filesystem::path& disc_path = {},
                                    std::string* error = nullptr);
/* Direct/LAN: retain the locally selected verified plan. The transport must
 * compare session_plan_fp() on every peer before executing the guest. */
bool mod_runtime_commit_for_direct_netplay(const std::filesystem::path& disc_path,
                                           std::string* error = nullptr);
bool mod_runtime_clear_for_netplay(std::string* error = nullptr);
/* [netplay] content_negotiation from game.toml. Off (default): the launcher's
 * netplay commit clears the plan exactly like mod_runtime_clear_for_netplay. */
void mod_runtime_set_netplay_content_negotiation(bool enabled);
bool mod_runtime_netplay_content_negotiation();
/* Restore the pre-session selection before returning to the launcher. */
void mod_runtime_end_netplay();
void mod_runtime_set_session_plan_fp(const std::string& fp);
const std::string& mod_runtime_session_plan_fp();
std::string mod_runtime_plan_fingerprint_portable();
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
int mod_runtime_cdda_track_count(void); /* zero when no playlist is active */
uint32_t mod_runtime_cdda_track_start(int track); /* zero = lead-out */
/* 0 unavailable, 1 copied external PCM, 2 mapped to mounted-disc LBA. */
int mod_runtime_read_cdda_sector(uint32_t lba, uint8_t* bytes, uint32_t size,
                                 uint32_t* source_lba);

#ifdef __cplusplus
}
#endif
