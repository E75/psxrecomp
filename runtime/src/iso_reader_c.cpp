/*
 * iso_reader_c.cpp — C wrapper for the C++ ISOReader class
 *
 * Provides iso_open() / iso_read_sector() / iso_close() for use by cdrom.c
 */

#include "iso_reader.h"
#include "mod_runtime.h"
#include <cstdio>
#include <cstring>

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

} /* extern "C" */
