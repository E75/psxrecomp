/*
 * iso_reader_c.cpp — C wrapper for the C++ ISOReader class
 *
 * Provides iso_open() / iso_read_sector() / iso_close() for use by cdrom.c
 */

#include "iso_reader.h"
#include "mod_runtime.h"
#include "audio_native.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace {

/* One ISO9660 directory record reduced to what the CD-DA track table needs. */
struct IsoDirEntry {
    std::string name;      /* version suffix (";1") stripped */
    uint32_t    lba = 0;
    uint32_t    size = 0;
    bool        is_dir = false;
};

bool iso_name_is(const std::string& a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i]))
            return false;
    return true;
}

bool iso_name_ends_with(const std::string& a, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (a.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = a[a.size() - n + i];
        if (std::toupper((unsigned char)c) != std::toupper((unsigned char)suffix[i]))
            return false;
    }
    return true;
}

void iso_list_dir(PS1::ISOReader* reader, uint32_t lba, uint32_t size,
                  std::vector<IsoDirEntry>& out) {
    uint8_t sector[2048];
    const uint32_t sectors = (size + 2047u) / 2048u;
    for (uint32_t s = 0; s < sectors; ++s) {
        if (!reader->ReadSector(lba + s, sector)) return;
        uint32_t off = 0;
        while (off + 33u <= 2048u) {
            const uint8_t len = sector[off];
            if (len == 0) break;                       /* rest of sector is padding */
            if (off + len > 2048u || len < 33u) break;
            const uint8_t name_len = sector[off + 32];
            if (name_len > 0 && off + 33u + name_len <= 2048u) {
                std::string name((const char*)(sector + off + 33), name_len);
                if (name != "." && name != "..") {
                    size_t semi = name.find(';');
                    if (semi != std::string::npos) name.resize(semi);
                    IsoDirEntry e;
                    e.name = name;
                    e.lba  = (uint32_t)sector[off + 2] | ((uint32_t)sector[off + 3] << 8) |
                             ((uint32_t)sector[off + 4] << 16) | ((uint32_t)sector[off + 5] << 24);
                    e.size = (uint32_t)sector[off + 10] | ((uint32_t)sector[off + 11] << 8) |
                             ((uint32_t)sector[off + 12] << 16) | ((uint32_t)sector[off + 13] << 24);
                    e.is_dir = (sector[off + 25] & 0x02u) != 0;
                    out.push_back(e);
                }
            }
            off += len;
        }
    }
}

const IsoDirEntry* iso_find(const std::vector<IsoDirEntry>& v, const char* name) {
    for (const auto& e : v) if (iso_name_is(e.name, name)) return &e;
    return nullptr;
}

/* Build the native CD-DA track table from the mounted image once.
 *
 * The eight `SD/DA/<name>.DA` entries of this title ARE the disc's Red Book music
 * tracks (docs/ASSET_PIPELINE.md phase 2i): entry LBA = track start, size/2048
 * = track length in CD frames, 150-sector pre-gap before each next entry.
 * They are only used when the image carries no audio tracks of its own — a
 * full multi-track cue stays authoritative. Returns the native playlist's
 * track count (0 = the CD model behaves exactly as before).
 */
int iso_cdda_native_scan(void* handle) {
    static void* scanned_handle = nullptr;
    if (scanned_handle == handle) return audio_native_cdda_track_count();
    if (!handle) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    if (!reader->IsOpen()) return 0;
    scanned_handle = handle;

    int disc_audio = 0;
    for (int t = 1; t <= reader->TrackCount(); ++t)
        if (reader->TrackIsAudio(t)) ++disc_audio;
    audio_native_cdda_note_disc_audio(disc_audio);

    /* The disc's only streamed XA lives inside the one movie; its decoded WAV
     * is an independent stream from the CD-DA tracks above. */
    PS1::ISOFileEntry str;
    if (reader->FindFile("MOVIE/WE2002.STR", str))
        audio_native_xa_configure(str.lba, str.size / 2048u, "MOVIE/WE2002.STR");

    if (disc_audio > 0) { audio_native_state_flush(); return 0; }

    std::vector<IsoDirEntry> root, sd, da;
    const PS1::RootDirectoryInfo r = reader->GetRootDirectory();
    iso_list_dir(reader, r.lba, r.size, root);
    if (const IsoDirEntry* d = iso_find(root, "SD")) iso_list_dir(reader, d->lba, d->size, sd);
    if (const IsoDirEntry* d = iso_find(sd, "DA")) iso_list_dir(reader, d->lba, d->size, da);

    std::vector<AudioNativeCddaTrack> tracks;
    for (const auto& e : da) {
        if (e.is_dir || !iso_name_ends_with(e.name, ".DA")) continue;
        AudioNativeCddaTrack t;
        std::memset(&t, 0, sizeof(t));
        t.start_lba = e.lba;
        t.sectors = e.size / 2048u;         /* ISO size == CD frames for these */
        std::snprintf(t.iso_path, sizeof(t.iso_path), "SD/DA/%s", e.name.c_str());
        tracks.push_back(t);
    }
    std::sort(tracks.begin(), tracks.end(),
              [](const AudioNativeCddaTrack& a, const AudioNativeCddaTrack& b) {
                  return a.start_lba < b.start_lba;
              });
    audio_native_cdda_configure(tracks.empty() ? nullptr : tracks.data(),
                                (int)tracks.size());
    /* One state file per session: which source each stream resolved to. */
    audio_native_state_flush();
    return audio_native_cdda_track_count();
}

} /* namespace */

extern "C" {

void* iso_open(const char* path) {
    auto* reader = new PS1::ISOReader();
    if (!reader->Open(path)) {
        delete reader;
        return nullptr;
    }
    return reader;
}

int iso_read_sector(void* handle, uint32_t lba, uint8_t* buffer, int size) {
    if (!handle || !buffer || size < 2048) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    if (!mod_runtime_read_disc_extent(lba, 0, buffer, size) && !reader->ReadSector(lba, buffer)) return 0;
    mod_runtime_patch_disc_sector(lba, 0, buffer, 2048);
    return 1;
}

int iso_read_raw_sector(void* handle, uint32_t lba, uint8_t* buffer, int size) {
    if (!handle || !buffer || size < 2352) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    if (!mod_runtime_read_disc_extent(lba, 1, buffer, size) && !reader->ReadRawSector(lba, buffer)) return 0;
    mod_runtime_patch_disc_sector(lba, 1, buffer, 2352);
    return 1;
}

int iso_read_subq(void* handle, uint32_t lba, uint8_t* buffer, int size,
                  int* valid) {
    if (!handle || !buffer || size < 12 || !valid) return 0;
    const auto* reader=static_cast<PS1::ISOReader*>(handle);
    const uint32_t start=mod_runtime_disc_extent_start();
    if (start && lba>=start && lba<mod_runtime_disc_sector_count(start)) {
        const auto bcd=[](uint32_t v) {return uint8_t((v/10)*16+v%10);};
        std::memset(buffer,0,12);buffer[0]=0x41;
        buffer[1]=bcd(reader->TrackCount()+1);buffer[2]=1;
        const uint32_t rel=lba-start,abs=lba+150;
        buffer[3]=bcd(rel/4500);buffer[4]=bcd(rel/75%60);buffer[5]=bcd(rel%75);
        buffer[6]=bcd(abs/4500);buffer[7]=bcd(abs/75%60);buffer[8]=bcd(abs%75);
        uint16_t crc=0;
        for(unsigned i=0;i<10;++i) {
            crc^=uint16_t(buffer[i])<<8;
            for(unsigned j=0;j<8;++j)crc=uint16_t((crc<<1)^((crc&0x8000)?0x1021:0));
        }
        crc=uint16_t(~crc);buffer[10]=uint8_t(crc);buffer[11]=uint8_t(crc>>8);
        *valid=1;return 1;
    }
    bool crc_valid = false;
    if (!static_cast<PS1::ISOReader*>(handle)->ReadSubChannelQ(
            lba, buffer, &crc_valid)) return 0;
    *valid = crc_valid ? 1 : 0;
    return 1;
}

int iso_has_subq_replacements(void* handle) {
    return handle && static_cast<PS1::ISOReader*>(handle)->HasSubChannelReplacements();
}

uint32_t iso_sector_count(void* handle) {
    if (!handle) return 0;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    return mod_runtime_disc_sector_count(reader->GetSectorCount());
}

/* CD-track TOC accessors (multi-track / CD-DA support). track is 1-based. */
int iso_track_count(void* handle) {
    if (!handle) return 1;
    return static_cast<PS1::ISOReader*>(handle)->TrackCount() + (mod_runtime_disc_extent_start()?1:0);
}

uint32_t iso_track_start_lba(void* handle, int track) {
    if (!handle) return 0;
    auto* reader=static_cast<PS1::ISOReader*>(handle);
    if (track==reader->TrackCount()+1 && mod_runtime_disc_extent_start()) return mod_runtime_disc_extent_start();
    return reader->TrackStartLBA(track);
}

uint32_t iso_track_pregap_lba(void* handle, int track) {
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    if (reader && track==reader->TrackCount()+1 && mod_runtime_disc_extent_start()) return mod_runtime_disc_extent_start();
    return reader ? reader->TrackPregapLBA(track) : 0;
}

int iso_track_is_audio(void* handle, int track) {
    if (!handle) return 0;
    return static_cast<PS1::ISOReader*>(handle)->TrackIsAudio(track) ? 1 : 0;
}

void iso_close(void* handle) {
    if (!handle) return;
    auto* reader = static_cast<PS1::ISOReader*>(handle);
    reader->Close();
    delete reader;
}

/* Audio-only overrides never affect filesystem reads, XA or disc validation. */
int iso_cdda_track_count(void* handle) {
    const int count=mod_runtime_cdda_track_count();
    if (count) return count;
    const int native=iso_cdda_native_scan(handle);
    if (native) return native;
    return iso_track_count(handle);
}
uint32_t iso_cdda_track_start_lba(void* handle,int track) {
    if(mod_runtime_cdda_track_count()) return mod_runtime_cdda_track_start(track);
    iso_cdda_native_scan(handle);
    if(audio_native_cdda_track_count()) return audio_native_cdda_track_start(track);
    return iso_track_start_lba(handle,track);
}
uint32_t iso_cdda_track_pregap_lba(void* handle,int track) {
    if(mod_runtime_cdda_track_count()) return mod_runtime_cdda_track_start(track);
    iso_cdda_native_scan(handle);
    if(audio_native_cdda_track_count()) return audio_native_cdda_track_pregap(track);
    return iso_track_pregap_lba(handle,track);
}
int iso_cdda_track_is_audio(void* handle,int track) {
    const int count=mod_runtime_cdda_track_count();
    if(count) return track>=2 && track<=count;
    iso_cdda_native_scan(handle);
    const int native=audio_native_cdda_track_count();
    if(native) return track>=2 && track<=native;
    return iso_track_is_audio(handle,track);
}
uint32_t iso_cdda_sector_count(void* handle) {
    if(mod_runtime_cdda_track_count()) return mod_runtime_cdda_track_start(0);
    iso_cdda_native_scan(handle);
    if(audio_native_cdda_track_count()) return audio_native_cdda_sector_count();
    return iso_sector_count(handle);
}
int iso_read_cdda_sector(void* handle,uint32_t lba,uint8_t* buffer,int size) {
    if(!handle || !buffer || size<2352) return 0;
    if(!mod_runtime_cdda_track_count()) {
        /* File-backed CD-DA (audio_native.c): 588 stereo frames per sector,
         * exactly the CD controller's unit, so cdrom.c keeps applying the
         * volume matrix, muting, position reports and track-end INT4. */
        iso_cdda_native_scan(handle);
        if(audio_native_cdda_track_count())
            return audio_native_cdda_read_sector(lba,buffer,size);
        return iso_read_raw_sector(handle,lba,buffer,size);
    }
    uint32_t source=0;
    const int kind=mod_runtime_read_cdda_sector(lba,buffer,size,&source);
    return kind==1 || (kind==2 && static_cast<PS1::ISOReader*>(handle)->ReadRawSector(source,buffer));
}

} /* extern "C" */
