#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "duckstation_texture_pack.h"
#include "../src/png_write.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",what); ++failures; }
}
using Pack = std::unique_ptr<DuckTexturePack,decltype(&duck_texture_pack_destroy)>;
Pack create(const fs::path& root) {
    DuckTexturePack* pack = nullptr; char error[512]{};
    check(duck_texture_pack_create(root.u8string().c_str(),&pack,error,sizeof(error)) == 1,error);
    return Pack(pack,&duck_texture_pack_destroy);
}
std::vector<uint16_t> words(size_t n) {
    std::vector<uint16_t> result(n);
    for (size_t i = 0; i < n; ++i) result[i] = static_cast<uint16_t>((i*7919)^0xa51c);
    return result;
}
void place(std::vector<uint16_t>& vram, unsigned x, unsigned y, unsigned w,
           const std::vector<uint16_t>& source) {
    for (size_t i = 0; i < source.size(); ++i) vram[(y+i/w)*1024+x+i%w] = source[i];
}
HdTextureDrawQuery query(std::vector<uint16_t>& vram, uint16_t page_x, uint16_t page_y,
                        uint8_t depth, uint8_t u0, uint8_t u1, uint8_t v0, uint8_t v1,
                        uint16_t clut_x = 512, uint16_t clut_y = 400) {
    return {page_x,page_y,depth,u0,u1,v0,v1,clut_x,clut_y,vram.data(),vram.size()};
}
void png(const fs::path& path, unsigned w = 4, unsigned h = 4) {
    fs::create_directories(path.parent_path());
    std::vector<uint8_t> pixels(size_t(w)*h*4);
    constexpr uint8_t alpha[] = {0,127,128,242,243,255};
    for (size_t i = 0; i < size_t(w)*h; ++i) {
        pixels[i*4] = 23; pixels[i*4+1] = 57; pixels[i*4+2] = 91; pixels[i*4+3] = alpha[i%6];
    }
#ifdef _WIN32
    FILE* file = _wfopen(path.c_str(),L"wb");
#else
    FILE* file = std::fopen(path.c_str(),"wb");
#endif
    check(file != nullptr,"open original test PNG");
    if (file) { check(png_write_rgba(file,pixels.data(),w,h) == 1,"encode original test PNG"); std::fclose(file); }
}
DuckTexturePixels decoded(DuckTexturePack* pack, uint64_t id) {
    DuckTexturePixels pixels{};
    check(duck_texture_pack_request_decode(pack,id) >= 0,"queue PNG decode");
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    int status = HD_TEXTURE_LOOKUP_NONE;
    do {
        status = duck_texture_pack_acquire_decoded(pack,id,&pixels);
        if (status != HD_TEXTURE_LOOKUP_NONE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < end);
    check(status == HD_TEXTURE_LOOKUP_FOUND,"background PNG decode becomes ready");
    return pixels;
}
bool decode_fails(DuckTexturePack* pack, uint64_t id) {
    int status = duck_texture_pack_request_decode(pack,id);
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (status == HD_TEXTURE_LOOKUP_NONE && std::chrono::steady_clock::now() < end) {
        DuckTexturePixels pixels{};
        status = duck_texture_pack_acquire_decoded(pack,id,&pixels);
        duck_texture_pixels_release(&pixels);
        if (status == HD_TEXTURE_LOOKUP_NONE) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return status == HD_TEXTURE_LOOKUP_ERROR;
}

void fixed_hash_vectors() {
    /* Independently produced by Python xxhash 3.6.0's native wheel. These
     * literal expectations cover XXH3's short/medium/long-input boundaries. */
    const std::pair<size_t,uint64_t> vectors[] = {
        {0,0x2d06800538d394c2ULL},{1,0xc5779b2f56fbe2e1ULL},{2,0x0edb68960f55aef6ULL},
        {3,0xfce9d54aec3f9d32ULL},{4,0x3631d2a04ec85311ULL},{8,0x000ca76cb9b329dcULL},
        {9,0xab6a9091cf532d8aULL},{16,0x9cbcdea9305d7528ULL},{17,0x2f133c3e193cbe48ULL},
        {32,0x3e4a087da5cd5990ULL},{64,0xc3e3ef9ad7f08dacULL},{65,0x740c4e7d1690d606ULL},
        {120,0x5942d561231419b2ULL},{121,0x152cb5857c4535f3ULL},{128,0x351cd56a8fe50e66ULL},
        {129,0x38a45de7ae84906eULL},{256,0x0005d0b83a005f4eULL},{8192,0xc68a00dcca652920ULL}
    };
    for (const auto& v : vectors) {
        const auto input = words(v.first);
        check(duck_texture_hash_words_le(input.data(),input.size()) == v.second,"fixed independent LE16 XXH3 vector");
    }
    std::vector<uint16_t> vram(1024*512,0xf00d);
    place(vram,101,39,2,words(8));
    check(duck_texture_hash_rect(vram.data(),vram.size(),101,39,2,4) == 0x000ca76cb9b329dcULL,
          "strided native rectangle excludes unrelated VRAM words");
    check(duck_texture_hash_rect(vram.data(),vram.size(),1023,0,2,1) == 0,"wrapped hash rectangle rejected");
}

void names() {
    DuckTextureKey key{};
    check(duck_texture_parse_name("texupload-P4-0123456789abcdef-FEDCBA9876543210-64x256-0-192-64x64-P0-14.png",&key) == 1,
          "literal upstream documented upload filename");
    check(key.source_hash == 0x0123456789abcdefULL && key.palette_hash == 0xfedcba9876543210ULL &&
          key.source_width_words == 64 && key.offset_y == 192 && key.width == 64 && key.palette_max == 14,
          "filename dimensions distinguish source words from expanded texels");
    check(duck_texture_parse_name("texpage-STC16-0123456789ABCDEF-256x256-8-16-32x64.PNG",&key) == 1 &&
          key.kind == DUCK_TEXTURE_PAGE && key.semitransparent && key.depth == 2 && !key.palette_hash,
          "direct ST filename omits palette fields");
    const char* invalid[] = {
        "texpage-P4-12345678-0123456789ABCDEF-64x256-0-0-4x1-P0-15.png",
        "texpage-P4-0123456789ABCDEF-0123456789ABCDEF-64x256-0-0-3x1-P0-15.png",
        "texpage-P4-0123456789ABCDEF-0123456789ABCDEF-64x256-0-0-4x1-P0-16.png",
        "texpage-P8-0123456789ABCDEF-0123456789ABCDEF-128x256-0-0-4x1-P15-0.png",
        "texupload-C16-0123456789ABCDEF-0x256-0-0-4x1.png",
        "texupload-C16-0123456789ABCDEF-65536x1-0-0-4x1.png",
        "texpage-C16-0123456789ABCDEF-256x256-255-0-2x1.png",
        "texpage-C16-0123456789ABCDEF-256x256-0-0-4x1.png.old",
        "../texpage-C16-0123456789ABCDEF-256x256-0-0-4x1.png"
    };
    for (const char* name : invalid) check(!duck_texture_parse_name(name,&key),"targeted malformed/unsupported name rejected");
}

void page_and_decode(const fs::path& root) {
    const char* name = "texpage-P4-3631D2A04EC85311-9CBCDEA9305D7528-64x256-4-2-8x2-P0-15.png";
    png(root/"replacements"/"nested"/name,16,4);
    std::vector<uint16_t> vram(1024*512);
    place(vram,65,258,2,words(4)); place(vram,512,400,16,words(16));
    auto pack = create(root); auto q = query(vram,64,256,0,5,10,2,3);
    DuckTextureMatch match{};
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"fixed subpage fixture matches");
    check(match.origin_u == 4 && match.origin_v == 2 && match.source_width == 8 && match.source_height == 2,
          "crop extent and origin are page-space texels");
    auto pixels = decoded(pack.get(),match.entry_id);
    check(pixels.width == 16 && pixels.height == 4,"arbitrary PNG scaling uses image dimensions");
    constexpr uint8_t alpha[] = {0,127,128,242,243,255};
    if (pixels.rgba) for (unsigned i = 0; i < 6; ++i)
        check(pixels.rgba[i*4+3] == alpha[i],"decode retains raw alpha boundary bytes");
    const uint16_t old = vram[258*1024+65];
    vram[258*1024+65] ^= 1; duck_texture_pack_invalidate(pack.get(),65,258,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"write invalidates positive page cache");
    vram[258*1024+65] = old; duck_texture_pack_invalidate(pack.get(),65,258,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"write invalidates negative page cache");
    vram[400*1024+512] ^= 1; duck_texture_pack_invalidate(pack.get(),512,400,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"CLUT write invalidates cached match");
    pack.reset();
    check(pixels.rgba && pixels.rgba[0] == 23,"pixel lease outlives pack and decode worker");
    duck_texture_pixels_release(&pixels);
}

void upload(const fs::path& root) {
    png(root/"replacements"/"texupload-C16-000CA76CB9B329DC-4x2-2-0-2x2.png",4,8);
    std::vector<uint16_t> vram(1024*512); place(vram,66,257,4,words(8));
    auto pack = create(root); auto q = query(vram,64,256,2,4,5,1,2);
    DuckTextureMatch match{};
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"upload names require original source identity");
    check(duck_texture_pack_track_upload(pack.get(),66,257,4,2,vram.data(),vram.size()),"track post-write native upload");
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND && match.origin_u == 4 && match.origin_v == 1,
          "upload crop has correct source origin");
    duck_texture_pack_invalidate(pack.get(),66,257,1,1); /* outside replacement crop */
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"partial source overwrite drops entire upload identity");
    duck_texture_pack_track_upload(pack.get(),66,257,4,2,vram.data(),vram.size());
    duck_texture_pack_reset_tracking(pack.get());
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"savestate/reset clears upload identity");
    check(!duck_texture_pack_track_upload(pack.get(),1023,0,2,1,vram.data(),vram.size()),"wrapped upload explicitly falls back");
}

void reload_tracking(const fs::path& root) {
    std::vector<uint16_t> vram(1024*512); place(vram,66,257,4,words(8));
    auto old = create(root);
    check(duck_texture_pack_track_upload(old.get(),66,257,4,2,vram.data(),vram.size()),"capture upload before replacement file exists");
    png(root/"replacements"/"texupload-C16-000CA76CB9B329DC-4x2-2-0-2x2.png");
    auto refreshed = create(root); auto q = query(vram,64,256,2,4,5,1,2); DuckTextureMatch match{};
    check(duck_texture_pack_match(refreshed.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"newly indexed replacement initially has no upload identity");
    check(duck_texture_pack_copy_tracking(refreshed.get(),old.get()),"same-session reload copies upload tracking");
    check(duck_texture_pack_match(refreshed.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"copied tracking makes newly indexed replacement immediately match");
    duck_texture_pack_invalidate(refreshed.get(),66,257,1,1);
    check(duck_texture_pack_match(refreshed.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"destination tracking invalidates independently");
    auto second = create(root); duck_texture_pack_copy_tracking(second.get(),old.get());
    check(duck_texture_pack_match(second.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"destination invalidation leaves original tracker intact");
}

void palettes(const fs::path& root) {
    std::vector<uint16_t> vram(1024*512); place(vram,128,0,1,words(2));
    place(vram,1008,400,16,words(16));
    const char* edge = "texpage-P8-0EDB68960F55AEF6-9CBCDEA9305D7528-128x256-0-0-2x2-P0-255.png";
    png(root/"edge"/"replacements"/edge);
    auto pack = create(root/"edge"); auto q = query(vram,128,0,1,0,1,0,1,1008,400); DuckTextureMatch match{};
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"full P8 edge CLUT hashes remaining 16 words");
    vram[400*1024] = 42; duck_texture_pack_invalidate(pack.get(),0,400,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"full P8 CLUT hash does not wrap into row start");
    const char* partial = "texpage-P8-0EDB68960F55AEF6-3631D2A04EC85311-128x256-0-0-2x2-P8-11.png";
    png(root/"partial"/"replacements"/partial);
    place(vram,512,400,16,words(16)); q.clut_x = 512;
    pack = create(root/"partial");
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"reduced P8 min8-max11 hashes prefix4 per upstream pin");
    vram[400*1024+520] ^= 1; duck_texture_pack_invalidate(pack.get(),520,400,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"pinned prefix quirk ignores palette8 despite declared minimum8");
    vram[400*1024+512] ^= 1; duck_texture_pack_invalidate(pack.get(),512,400,1,1);
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"reduced P8 prefix change stops replacement");
    q.clut_x = 1023;
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"misaligned/edge reduced CLUT rejected");
}

void st_alias_duplicates(const fs::path& root) {
    const char* ordinary = "texpage-C16-000CA76CB9B329DC-256x256-0-0-4x2.png";
    const char* st = "texpage-STC16-000CA76CB9B329DC-256x256-0-0-4x2.png";
    png(root/"paired"/"replacements"/ordinary); png(root/"paired"/"replacements"/st);
    std::vector<uint16_t> vram(1024*512); place(vram,64,0,4,words(8)); auto q = query(vram,64,0,2,0,3,0,1);
    auto pack = create(root/"paired"); DuckTextureMatch match{};
    check(duck_texture_pack_match_draw(pack.get(),&q,0,&match) == HD_TEXTURE_LOOKUP_FOUND && !match.key.semitransparent,
          "ordinary primitive prefers ordinary filename");
    check(duck_texture_pack_match_draw(pack.get(),&q,1,&match) == HD_TEXTURE_LOOKUP_FOUND && match.key.semitransparent,
          "semitransparent primitive prefers ST filename");
    png(root/"alias"/"replacements"/"art"/"sample.png");
    fs::create_directories(root/"alias");
    std::ofstream config(root/"alias"/"config.yaml");
    config << "MaxVRAMWriteCoalesceWidth: 1\nAliases:\n  " << std::string(ordinary).substr(0,std::strlen(ordinary)-4)
           << ": |\n    art/sample.png\n"; config.close();
    pack = create(root/"alias");
    check(duck_texture_pack_match_draw(pack.get(),&q,1,&match) == HD_TEXTURE_LOOKUP_FOUND && !match.key.semitransparent,
          "bounded literal alias and opposite-convention fallback");
    DuckTexturePackInfo info{}; duck_texture_pack_get_info(pack.get(),&info);
    check(info.ignored_count && std::strstr(info.diagnostic,"MaxVRAMWriteCoalesceWidth"),"unsupported config option explicitly diagnosed");
    png(root/"duplicate"/"replacements"/"a"/ordinary); png(root/"duplicate"/"replacements"/"b"/ordinary);
    pack = create(root/"duplicate");
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_AMBIGUOUS,"recursive duplicate keys fall back deterministically");
    q.u_first = 254; q.u_last = 1;
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_NONE,"wrapped UV interval falls back");
}

void failed_payloads(const fs::path& root) {
    const char* name = "texpage-C16-000CA76CB9B329DC-256x256-0-0-4x2.png";
    png(root/"budget"/"replacements"/name);
    std::vector<uint16_t> vram(1024*512); place(vram,64,0,4,words(8)); auto q = query(vram,64,0,2,0,3,0,1);
    auto pack = create(root/"budget"); DuckTextureMatch match{};
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"over-budget payload identity remains valid metadata");
    duck_texture_pack_set_decode_budget(pack.get(),8);
    check(decode_fails(pack.get(),match.entry_id),"over-budget decoded image fails before pixel allocation");
    fs::create_directories(root/"malformed"/"replacements");
    std::ofstream invalid(root/"malformed"/"replacements"/name,std::ios::binary); invalid << "not PNG"; invalid.close();
    pack = create(root/"malformed");
    check(duck_texture_pack_match(pack.get(),&q,&match) == HD_TEXTURE_LOOKUP_FOUND,"malformed PNG identity can be indexed without render-thread decode");
    check(decode_fails(pack.get(),match.entry_id),"malformed PNG fails asynchronously for native fallback");
    const fs::path oversized = root/"dimension"/"replacements"/name;
    png(oversized);
    std::ifstream file(oversized,std::ios::binary); std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{}); file.close();
    /* Forge a valid IHDR width above the decode dimension bound. Keeping the
     * tiny original IDAT makes the fixture inexpensive and verifies rejection
     * using image metadata before an enormous allocation could happen. */
    bytes[16] = 0; bytes[17] = 0; bytes[18] = 0x23; bytes[19] = 0x28; /* 9000 */
    const uint32_t crc = png_crc_update(0xffffffffu,bytes.data()+12,17)^0xffffffffu;
    for (unsigned i = 0; i < 4; ++i) bytes[29+i] = static_cast<uint8_t>(crc >> (24-8*i));
    std::ofstream changed(oversized,std::ios::binary|std::ios::trunc);
    changed.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); changed.close();
    pack = create(root/"dimension"); duck_texture_pack_match(pack.get(),&q,&match);
    check(decode_fails(pack.get(),match.entry_id),"oversized PNG IHDR fails within native fallback bounds");
}

void dumping(const fs::path& root) {
    auto pack = create(root); std::vector<uint16_t> vram(1024*512);
    vram[0] = 0x3210; vram[400*1024+1] = 0x001f; vram[400*1024+2] = 0x8000; vram[400*1024+3] = 0xffff;
    auto q = query(vram,0,0,0,0,3,0,0,0,400); char error[512]{};
    check(duck_texture_pack_dump_draw(pack.get(),&q,1,error,sizeof(error)) == HD_TEXTURE_LOOKUP_FOUND,"queue native ST page dump");
    check(duck_texture_pack_dump_draw(pack.get(),&q,1,error,sizeof(error)) == HD_TEXTURE_LOOKUP_NONE,"dump session deduplicates without filesystem access");
    pack.reset(); /* required to finish queued background writes */
    const fs::path file = root/"dumps"/"texpage-STP4-A92BF769517E0361-59786C612FB45363-64x256-0-0-4x1-P0-15.png";
    check(fs::is_regular_file(file),"fixed independent hash filename published at worker shutdown");
    int width = 0, height = 0, channels = 0;
    std::ifstream encoded_file(file,std::ios::binary);
    std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(encoded_file)),{});
    auto* image = encoded.empty() ? nullptr : stbi_load_from_memory(encoded.data(),static_cast<int>(encoded.size()),&width,&height,&channels,4);
    constexpr uint8_t expected[] = {0,0,0,0,255,0,0,255,0,0,0,128,255,255,255,128};
    check(image && width == 4 && height == 1 && !std::memcmp(image,expected,sizeof(expected)),"dump decodes native indices/CLUT and ST alpha");
    if (image) stbi_image_free(image);
    std::ofstream owner(file,std::ios::binary|std::ios::trunc); owner << "owner edit"; owner.close();
    pack = create(root); duck_texture_pack_dump_draw(pack.get(),&q,1,error,sizeof(error)); pack.reset();
    std::ifstream edited(file,std::ios::binary); std::string payload((std::istreambuf_iterator<char>(edited)),{});
    check(payload == "owner edit","existing owner dump is never overwritten");
    pack = create(root/"upload");
    duck_texture_pack_track_upload(pack.get(),0,0,1,1,vram.data(),vram.size());
    check(duck_texture_pack_dump_draw(pack.get(),&q,0,error,sizeof(error)) == HD_TEXTURE_LOOKUP_FOUND,"tracked source emits texupload dump");
    pack.reset();
    check(fs::is_regular_file(root/"upload"/"dumps"/"texupload-P4-A92BF769517E0361-59786C612FB45363-1x1-0-0-4x1-P0-15.png"),
          "upload dump retains original source dimensions");
    /* PNG fixtures remain independent of the VRAM pointer used by replacement
     * matching, so dumping never reads already-replaced pixels. */
}
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path()/fs::u8path("psx-duck-texture-\xCE\xA9-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fixed_hash_vectors(); names(); page_and_decode(root/"page"); upload(root/"upload"); reload_tracking(root/"reload");
        palettes(root/"palettes"); st_alias_duplicates(root/"keys"); failed_payloads(root/"invalid"); dumping(root/"dump");
        /* Every artifact is created beneath this fresh, explicitly named test
         * root. No user-provided paths participate in recursive cleanup. */
        fs::remove_all(root);
    } catch (const std::exception& e) { check(false,e.what()); }
    if (failures) { std::fprintf(stderr,"test_duckstation_texture_pack: %d failure(s)\n",failures); return 1; }
    std::puts("PASS: DuckStation format, independent hashes, native matching, decode and dumps");
    return 0;
}
