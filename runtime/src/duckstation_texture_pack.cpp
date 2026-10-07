#include "duckstation_texture_pack.h"
#include "../third_party/stb_image.h"
#define XXH_INLINE_ALL
#include "../third_party/xxhash.h"
#include "png_write.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace fs = std::filesystem;
namespace {
constexpr size_t kWords = 1024u * 512u;
constexpr size_t kMaxEntries = 262144;
constexpr size_t kMaxUploads = 8192;
constexpr size_t kMaxRectCache = 256;
constexpr size_t kMaxDumps = 8192;
constexpr size_t kMaxDumpQueue = 8;
constexpr size_t kMaxDecodeQueue = 32;
constexpr size_t kDefaultBudget = 64u * 1024u * 1024u;
constexpr size_t kMaxEncoded = 64u * 1024u * 1024u;
constexpr uint32_t kMaxImageDimension = 8192;

void error_text(char* dest, size_t capacity, const std::string& text) {
    if (dest && capacity) std::snprintf(dest, capacity, "%s", text.c_str());
}
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
unsigned expansion(unsigned depth) { return depth == 0 ? 4 : depth == 1 ? 2 : 1; }
struct Rect { unsigned x, y, w, h; };
bool intersects(Rect a, Rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
bool contains(Rect a, Rect b) {
    return b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h;
}
bool valid_rect(Rect r) { return r.w && r.h && r.x + r.w <= 1024 && r.y + r.h <= 512; }

template<class T> bool number(const std::string& text, T& value, int base = 10) {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool dimensions(const std::string& s, uint16_t& w, uint16_t& h) {
    const size_t at = s.find('x');
    return at != std::string::npos && number(s.substr(0, at), w) &&
           number(s.substr(at + 1), h) && w && h;
}
bool valid_key(const DuckTextureKey& k) {
    if (k.kind > DUCK_TEXTURE_PAGE || k.depth > 2 || k.semitransparent > 1 ||
        !k.source_width_words || k.source_width_words > 1024 || !k.source_height ||
        k.source_height > 512 || !k.width || !k.height) return false;
    const unsigned ppw = expansion(k.depth);
    if (k.offset_x % ppw || k.width % ppw ||
        unsigned(k.offset_x) + k.width > unsigned(k.source_width_words) * ppw ||
        unsigned(k.offset_y) + k.height > k.source_height) return false;
    if (k.kind == DUCK_TEXTURE_PAGE && (k.source_width_words != 256 / ppw ||
        k.source_height != 256 || unsigned(k.offset_x) + k.width > 256 ||
        unsigned(k.offset_y) + k.height > 256)) return false;
    if (k.depth < 2 && (k.palette_min > k.palette_max ||
        k.palette_max >= (k.depth == 0 ? 16 : 256))) return false;
    return k.depth < 2 || (!k.palette_hash && !k.palette_min && !k.palette_max);
}
std::string stem(const DuckTextureKey& k) {
    char result[192];
    if (!duck_texture_format_name(&k, result, sizeof(result))) return {};
    return result;
}
bool query_rect(const HdTextureDrawQuery* q, Rect& r) {
    if (!q || !q->vram || q->vram_word_count < kWords || q->depth > 2 ||
        q->u_first > q->u_last || q->v_first > q->v_last ||
        q->page_x >= 1024 || q->page_x % 64 || q->page_y > 256 || q->page_y % 256)
        return false;
    const unsigned ppw = expansion(q->depth);
    /* A page that wraps in X is outside the initial safe subset. */
    if (q->page_x + 256 / ppw > 1024) return false;
    r = {q->page_x + q->u_first / ppw, unsigned(q->page_y) + q->v_first,
         unsigned(q->u_last / ppw - q->u_first / ppw + 1),
         unsigned(q->v_last - q->v_first + 1)};
    return valid_rect(r);
}

uint64_t palette_hash(const HdTextureDrawQuery& q, unsigned first, unsigned last, bool& valid) {
    valid = false;
    if (q.depth == 2) { valid = true; return 0; }
    const unsigned full = q.depth == 0 ? 16 : 256;
    if (first > last || last >= full || q.clut_x >= 1024 || q.clut_y >= 512 || q.clut_x % 16)
        return 0;
    unsigned count = last - first + 1;
    if (first == 0 && last == full - 1) count = std::min(full, 1024u - q.clut_x);
    else if (q.clut_x + last >= 1024) return 0;
    /* Compatibility with the pinned upstream behavior: range length is used,
     * but min does not offset the hashed palette pointer. */
    valid = true;
    return duck_texture_hash_words_le(q.vram + q.clut_y * 1024 + q.clut_x, count);
}

struct Entry { DuckTextureKey key{}; std::string path; bool ambiguous = false; };
struct Upload { Rect rect; uint64_t hash; };
struct RectHash { Rect rect; uint64_t hash; const uint16_t* vram; };
struct PageGroup {
    Rect texels;
    std::unordered_map<uint64_t, std::vector<size_t>> hashes;
};
struct MatchCache {
    HdTextureDrawQuery query;
    int st, status;
    DuckTextureMatch result;
};
struct Image { std::vector<uint8_t> rgba; uint32_t w = 0, h = 0; };
struct Lease { std::shared_ptr<Image> image; };
enum class DecodeStatus { Queued, Loading, Ready, Failed };
struct Decode {
    DecodeStatus status = DecodeStatus::Queued;
    std::string path;
    std::shared_ptr<Image> image;
    uint64_t use = 0;
};
struct Dump { fs::path path; std::shared_ptr<Image> image; };
/* png_write.h's C-compatible CRC table is mutable during initialization.
 * Different pack workers therefore serialize this TU's calls to the writer. */
std::mutex png_writer_mutex;
std::atomic<uint64_t> dump_sequence{static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};

FILE* create_exclusive(const fs::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_BINARY | _O_WRONLY);
    if (fd < 0) { CloseHandle(handle); return nullptr; }
    FILE* file = _fdopen(fd,"wb");
    if (!file) _close(fd);
#else
    const int fd = ::open(path.c_str(),O_WRONLY | O_CREAT | O_EXCL,0600);
    if (fd < 0) return nullptr;
    FILE* file = ::fdopen(fd,"wb");
    if (!file) ::close(fd);
#endif
    return file;
}

bool publish_exclusive(const fs::path& temp, const fs::path& target) {
#ifdef _WIN32
    if (MoveFileExW(temp.c_str(),target.c_str(),0)) return true;
    const DWORD code = GetLastError();
    if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) throw std::runtime_error("cannot publish texture dump PNG");
#else
    if (::link(temp.c_str(),target.c_str()) == 0) return true;
    if (errno != EEXIST) throw std::runtime_error("cannot publish texture dump PNG");
#endif
    return false;
}

struct Worker {
    std::mutex mutex;
    std::condition_variable wake;
    std::unordered_map<uint64_t, Decode> decoded;
    std::deque<uint64_t> decode_queue;
    std::deque<Dump> dump_queue;
    std::thread thread;
    size_t budget = kDefaultBudget, used = 0;
    uint64_t tick = 0;
    bool stop = false;
    std::string dump_error;
    ~Worker() {
        { std::lock_guard<std::mutex> lock(mutex); stop = true; }
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }
    void evict(size_t incoming) {
        while (used + incoming > budget) {
            auto candidate = decoded.end();
            for (auto it = decoded.begin(); it != decoded.end(); ++it)
                if (it->second.status == DecodeStatus::Ready &&
                    (candidate == decoded.end() || it->second.use < candidate->second.use)) candidate = it;
            if (candidate == decoded.end()) break;
            used -= candidate->second.image->rgba.size();
            decoded.erase(candidate);
        }
    }
};

void worker_main(Worker* w) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        uint64_t id = 0; std::string path; Dump dump; size_t budget = 0;
        {
            std::unique_lock<std::mutex> lock(w->mutex);
            w->wake.wait(lock, [&] { return w->stop || !w->decode_queue.empty() || !w->dump_queue.empty(); });
            /* Finish queued dumps on shutdown so the final frame is durable. */
            if (w->stop && w->dump_queue.empty()) return;
            if (!w->stop && !w->decode_queue.empty()) {
                id = w->decode_queue.front(); w->decode_queue.pop_front();
                auto it = w->decoded.find(id);
                if (it == w->decoded.end()) continue;
                it->second.status = DecodeStatus::Loading;
                path = it->second.path; budget = w->budget;
            } else { dump = std::move(w->dump_queue.front()); w->dump_queue.pop_front(); }
        }
        if (id) {
            std::shared_ptr<Image> image;
            try {
                std::ifstream input(fs::u8path(path), std::ios::binary | std::ios::ate);
                const auto length = input ? input.tellg() : std::streampos(-1);
                if (length > 0 && static_cast<uint64_t>(length) <= kMaxEncoded) {
                    std::vector<uint8_t> encoded(static_cast<size_t>(length));
                    input.seekg(0);
                    int width = 0, height = 0, channels = 0;
                    constexpr uint8_t png_signature[] = {137,80,78,71,13,10,26,10};
                    if (input.read(reinterpret_cast<char*>(encoded.data()), length) &&
                        encoded.size() >= sizeof(png_signature) &&
                        !std::memcmp(encoded.data(),png_signature,sizeof(png_signature)) &&
                        stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels) &&
                        width > 0 && height > 0 && width <= int(kMaxImageDimension) && height <= int(kMaxImageDimension) &&
                        uint64_t(width) * height * 4 <= budget) {
                        stbi_uc* pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                                                               &width, &height, &channels, 4);
                        if (pixels) {
                            std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> owned(pixels, &stbi_image_free);
                            image = std::make_shared<Image>(); image->w = width; image->h = height;
                            image->rgba.assign(pixels, pixels + size_t(width) * height * 4);
                        }
                    }
                }
            } catch (...) { image.reset(); }
            std::lock_guard<std::mutex> lock(w->mutex);
            auto it = w->decoded.find(id);
            if (it == w->decoded.end()) continue;
            if (image) w->evict(image->rgba.size());
            if (!image || w->used + image->rgba.size() > w->budget) { it->second.status = DecodeStatus::Failed; continue; }
            w->used += image->rgba.size(); it->second.image = std::move(image);
            it->second.status = DecodeStatus::Ready; it->second.use = ++w->tick;
        } else {
            bool created = false;
            fs::path temporary = dump.path;
            temporary += ".partial-" + std::to_string(++dump_sequence);
            try {
                std::error_code ec;
                fs::create_directories(dump.path.parent_path(), ec);
                if (ec) throw std::runtime_error("cannot create texture dump directory: " + ec.message());
                if (fs::exists(dump.path,ec) && !ec) continue;
                FILE* file = create_exclusive(temporary);
                if (!file) throw std::runtime_error("cannot create temporary texture dump PNG");
                created = true;
                bool success = false;
                try {
                    std::lock_guard<std::mutex> write_lock(png_writer_mutex);
                    success = png_write_rgba(file,dump.image->rgba.data(),dump.image->w,dump.image->h) != 0;
                } catch (...) { std::fclose(file); throw; }
                if (std::fclose(file) != 0 || !success) throw std::runtime_error("cannot write texture dump PNG");
                publish_exclusive(temporary,dump.path);
                fs::remove(temporary,ec);
            } catch (const std::exception& e) {
                if (created) { std::error_code ignored; fs::remove(temporary,ignored); }
                std::lock_guard<std::mutex> lock(w->mutex); w->dump_error = e.what();
                std::fprintf(stderr,"[texture-pack] dump failed: %s\n",e.what());
            } catch (...) {
                if (created) { std::error_code ignored; fs::remove(temporary,ignored); }
                std::lock_guard<std::mutex> lock(w->mutex); w->dump_error = "texture dump allocation failed";
                std::fprintf(stderr,"[texture-pack] dump failed: allocation failed\n");
            }
        }
    }
}
} // namespace

struct DuckTexturePack {
    std::string root, diagnostic;
    fs::path replacements, dumps;
    std::vector<Entry> entries;
    std::array<std::unordered_map<uint64_t, std::vector<size_t>>, 3> uploads_by_hash;
    std::array<std::unordered_map<uint64_t, PageGroup>, 3> page_groups;
    std::unordered_map<std::string, size_t> by_name;
    std::vector<Upload> uploads;
    std::vector<RectHash> rect_cache;
    std::vector<MatchCache> match_cache;
    std::unordered_set<std::string> dumped;
    size_t ignored = 0, ambiguous = 0;
    bool geometry_limit_reported = false, dump_limit_reported = false;
    Worker worker;
};

namespace {
void note(DuckTexturePack& p, const std::string& s) {
    ++p.ignored;
    if (p.diagnostic.size() < 2048) p.diagnostic += (p.diagnostic.empty() ? "" : "; ") + s;
}
void add_entry(DuckTexturePack& p, const DuckTextureKey& key, const fs::path& path) {
    const std::string name = stem(key);
    auto previous = p.by_name.find(name);
    if (previous != p.by_name.end()) {
        auto& entry = p.entries[previous->second];
        if (!entry.ambiguous) { entry.ambiguous = true; ++p.ambiguous; }
        return;
    }
    if (p.entries.size() >= kMaxEntries) throw std::runtime_error("texture replacement entry limit (262144) exceeded");
    p.by_name.emplace(name, p.entries.size());
    if (key.kind == DUCK_TEXTURE_UPLOAD) p.uploads_by_hash[key.depth][key.source_hash].push_back(p.entries.size());
    else {
        const uint64_t geometry = (uint64_t(key.offset_x) << 48) | (uint64_t(key.offset_y) << 32) |
                                  (uint64_t(key.width) << 16) | key.height;
        PageGroup& group = p.page_groups[key.depth][geometry];
        group.texels = {key.offset_x, key.offset_y, key.width, key.height};
        group.hashes[key.source_hash].push_back(p.entries.size());
    }
    p.entries.push_back({key, path.u8string(), false});
}
uint64_t cached_hash(DuckTexturePack& p, const HdTextureDrawQuery& q, Rect r) {
    for (const auto& saved : p.rect_cache)
        if (saved.vram == q.vram && saved.rect.x == r.x && saved.rect.y == r.y && saved.rect.w == r.w && saved.rect.h == r.h) return saved.hash;
    const uint64_t value = duck_texture_hash_rect(q.vram, q.vram_word_count, r.x, r.y, r.w, r.h);
    if (p.rect_cache.size() >= kMaxRectCache) p.rect_cache.erase(p.rect_cache.begin());
    p.rect_cache.push_back({r, value, q.vram});
    return value;
}
std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() > 1 && ((s.front() == '\'' && s.back() == '\'') || (s.front() == '"' && s.back() == '"')))
        return s.substr(1, s.size() - 2);
    const auto comment = s.find(" #");
    return comment == std::string::npos ? s : trim(s.substr(0, comment));
}
void load_config(DuckTexturePack& p, const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    if (fs::file_size(path, ec) > 1024 * 1024 || ec) { note(p, "config.yaml exceeds 1 MiB"); return; }
    std::ifstream file(path); std::string line; bool aliases = false;
    while (std::getline(file, line)) {
        const std::string text = trim(line);
        if (text.empty() || text.front() == '#') continue;
        if (line.size() > 4096) { note(p, "oversized config.yaml line"); continue; }
        const auto colon = text.find(':');
        if (colon == std::string::npos) { note(p, "unsupported config.yaml syntax"); continue; }
        const std::string key = unquote(text.substr(0, colon));
        std::string value = unquote(text.substr(colon + 1));
        const bool indented = line.front() == ' ' || line.front() == '\t';
        if (!indented) { aliases = key == "Aliases"; if (!aliases) note(p, "config.yaml option not applied: " + key); continue; }
        if (!aliases) continue;
        DuckTextureKey parsed{};
        if (!duck_texture_parse_name(key.c_str(), &parsed)) { note(p, "invalid alias key"); continue; }
        if (value == "|" || value == "|-" || value == ">" || value == ">-") {
            if (!std::getline(file, line) || line.find_first_not_of(" \t") < 4) { note(p, "unsupported alias block"); continue; }
            value = trim(line);
        }
        fs::path relative = fs::u8path(value);
        bool unsafe = relative.empty() || relative.is_absolute() || relative.has_root_name();
        for (const auto& part : relative) if (part == "..") unsafe = true;
        if (unsafe || lower(relative.extension().string()) != ".png") { note(p, "unsupported alias path"); continue; }
        const fs::path target = p.replacements / relative;
        if (!fs::is_regular_file(target, ec) || ec) { note(p, "alias target missing"); ec.clear(); continue; }
        const auto old = p.by_name.find(stem(parsed));
        /* A direct canonical filename takes precedence over an alias. */
        if (old == p.by_name.end()) add_entry(p, parsed, target);
    }
}
} // namespace

extern "C" {
int duck_texture_parse_name(const char* filename, DuckTextureKey* out) {
    if (!filename || !out) return 0;
    try {
        std::string text(filename);
        if (text.size() > 192 || text.find_first_of("/\\") != std::string::npos) return 0;
        const auto dot = text.find('.');
        if (dot != std::string::npos) {
            if (lower(text.substr(dot)) != ".png") return 0;
            text.resize(dot);
        }
        std::vector<std::string> parts;
        size_t begin = 0;
        for (size_t at = 0; at <= text.size(); ++at) if (at == text.size() || text[at] == '-') {
            parts.push_back(text.substr(begin, at - begin)); begin = at + 1;
        }
        if (parts.size() != 7 && parts.size() != 10) return 0;
        DuckTextureKey k{};
        if (parts[0] == "texupload") k.kind = DUCK_TEXTURE_UPLOAD;
        else if (parts[0] == "texpage") k.kind = DUCK_TEXTURE_PAGE;
        else return 0;
        std::string mode = parts[1];
        if (mode.size() > 2 && mode.substr(0, 2) == "ST") { k.semitransparent = 1; mode.erase(0, 2); }
        if (mode == "P4") k.depth = 0; else if (mode == "P8") k.depth = 1; else if (mode == "C16") k.depth = 2; else return 0;
        if (parts[2].size() != 16 || !number(parts[2], k.source_hash, 16)) return 0;
        size_t field = 3;
        if (k.depth < 2) {
            if (parts.size() != 10 || parts[3].size() != 16 || !number(parts[3], k.palette_hash, 16)) return 0;
            ++field;
        } else if (parts.size() != 7) return 0;
        if (!dimensions(parts[field++], k.source_width_words, k.source_height) ||
            !number(parts[field++], k.offset_x) || !number(parts[field++], k.offset_y) ||
            !dimensions(parts[field++], k.width, k.height)) return 0;
        if (k.depth < 2) {
            unsigned a = 0, b = 0;
            if (parts[field].empty() || parts[field][0] != 'P' || !number(parts[field].substr(1), a) ||
                !number(parts[field + 1], b) || a > 255 || b > 255) return 0;
            k.palette_min = static_cast<uint8_t>(a); k.palette_max = static_cast<uint8_t>(b);
        }
        if (!valid_key(k)) return 0;
        *out = k; return 1;
    } catch (...) { return 0; }
}

int duck_texture_format_name(const DuckTextureKey* k, char* out, size_t capacity) {
    if (!k || !out || !capacity || !valid_key(*k)) return 0;
    const char* kind = k->kind == DUCK_TEXTURE_UPLOAD ? "texupload" : "texpage";
    const char* mode = k->depth == 0 ? "P4" : k->depth == 1 ? "P8" : "C16";
    const char* st = k->semitransparent ? "ST" : "";
    int count;
    if (k->depth < 2) count = std::snprintf(out, capacity,
        "%s-%s%s-%016llX-%016llX-%ux%u-%u-%u-%ux%u-P%u-%u", kind, st, mode,
        static_cast<unsigned long long>(k->source_hash), static_cast<unsigned long long>(k->palette_hash),
        k->source_width_words, k->source_height, k->offset_x, k->offset_y, k->width, k->height, k->palette_min, k->palette_max);
    else count = std::snprintf(out, capacity, "%s-%s%s-%016llX-%ux%u-%u-%u-%ux%u", kind, st, mode,
        static_cast<unsigned long long>(k->source_hash), k->source_width_words, k->source_height,
        k->offset_x, k->offset_y, k->width, k->height);
    return count >= 0 && static_cast<size_t>(count) < capacity;
}

uint64_t duck_texture_hash_words_le(const uint16_t* words, size_t count) {
    if ((!words && count) || count > kWords) return 0;
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    std::array<uint8_t, 2048> bytes{};
    while (count) {
        const size_t chunk = std::min(count, bytes.size() / 2);
        for (size_t i = 0; i < chunk; ++i) { bytes[2 * i] = words[i] & 255; bytes[2 * i + 1] = words[i] >> 8; }
        XXH3_64bits_update(&state, bytes.data(), chunk * 2);
        words += chunk; count -= chunk;
    }
    return XXH3_64bits_digest(&state);
}
uint64_t duck_texture_hash_rect(const uint16_t* vram, size_t count, uint16_t x, uint16_t y,
                               uint16_t width, uint16_t height) {
    if (!vram || count < kWords || !valid_rect({x, y, width, height})) return 0;
    XXH3_state_t state; XXH3_64bits_reset(&state);
    std::array<uint8_t, 2048> bytes{};
    for (unsigned row = 0; row < height; ++row) {
        const uint16_t* src = vram + (y + row) * 1024 + x;
        for (unsigned i = 0; i < width; ++i) { bytes[2 * i] = src[i] & 255; bytes[2 * i + 1] = src[i] >> 8; }
        XXH3_64bits_update(&state, bytes.data(), size_t(width) * 2);
    }
    return XXH3_64bits_digest(&state);
}

int duck_texture_pack_create(const char* root, DuckTexturePack** out, char* error, size_t capacity) {
    if (out) *out = nullptr;
    if (!out || !root || !root[0]) { error_text(error, capacity, "texture pack root is unset"); return 0; }
    try {
        auto p = std::make_unique<DuckTexturePack>();
        std::error_code ec; fs::path input = fs::absolute(fs::u8path(root), ec).lexically_normal();
        if (ec || (fs::exists(input, ec) && !fs::is_directory(input, ec))) {
            error_text(error, capacity, "texture pack root is not a directory"); return 0;
        }
        if (lower(input.filename().u8string()) == "replacements") { p->replacements = input; input = input.parent_path(); }
        else p->replacements = input / "replacements";
        p->root = input.u8string(); p->dumps = input / "dumps";
        std::vector<fs::path> paths;
        if (fs::is_directory(p->replacements, ec)) {
            size_t scanned = 0;
            for (fs::recursive_directory_iterator it(p->replacements, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (++scanned > kMaxEntries * 4) throw std::runtime_error("texture replacement directory scan limit exceeded");
                if (it.depth() >= 16) { if (it->is_directory(ec)) it.disable_recursion_pending(); note(*p, "replacement nesting exceeds 16 levels"); }
                if (it->is_regular_file(ec)) {
                    const std::string name = it->path().filename().u8string();
                    if (name.rfind("texupload-", 0) != 0 && name.rfind("texpage-", 0) != 0 && name.rfind("vram-write-", 0) != 0) continue;
                    if (lower(it->path().extension().string()) != ".png") { note(*p, "replacement format outside PNG subset"); continue; }
                    if (paths.size() >= kMaxEntries) throw std::runtime_error("texture replacement file limit (262144) exceeded");
                    paths.push_back(it->path());
                }
            }
            if (ec) { error_text(error, capacity, "cannot scan replacement directory: " + ec.message()); return 0; }
        } else if (ec && ec != std::errc::no_such_file_or_directory) {
            error_text(error, capacity, "cannot inspect replacement directory: " + ec.message()); return 0;
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            DuckTextureKey k{};
            if (duck_texture_parse_name(path.filename().u8string().c_str(), &k)) add_entry(*p, k, path);
            else note(*p, "invalid or unsupported replacement filename");
        }
        load_config(*p, input / "config.yaml");
        *out = p.release(); return 1;
    } catch (const std::exception& e) { error_text(error, capacity, e.what()); return 0; }
      catch (...) { error_text(error, capacity, "texture pack allocation failed"); return 0; }
}
void duck_texture_pack_destroy(DuckTexturePack* p) { delete p; }
void duck_texture_pack_get_info(const DuckTexturePack* p, DuckTexturePackInfo* out) {
    if (!out) return;
    *out = {};
    if (p) { out->root = p->root.c_str(); out->diagnostic = p->diagnostic.c_str(); out->replacement_count = p->entries.size();
             out->ambiguous_count = p->ambiguous; out->ignored_count = p->ignored; }
}
void duck_texture_pack_invalidate(DuckTexturePack* p, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!p || !w || !h) return;
    if (w > 1024 || h > 512) { duck_texture_pack_reset_tracking(p); return; }
    const unsigned px = x % 1024, py = y % 512;
    const unsigned w0 = std::min(unsigned(w), 1024 - px), h0 = std::min(unsigned(h), 512 - py);
    const std::array<Rect, 4> regions{{{px,py,w0,h0},{0,py,unsigned(w)-w0,h0},
                                    {px,0,w0,unsigned(h)-h0},{0,0,unsigned(w)-w0,unsigned(h)-h0}}};
    for (const auto& r : regions) if (r.w && r.h) {
        const size_t old_uploads = p->uploads.size();
        p->uploads.erase(std::remove_if(p->uploads.begin(), p->uploads.end(), [&](const Upload& u){ return intersects(u.rect,r); }), p->uploads.end());
        p->rect_cache.erase(std::remove_if(p->rect_cache.begin(), p->rect_cache.end(), [&](const RectHash& v){ return intersects(v.rect,r); }), p->rect_cache.end());
        if (p->uploads.size() != old_uploads) p->match_cache.clear();
        else p->match_cache.erase(std::remove_if(p->match_cache.begin(), p->match_cache.end(), [&](const MatchCache& c) {
            const auto& q = c.query;
            const Rect page{q.page_x,q.page_y,256 / expansion(q.depth),256};
            const Rect clut{q.clut_x,q.clut_y,std::min(q.depth == 0 ? 16u : 256u,1024u-q.clut_x),1};
            return intersects(page,r) || (q.depth < 2 && intersects(clut,r));
        }), p->match_cache.end());
    }
}
void duck_texture_pack_reset_tracking(DuckTexturePack* p) { if (p) { p->uploads.clear(); p->rect_cache.clear(); p->match_cache.clear(); } }
int duck_texture_pack_copy_tracking(DuckTexturePack* destination, const DuckTexturePack* source) {
    if (!destination || !source) return 0;
    try {
        auto uploads = source->uploads;
        destination->uploads.swap(uploads);
        destination->rect_cache.clear();
        destination->match_cache.clear();
        return 1;
    } catch (...) { return 0; }
}
int duck_texture_pack_track_upload(DuckTexturePack* p, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                  const uint16_t* vram, size_t count) {
    if (!p) return 0;
    duck_texture_pack_invalidate(p, x, y, w, h);
    if (!vram || count < kWords || !valid_rect({x,y,w,h})) return 0;
    try {
        if (p->uploads.size() >= kMaxUploads) p->uploads.erase(p->uploads.begin());
        p->uploads.push_back({{x,y,w,h},duck_texture_hash_rect(vram,count,x,y,w,h)});
        p->match_cache.clear(); return 1;
    } catch (...) { return 0; }
}

int duck_texture_pack_match(DuckTexturePack* p, const HdTextureDrawQuery* q, DuckTextureMatch* out) {
    return duck_texture_pack_match_draw(p,q,0,out);
}
int duck_texture_pack_match_draw(DuckTexturePack* p, const HdTextureDrawQuery* q, int st, DuckTextureMatch* out) {
    if (out) *out = {};
    if (!p || !q || !out) return HD_TEXTURE_LOOKUP_ERROR;
    Rect wanted{}; if (!query_rect(q, wanted)) return HD_TEXTURE_LOOKUP_NONE;
    try {
        for (const auto& c : p->match_cache) {
            const auto& a = c.query;
            if (c.st == (st != 0) && a.vram == q->vram && a.page_x == q->page_x && a.page_y == q->page_y &&
                a.depth == q->depth && a.clut_x == q->clut_x && a.clut_y == q->clut_y &&
                a.u_first == q->u_first && a.u_last == q->u_last && a.v_first == q->v_first && a.v_last == q->v_last) {
                *out = c.result; return c.status;
            }
        }
        const unsigned ppw = expansion(q->depth);
        size_t winner = size_t(-1); int origin_u = 0, origin_v = 0, rank = 2;
        bool ambiguous = false;
        const auto consider = [&](size_t index, int u, int v) {
            const Entry& e = p->entries[index]; const auto& k = e.key;
            const int candidate_rank = k.semitransparent == (st != 0) ? 0 : 1;
            if (candidate_rank > rank) return;
            bool valid = false;
            if (palette_hash(*q,k.palette_min,k.palette_max,valid) != k.palette_hash || !valid) return;
            if (candidate_rank < rank) { rank = candidate_rank; winner = size_t(-1); ambiguous = false; }
            if (e.ambiguous || winner != size_t(-1)) ambiguous = true;
            winner = index; origin_u = u; origin_v = v;
        };
        for (const auto& upload : p->uploads) {
            if (!contains(upload.rect,wanted)) continue;
            const auto found = p->uploads_by_hash[q->depth].find(upload.hash);
            if (found == p->uploads_by_hash[q->depth].end()) continue;
            for (const size_t index : found->second) {
                const auto& k = p->entries[index].key;
                if (upload.rect.w != k.source_width_words || upload.rect.h != k.source_height) continue;
                Rect rect{upload.rect.x+k.offset_x/ppw,upload.rect.y+k.offset_y,unsigned(k.width)/ppw,k.height};
                if (contains(rect,wanted)) consider(index,(int(upload.rect.x)-q->page_x)*int(ppw)+k.offset_x,
                                                   int(upload.rect.y)-q->page_y+k.offset_y);
            }
        }
        size_t groups_checked = 0, hashed_bytes = 0; bool over_budget = false;
        for (const auto& item : p->page_groups[q->depth]) {
            const auto& group = item.second; const auto& r = group.texels;
            Rect rect{q->page_x+r.x/ppw,q->page_y+r.y,r.w/ppw,r.h};
            if (!contains(rect,wanted)) continue;
            const bool cached = std::any_of(p->rect_cache.begin(),p->rect_cache.end(),[&](const RectHash& c) {
                return c.vram == q->vram && c.rect.x == rect.x && c.rect.y == rect.y && c.rect.w == rect.w && c.rect.h == rect.h;
            });
            if (!cached) hashed_bytes += size_t(rect.w) * rect.h * 2;
            if (++groups_checked > 4096 || hashed_bytes > 4u*1024u*1024u) { over_budget = true; break; }
            const auto found = group.hashes.find(cached_hash(*p,*q,rect));
            if (found != group.hashes.end()) for (const size_t index : found->second) consider(index,r.x,r.y);
        }
        if (over_budget && !p->geometry_limit_reported) {
            note(*p,"page candidate geometry/hash budget exceeded");
            std::fprintf(stderr,"[texture-pack] page candidate geometry/hash budget exceeded; using native texture\n");
            p->geometry_limit_reported = true;
        }
        int status = over_budget ? HD_TEXTURE_LOOKUP_ERROR : winner == size_t(-1) ? HD_TEXTURE_LOOKUP_NONE :
                     ambiguous ? HD_TEXTURE_LOOKUP_AMBIGUOUS : HD_TEXTURE_LOOKUP_FOUND;
        if (status == HD_TEXTURE_LOOKUP_FOUND) {
            const Entry& e = p->entries[winner]; out->key = e.key; out->entry_id = winner + 1;
            out->replacement_path = e.path.c_str(); out->origin_u = origin_u; out->origin_v = origin_v;
            out->source_width = e.key.width; out->source_height = e.key.height;
        }
        if (p->match_cache.size() >= 256) p->match_cache.erase(p->match_cache.begin());
        p->match_cache.push_back({*q,st != 0,status,*out});
        return status;
    } catch (...) { return HD_TEXTURE_LOOKUP_ERROR; }
}
void duck_texture_pack_set_decode_budget(DuckTexturePack* p, size_t bytes) {
    if (!p) return;
    std::lock_guard<std::mutex> lock(p->worker.mutex);
    p->worker.budget = bytes; p->worker.evict(0);
}
int duck_texture_pack_request_decode(DuckTexturePack* p, uint64_t id) {
    if (!p || !id || id > p->entries.size() || p->entries[id-1].ambiguous) return HD_TEXTURE_LOOKUP_ERROR;
    try {
        Worker& w = p->worker; std::lock_guard<std::mutex> lock(w.mutex);
        auto it = w.decoded.find(id);
        if (it != w.decoded.end()) {
            it->second.use = ++w.tick;
            return it->second.status == DecodeStatus::Ready ? HD_TEXTURE_LOOKUP_FOUND :
                   it->second.status == DecodeStatus::Failed ? HD_TEXTURE_LOOKUP_ERROR : HD_TEXTURE_LOOKUP_NONE;
        }
        if (!w.budget) return HD_TEXTURE_LOOKUP_ERROR;
        if (w.decode_queue.size() >= kMaxDecodeQueue) return HD_TEXTURE_LOOKUP_NONE;
        if (w.decoded.size() >= 512) {
            auto victim = w.decoded.end();
            for (auto item = w.decoded.begin(); item != w.decoded.end(); ++item)
                if ((item->second.status == DecodeStatus::Ready || item->second.status == DecodeStatus::Failed) &&
                    (victim == w.decoded.end() || item->second.use < victim->second.use)) victim = item;
            if (victim == w.decoded.end()) return HD_TEXTURE_LOOKUP_NONE;
            if (victim->second.image) w.used -= victim->second.image->rgba.size();
            w.decoded.erase(victim);
        }
        if (!w.thread.joinable()) w.thread = std::thread(worker_main,&w);
        w.decoded[id].path = p->entries[id-1].path; w.decode_queue.push_back(id);
        w.wake.notify_one(); return HD_TEXTURE_LOOKUP_NONE;
    } catch (...) { return HD_TEXTURE_LOOKUP_ERROR; }
}
int duck_texture_pack_acquire_decoded(DuckTexturePack* p, uint64_t id, DuckTexturePixels* out) {
    if (out) *out = {};
    if (!p || !out) return HD_TEXTURE_LOOKUP_ERROR;
    try {
        Worker& w = p->worker; std::lock_guard<std::mutex> lock(w.mutex); const auto it = w.decoded.find(id);
        if (it == w.decoded.end()) return HD_TEXTURE_LOOKUP_NONE;
        if (it->second.status != DecodeStatus::Ready) return it->second.status == DecodeStatus::Failed ? HD_TEXTURE_LOOKUP_ERROR : HD_TEXTURE_LOOKUP_NONE;
        auto* lease = new Lease{it->second.image}; it->second.use = ++w.tick;
        out->lease = lease; out->rgba = lease->image->rgba.data(); out->width = lease->image->w;
        out->height = lease->image->h; out->stride = out->width * 4; return HD_TEXTURE_LOOKUP_FOUND;
    } catch (...) { return HD_TEXTURE_LOOKUP_ERROR; }
}
void duck_texture_pixels_release(DuckTexturePixels* p) { if (p) { delete static_cast<Lease*>(p->lease); *p = {}; } }

int duck_texture_pack_dump_draw(DuckTexturePack* p, const HdTextureDrawQuery* q, int st, char* error, size_t capacity) {
    if (!p || !q) return HD_TEXTURE_LOOKUP_ERROR;
    Rect rect{}; if (!query_rect(q,rect)) return HD_TEXTURE_LOOKUP_NONE;
    try {
        Worker& w = p->worker;
        { std::lock_guard<std::mutex> lock(w.mutex);
          if (!w.dump_error.empty()) { error_text(error,capacity,w.dump_error); return HD_TEXTURE_LOOKUP_ERROR; }
          if (w.dump_queue.size() >= kMaxDumpQueue) return HD_TEXTURE_LOOKUP_NONE; }
        const unsigned ppw = expansion(q->depth); const Upload* upload = nullptr;
        for (const auto& u : p->uploads) if (contains(u.rect,rect)) { if (upload) { upload = nullptr; break; } upload = &u; }
        DuckTextureKey k{}; k.kind = upload ? DUCK_TEXTURE_UPLOAD : DUCK_TEXTURE_PAGE;
        k.depth = q->depth; k.semitransparent = st != 0;
        k.source_hash = upload ? upload->hash : cached_hash(*p,*q,rect);
        k.source_width_words = upload ? upload->rect.w : 256 / ppw; k.source_height = upload ? upload->rect.h : 256;
        k.offset_x = (rect.x - (upload ? upload->rect.x : q->page_x)) * ppw;
        k.offset_y = rect.y - (upload ? upload->rect.y : q->page_y); k.width = rect.w * ppw; k.height = rect.h;
        k.palette_max = q->depth == 0 ? 15 : q->depth == 1 ? 255 : 0;
        bool palette_valid = false; k.palette_hash = palette_hash(*q,0,k.palette_max,palette_valid);
        if (!palette_valid) return HD_TEXTURE_LOOKUP_NONE;
        const std::string name = stem(k);
        if (name.empty()) return HD_TEXTURE_LOOKUP_NONE;
        if (p->dumped.count(name)) return HD_TEXTURE_LOOKUP_NONE;
        if (p->dumped.size() >= kMaxDumps) {
            if (!p->dump_limit_reported) {
                note(*p,"dump session limit (8192) reached; reset dump tracking to continue");
                std::fprintf(stderr,"[texture-pack] dump session limit (8192) reached; reset dump tracking to continue\n");
                p->dump_limit_reported = true;
            }
            error_text(error,capacity,"texture dump session limit (8192) reached");
            return HD_TEXTURE_LOOKUP_ERROR;
        }
        auto image = std::make_shared<Image>(); image->w = k.width; image->h = k.height;
        image->rgba.resize(size_t(image->w) * image->h * 4);
        for (unsigned y = 0; y < image->h; ++y) for (unsigned x = 0; x < image->w; ++x) {
            uint16_t color = q->vram[(rect.y+y)*1024 + rect.x + x/ppw];
            if (q->depth < 2) {
                const unsigned index = q->depth == 0 ? ((color >> ((x%4)*4)) & 15) : ((color >> ((x%2)*8)) & 255);
                color = q->clut_x + index < 1024 ? q->vram[q->clut_y*1024+q->clut_x+index] : 0;
            }
            uint8_t* pixel = image->rgba.data() + (size_t(y)*image->w+x)*4;
            for (unsigned c = 0; c < 3; ++c) {
                const unsigned value = (color >> (c*5)) & 31;
                pixel[c] = static_cast<uint8_t>((value * 255u + 15u) / 31u);
            }
            pixel[3] = color == 0 ? 0 : (st && (color & 0x8000)) ? 128 : 255;
        }
        { std::lock_guard<std::mutex> lock(w.mutex);
          if (w.dump_queue.size() >= kMaxDumpQueue) return HD_TEXTURE_LOOKUP_NONE;
          if (!w.thread.joinable()) w.thread = std::thread(worker_main,&w);
          w.dump_queue.push_back({p->dumps / (name + ".png"), std::move(image)});
          p->dumped.insert(name);
          w.wake.notify_one(); }
        return HD_TEXTURE_LOOKUP_FOUND;
    } catch (const std::exception& e) { error_text(error,capacity,e.what()); return HD_TEXTURE_LOOKUP_ERROR; }
      catch (...) { error_text(error,capacity,"texture dump allocation failed"); return HD_TEXTURE_LOOKUP_ERROR; }
}
void duck_texture_pack_reset_dump(DuckTexturePack* p) {
    if (!p) return;
    p->dumped.clear();
    p->dump_limit_reported = false;
    std::lock_guard<std::mutex> lock(p->worker.mutex);
    p->worker.dump_error.clear();
}
} // extern C
