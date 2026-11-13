#include "vk_hd_pack.h"

#include "hd_texture_pack.h"
#include "texture_image_decode.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;

/* The Konami head/kit palettes live in VRAM rows 480..489 and a CLUT X is
 * always a multiple of 16 words (GP0 E1h carries a 16-word unit), so a palette
 * override is exactly one 16-word aligned block. */
constexpr int kPaletteEntries = 16;
constexpr int kPaletteWords = kPaletteEntries;

std::unordered_map<uint32_t, std::array<uint16_t, kPaletteEntries>> clut_overrides;
VkHdPackStats stats{};
std::string stats_root;
bool env_checked = false;

std::string ascii_lower(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

bool ends_with_ci(const std::string& value, const char* suffix) {
    const std::string lower = ascii_lower(value);
    const std::string wanted = ascii_lower(suffix);
    return lower.size() >= wanted.size() &&
           lower.compare(lower.size() - wanted.size(), wanted.size(), wanted) == 0;
}

bool parse_hex(const std::string& text, uint32_t* out) {
    if (!out || text.empty() || text.size() > 8) return false;
    uint32_t value = 0;
    for (const unsigned char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

/* `<texture_hash>-<palette_hash>.clut.png`. The texture half keeps the pack
 * keyed the same way as the GL pack; the palette half is what the upload-time
 * patch can actually resolve, because the CLUT is shared by every texture that
 * points at it. */
bool parse_clut_filename(const fs::path& path, uint32_t* texture_hash,
                         uint32_t* palette_hash) {
    if (ascii_lower(path.extension().string()) != ".png") return false;
    std::string stem = path.stem().string();
    if (!ends_with_ci(stem, ".clut")) return false;
    stem.resize(stem.size() - 5);
    const size_t dash = stem.find('-');
    if (dash == std::string::npos || stem.find('-', dash + 1) != std::string::npos)
        return false;
    return parse_hex(stem.substr(0, dash), texture_hash) &&
           parse_hex(stem.substr(dash + 1), palette_hash);
}

bool load_clut_image(const fs::path& path, std::array<uint16_t, kPaletteEntries>* out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
    if (encoded.empty()) return false;
    DecodedTextureImage image;
    std::string error;
    if (!texture_image_decode(encoded.data(), encoded.size(),
                              TEXTURE_IMAGE_DEFAULT_DECODED_BUDGET, image, error))
        return false;
    if (!image.width || !image.height || image.rgba.size() < size_t(image.width) * 4u)
        return false;
    const uint32_t count = image.width < kPaletteEntries ? image.width : kPaletteEntries;
    for (uint32_t i = 0; i < kPaletteEntries; ++i) {
        if (i >= count) { (*out)[i] = 0; continue; }
        const uint8_t* px = image.rgba.data() + size_t(i) * 4u;
        if (px[3] < 128) { (*out)[i] = 0x0000; continue; }
        (*out)[i] = static_cast<uint16_t>(((px[0] >> 3) & 0x1F) |
                                         (((px[1] >> 3) & 0x1F) << 5) |
                                         (((px[2] >> 3) & 0x1F) << 10));
    }
    return true;
}

void add_clut_directory(const fs::path& directory) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) return;
    for (fs::directory_iterator it(directory, error), end; !error && it != end;
         it.increment(error)) {
        uint32_t texture_hash = 0, palette_hash = 0;
        if (!parse_clut_filename(it->path(), &texture_hash, &palette_hash)) continue;
        std::array<uint16_t, kPaletteEntries> entries{};
        if (!load_clut_image(it->path(), &entries)) continue;
        if (clut_overrides.emplace(palette_hash, entries).second) ++stats.clut_files;
    }
}

void scan_clut_overrides(const char* root) {
    clut_overrides.clear();
    stats.clut_files = 0;
    if (!root || !root[0]) return;
    std::error_code error;
    const fs::path base = fs::u8path(root);
    add_clut_directory(base);
    add_clut_directory(base / "replacements");
    for (fs::directory_iterator it(base, error), end; !error && it != end;
         it.increment(error)) {
        if (!it->is_directory(error)) continue;
        if (ends_with_ci(it->path().filename().string(), "-texture-replacements"))
            add_clut_directory(it->path());
    }
}

void refresh_stats(void) {
    GpuHdTextureDiag diag{};
    gpu_hd_textures_get_diag(&diag);
    stats.loaded = gpu_hd_textures_replacements_enabled() ? 1 : 0;
    stats.entries = diag.replacement_count;
    stats_root = diag.root ? diag.root : "";
    stats.root = stats_root.c_str();
}

}  // namespace

extern "C" int vk_hd_pack_init(void) {
    if (env_checked) { refresh_stats(); return stats.loaded; }
    env_checked = true;
    if (!gpu_hd_textures_active()) {
        const char* root = std::getenv("PSXRECOMP_HD_PACK");
        if (!root || !root[0]) root = std::getenv("PSXRECOMP_HD_TEXTURE_ROOT");
        if (root && root[0]) {
            char error[256]{};
            if (!gpu_hd_textures_configure(root, 1, 0, error, sizeof(error)))
                std::fprintf(stderr, "psxrecomp: vulkan hd pack: %s\n", error);
        }
    }
    refresh_stats();
    if (stats.loaded) scan_clut_overrides(stats_root.c_str());
    return stats.loaded;
}

extern "C" int vk_hd_pack_set_root(const char* root, char* error, size_t capacity) {
    if (error && capacity) error[0] = 0;
    if (!root || !root[0]) {
        gpu_hd_textures_shutdown();
        clut_overrides.clear();
        stats.clut_files = 0;
        refresh_stats();
        return 1;
    }
    if (!gpu_hd_textures_configure(root, 1, 0, error, capacity)) return 0;
    env_checked = true;
    refresh_stats();
    scan_clut_overrides(stats_root.c_str());
    return 1;
}

extern "C" int vk_hd_pack_active(void) { return gpu_hd_textures_replacements_enabled(); }

extern "C" void vk_hd_pack_get_stats(VkHdPackStats* out) {
    if (!out) return;
    refresh_stats();
    *out = stats;
    out->root = stats_root.c_str();
}

extern "C" int vk_hd_pack_acquire(uint16_t texpage, uint16_t clut_x, uint16_t clut_y,
                                  const int limits[4], uint32_t window, int semi,
                                  GpuHdTextureImage* out) {
    if (!out) return 0;
    *out = GpuHdTextureImage{};
    if (!gpu_hd_textures_acquire_draw(texpage, clut_x, clut_y, limits, window, semi, out)) {
        ++stats.misses;
        return 0;
    }
    return 1;
}

extern "C" void vk_hd_pack_release(GpuHdTextureImage* image) {
    if (!image) return;
    gpu_hd_textures_release_image(image);
}

extern "C" void vk_hd_pack_note_texture(uint32_t width, uint32_t height) {
    ++stats.overrides;
    stats.pixels_replaced += static_cast<uint64_t>(width) * height;
}

extern "C" int vk_hd_pack_clut_lookup(const uint16_t* palette16, uint16_t out[16]) {
    if (!palette16 || !out || clut_overrides.empty()) return 0;
    const uint32_t hash = hd_texture_crc32_words_le(palette16, kPaletteWords);
    const auto found = clut_overrides.find(hash);
    if (found == clut_overrides.end()) return 0;
    for (int i = 0; i < kPaletteEntries; ++i) out[i] = found->second[i];
    return 1;
}

extern "C" int vk_hd_pack_patch_clut(uint16_t* words, int x, int y, int w, int h) {
    if (!words || clut_overrides.empty() || w <= 0 || h <= 0) return 0;
    if (w > 1024 || h > 512) return 0;
    int patched = 0;
    for (int row = 0; row < h; ++row) {
        /* Word x is the VRAM X; only fully covered, 16-word aligned blocks can
         * be a CLUT (GP0 E1h addresses whole 16-entry units). */
        const int first = (x + 15) & ~15;
        for (int col = first; col + kPaletteWords <= x + w; col += kPaletteWords) {
            uint16_t* block = words + static_cast<size_t>(row) * w + (col - x);
            std::array<uint16_t, kPaletteEntries> replacement{};
            if (!vk_hd_pack_clut_lookup(block, replacement.data())) continue;
            for (int i = 0; i < kPaletteEntries; ++i) {
                const uint16_t old = block[i];
                /* bit15 is the PS1 semi-transparency flag, not colour: keep the
                 * uploaded STP bit and replace only the 5-5-5 colour. */
                block[i] = static_cast<uint16_t>((old & 0x8000u) |
                                                 (replacement[i] & 0x7FFFu));
            }
            ++patched;
        }
    }
    stats.palettes += static_cast<uint64_t>(patched);
    return patched;
}
