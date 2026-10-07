#include "gpu_hd_textures.h"
#include "gpu_gl_renderer.h"
#include "hd_texture_pack.h"
#include "duckstation_texture_pack.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>

namespace {
constexpr size_t kVramWords = 1024u * 512u;
struct Session {
    std::string root;
    HdTexturePack* beetle = nullptr;
    DuckTexturePack* duck = nullptr;
    bool replacements = false;
    bool dump = false;
    bool dump_failed = false;
    GpuHdTextureDiag diag{};
    ~Session() { hd_texture_pack_destroy(beetle); duck_texture_pack_destroy(duck); }
};
struct Lease {
    HdTexturePixels beetle{};
    DuckTexturePixels duck{};
    ~Lease() { hd_texture_pixels_release(&beetle); duck_texture_pixels_release(&duck); }
};
std::unique_ptr<Session> session;
const uint16_t* native_vram = nullptr;
uint64_t generation = 1;

void fail(char* error, size_t capacity, const char* message) {
    if (error && capacity) std::snprintf(error, capacity, "%s", message);
}
bool is_beetle_root(const std::filesystem::path& root) {
    std::error_code error;
    if (std::filesystem::exists(root / "Hashes.ini", error)) return true;
    if (std::filesystem::exists(root.parent_path() / "Hashes.ini", error)) return true;
    const auto name = root.filename().u8string();
    const std::string suffix = "-texture-replacements";
    return name.size() >= suffix.size() &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}
int window_coord(int value, int mask, int offset) {
    return (value & ~(mask * 8)) | ((offset & mask) * 8);
}
bool make_query(uint16_t texpage, uint16_t cx, uint16_t cy, const int limits[4],
                uint32_t window, HdTextureDrawQuery& query) {
    if (!native_vram || !limits || ((texpage >> 7) & 3u) == 3u) return false;
    if (limits[0] < 0 || limits[1] < 0 || limits[2] > 255 || limits[3] > 255 ||
        limits[0] > limits[2] || limits[1] > limits[3]) return false;
    int first[2] = {255,255}, last[2] = {0,0};
    for (int axis = 0; axis < 2; ++axis) {
        int mask = (window >> (axis * 5)) & 31;
        int offset = (window >> (10 + axis * 5)) & 31;
        for (int p = limits[axis]; p <= limits[axis + 2]; ++p) {
            const int mapped = window_coord(p, mask, offset);
            first[axis] = std::min(first[axis], mapped);
            last[axis] = std::max(last[axis], mapped);
        }
    }
    query = {};
    query.page_x = (texpage & 15u) * 64u;
    query.page_y = ((texpage >> 4) & 1u) * 256u;
    query.depth = (texpage >> 7) & 3u;
    query.u_first = static_cast<uint8_t>(first[0]);
    query.u_last = static_cast<uint8_t>(last[0]);
    query.v_first = static_cast<uint8_t>(first[1]);
    query.v_last = static_cast<uint8_t>(last[1]);
    query.clut_x = cx; query.clut_y = cy;
    query.vram = native_vram; query.vram_word_count = kVramWords;
    return true;
}
void dump_query(const HdTextureDrawQuery& query, int semi) {
    if (!session->dump || session->dump_failed) return;
    char error[256]{};
    const int status = duck_texture_pack_dump_draw(session->duck, &query, semi, error, sizeof(error));
    if (status == HD_TEXTURE_LOOKUP_FOUND) ++session->diag.dumped_textures;
    if (status < 0) {
        std::fprintf(stderr, "psxrecomp: texture dump stopped: %s\n", error);
        /* Keep the configured raster authority until a safe session boundary.
         * Tearing it down inside a pre-draw query would lose command order. */
        session->dump_failed = true;
    }
}
}

extern "C" int gpu_hd_textures_configure(const char* root, int replacements,
                                          int dump, char* error, size_t capacity) {
    if (error && capacity) error[0] = 0;
    if ((!root || !root[0]) && !replacements && !dump) {
        gpu_hd_textures_shutdown(); return 1;
    }
    if (!root || !root[0]) { fail(error, capacity, "Select an external texture-pack directory."); return 0; }
    try {
        auto next = std::make_unique<Session>();
        next->root = root; next->replacements = replacements != 0; next->dump = dump != 0;
        const bool beetle = is_beetle_root(std::filesystem::u8path(root));
        if (beetle) {
            if (!hd_texture_pack_create(root, &next->beetle, error, capacity)) return 0;
        } else {
            char ignored[256]{};
            hd_texture_pack_create(root, &next->beetle, ignored, sizeof(ignored));
        }
        if (!next->beetle) {
            auto directory = std::filesystem::u8path(root);
            if (directory.filename().u8string() == "replacements") directory = directory.parent_path();
            std::filesystem::create_directories(directory / "replacements");
            std::filesystem::create_directories(directory / "dumps");
        }
        if (!duck_texture_pack_create(root, &next->duck, error, capacity)) return 0;
        if (session && session->root == next->root) {
            if (!duck_texture_pack_copy_tracking(next->duck, session->duck)) {
                fail(error, capacity, "Could not preserve texture-upload tracking during reload."); return 0;
            }
            if (session->beetle && next->beetle) {
                uint8_t* saved = nullptr; size_t saved_size = 0;
                if (!hd_texture_pack_tracking_state_save(session->beetle, &saved, &saved_size)) {
                    fail(error, capacity, "Could not save Beetle texture-upload tracking during reload."); return 0;
                }
                const int restored = hd_texture_pack_tracking_state_load(next->beetle, saved, saved_size);
                std::free(saved);
                if (!restored) { fail(error, capacity, "Could not restore Beetle texture-upload tracking during reload."); return 0; }
            }
        }
        /* This also flushes all queued users before old replacement textures
         * and pixel leases may be destroyed. */
        gl_renderer_set_hd_texture_mode(next->replacements || next->dump);
        gl_renderer_clear_hd_texture_cache();
        session = std::move(next); ++generation;
        DuckTexturePackInfo info{};
        duck_texture_pack_get_info(session->duck, &info);
        size_t replacements_found = info.replacement_count;
        size_t ambiguous = info.ambiguous_count;
        if (session->beetle) {
            HdTexturePackInfo beetle_info{};
            hd_texture_pack_get_info(session->beetle, &beetle_info);
            replacements_found = beetle_info.unique_key_count;
            ambiguous = beetle_info.ambiguous_key_count;
        }
        std::fprintf(stdout, "psxrecomp: texture pack '%s': %zu replacements, %zu ambiguous, %zu ignored\n",
                     session->root.c_str(), replacements_found, ambiguous, info.ignored_count);
        if (info.diagnostic && info.diagnostic[0])
            std::fprintf(stderr, "psxrecomp: texture pack: %s\n", info.diagnostic);
        return 1;
    } catch (const std::exception& exception) {
        fail(error, capacity, exception.what()); return 0;
    }
}
extern "C" void gpu_hd_textures_shutdown(void) {
    gl_renderer_set_hd_texture_mode(0);
    gl_renderer_clear_hd_texture_cache();
    session.reset(); ++generation;
}
extern "C" int gpu_hd_textures_reload(char* error, size_t capacity) {
    if (!session) { fail(error, capacity, "No texture-pack session is active."); return 0; }
    const std::string root = session->root;
    return gpu_hd_textures_configure(root.c_str(), session->replacements, session->dump, error, capacity);
}
extern "C" void gpu_hd_textures_set_dump_enabled(int enabled) {
    if (!session || session->dump == (enabled != 0)) return;
    gl_renderer_set_hd_texture_mode(session->replacements || enabled);
    session->dump = enabled != 0;
    session->dump_failed = false;
}
extern "C" int gpu_hd_textures_replacements_enabled(void) { return session && session->replacements; }
extern "C" int gpu_hd_textures_dump_enabled(void) { return session && session->dump; }
extern "C" int gpu_hd_textures_active(void) { return session && (session->replacements || session->dump); }
extern "C" void gpu_hd_textures_get_diag(GpuHdTextureDiag* out) {
    if (!out) return;
    *out = {};
    if (!session) { out->root = ""; return; }
    *out = session->diag;
    out->root = session->root.c_str(); out->active = gpu_hd_textures_active();
    out->replacements = session->replacements; out->dump = session->dump && !session->dump_failed;
    out->format = session->beetle ? 1 : 2;
    if (session->beetle) { HdTexturePackInfo info{}; hd_texture_pack_get_info(session->beetle, &info); out->replacement_count = info.unique_key_count; }
    else { DuckTexturePackInfo info{}; duck_texture_pack_get_info(session->duck, &info); out->replacement_count = info.replacement_count; }
}
extern "C" void gpu_hd_textures_note_applied(void) { if (session) ++session->diag.applied_draws; }
extern "C" void gpu_hd_textures_set_vram(const uint16_t* vram) { native_vram = vram; gpu_hd_textures_reset_tracking(); }
extern "C" void gpu_hd_textures_reset_tracking(void) {
    if (!session) return;
    hd_texture_pack_reset_tracking(session->beetle);
    duck_texture_pack_reset_tracking(session->duck);
}
extern "C" void gpu_hd_textures_invalidate(int x, int y, int width, int height) {
    if (!session || width < 1 || height < 1) return;
    if (width > 1024 || height > 512) { gpu_hd_textures_reset_tracking(); return; }
    hd_texture_pack_invalidate(session->beetle, x & 1023, y & 511, width, height);
    duck_texture_pack_invalidate(session->duck, x & 1023, y & 511, width, height);
}
extern "C" void gpu_hd_textures_track_upload(int x, int y, int width, int height,
                                             const uint16_t* words) {
    if (!session || !native_vram || !words || width < 1 || width > 1024 || height < 1 || height > 512) return;
    if (width == 1024 && height == 512) { gpu_hd_textures_reset_tracking(); return; }
    /* gpu.c's A0 staging already contains post-mask words. Read the canonical
     * mirror here also for host-initiated transfers which use raw input. */
    const size_t count = static_cast<size_t>(width) * height;
    std::unique_ptr<uint16_t[]> applied(new (std::nothrow) uint16_t[count]);
    if (!applied) { gpu_hd_textures_reset_tracking(); return; }
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
            applied[static_cast<size_t>(row) * width + col] =
                native_vram[((y + row) & 511) * 1024 + ((x + col) & 1023)];
    hd_texture_pack_track_upload(session->beetle, x & 1023, y & 511, width, height,
                                 applied.get(), count, nullptr);
    duck_texture_pack_track_upload(session->duck, x & 1023, y & 511, width, height,
                                   native_vram, kVramWords);
}
extern "C" int gpu_hd_textures_acquire_draw(uint16_t tp, uint16_t cx, uint16_t cy,
                                            const int limits[4], uint32_t window,
                                            int semi, GpuHdTextureImage* image) {
    if (!image) return 0;
    *image = {};
    if (!session) return 0;
    ++session->diag.draw_queries;
    HdTextureDrawQuery query{};
    if (!make_query(tp, cx, cy, limits, window, query)) return 0;
    dump_query(query, semi);
    if (!session->replacements) return 0;
    auto lease = std::unique_ptr<Lease>(new (std::nothrow) Lease);
    if (!lease) return 0;
    if (session->beetle) {
        HdTextureMatch match{};
        if (hd_texture_pack_match(session->beetle, &query, &match) != HD_TEXTURE_LOOKUP_FOUND) return 0;
        ++session->diag.matched_draws;
        hd_texture_pack_request_decode(session->beetle, match.entry.texture_hash, match.entry.palette_hash);
        if (hd_texture_pack_acquire_decoded(session->beetle, match.entry.texture_hash,
                                          match.entry.palette_hash, &lease->beetle) != HD_TEXTURE_LOOKUP_FOUND) return 0;
        const unsigned pixels_per_word = 4u >> query.depth;
        image->rgba = lease->beetle.rgba; image->width = lease->beetle.width;
        image->height = lease->beetle.height; image->stride = lease->beetle.stride;
        image->source_width = match.upload_width_words * pixels_per_word;
        image->source_height = match.upload_height;
        image->origin_u = query.u_first - (match.source_word_x * pixels_per_word + (query.u_first % pixels_per_word));
        image->origin_v = query.v_first - match.source_y;
        image->cache_key = (static_cast<uint64_t>(match.entry.texture_hash) << 32) | match.entry.palette_hash;
        image->alpha_mode = 1;
    } else {
        DuckTextureMatch match{};
        if (duck_texture_pack_match_draw(session->duck, &query, semi, &match) != HD_TEXTURE_LOOKUP_FOUND) return 0;
        ++session->diag.matched_draws;
        duck_texture_pack_request_decode(session->duck, match.entry_id);
        if (duck_texture_pack_acquire_decoded(session->duck, match.entry_id, &lease->duck) != HD_TEXTURE_LOOKUP_FOUND) return 0;
        image->rgba = lease->duck.rgba; image->width = lease->duck.width;
        image->height = lease->duck.height; image->stride = lease->duck.stride;
        image->source_width = match.source_width; image->source_height = match.source_height;
        image->origin_u = match.origin_u; image->origin_v = match.origin_v;
        image->cache_key = match.entry_id;
        image->alpha_mode = match.key.semitransparent ? 3 : 2;
    }
    image->generation = generation; image->lease = lease.release();
    ++session->diag.ready_draws;
    return 1;
}
extern "C" void gpu_hd_textures_release_image(GpuHdTextureImage* image) {
    if (!image) return;
    delete static_cast<Lease*>(image->lease); *image = {};
}
extern "C" void gpu_hd_textures_observe_draw(uint16_t tp, uint16_t cx, uint16_t cy,
    const int limits[4], uint32_t window, int semi) {
    if (!session || !session->dump || session->dump_failed) return;
    HdTextureDrawQuery query{};
    if (make_query(tp, cx, cy, limits, window, query)) {
        ++session->diag.draw_queries; dump_query(query, semi);
    }
}
