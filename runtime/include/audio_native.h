/*
 * audio_native.h — file-backed CD-DA and streamed-XA audio sources.
 *
 * The retail disc's music is not in a Track-1-only dump: the eight
 * `SD/DA/<name>.DA` entries are the disc's Red Book CD-DA tracks (15:36) and the
 * only streamed XA on the disc is inside `MOVIE/WE2002.STR` (one stereo 4-bit
 * 37.8 kHz channel, 2:02). `tools/assets/extract_xa.py` writes both as 16-bit
 * PCM WAVs into `assets/audio/`. This module makes the runtime play those
 * files instead of needing the disc, without moving the mixing point:
 *
 *   CD-DA  — served at the CD controller's sector seam (iso_reader_c.cpp):
 *            one 2352-byte sector == 588 interleaved stereo frames, so the
 *            existing cdrom.c path keeps doing the volume matrix, muting,
 *            position reports and track-end INT4 unchanged.
 *   XA     — substituted at the SPU CD-input seam (spu_cd_audio_push), clocked
 *            by the emulated stream's own sector delivery so the movie's 7:1
 *            video:audio interleave stays in sync.
 *
 * Both paths fall back to the emulated CD when no file is present.
 *
 * Config: `[audio] cdda_dir` in game.toml (default "assets/audio"),
 * overridden by the PSX_CDDA_DIR environment variable.
 */

#ifndef PSX_AUDIO_NATIVE_H
#define PSX_AUDIO_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One CD-DA track as laid out by the disc's TOC. `iso_path` is the matching
 * ISO9660 entry (e.g. "SD/DA/01GOALDM.DA"); the WAV name is derived from it
 * the same way extract_xa.py derives it. */
typedef struct AudioNativeCddaTrack {
    uint32_t start_lba;
    uint32_t sectors;
    char     iso_path[64];
} AudioNativeCddaTrack;

/* Largest stereo frame count a single XA push may carry (spu.c stack buffer). */
#define AUDIO_NATIVE_XA_MAX_FRAMES 4096

void audio_native_init(void);   /* idempotent; resolves the config + directory */

/* ---- CD-DA (called from iso_reader_c.cpp) ------------------------------ */
/* Install the track table. Call only when the mounted image has no audio
 * tracks of its own; the playlist stays inactive (count 0) if no WAV exists. */
void audio_native_cdda_configure(const AudioNativeCddaTrack* tracks, int count);
/* Tell the state report whether the mounted image itself carries audio
 * tracks (a full multi-track cue): then the disc is the live source. */
void audio_native_cdda_note_disc_audio(int audio_tracks);

int      audio_native_cdda_track_count(void);          /* 0 = no native playlist */
uint32_t audio_native_cdda_track_start(int track);     /* 1-based, 0 = lead-out */
uint32_t audio_native_cdda_track_pregap(int track);
uint32_t audio_native_cdda_sector_count(void);         /* lead-out LBA */
int      audio_native_cdda_track_for_lba(uint32_t lba);
/* 1 = sector served (588 frames), 0 = no data (gap / past end / no file). */
int      audio_native_cdda_read_sector(uint32_t lba, uint8_t* out, int size);

/* ---- Streamed XA (called from spu.c) ---------------------------------- */
/* `str_lba`/`str_sectors` describe the STR extent the audio is interleaved
 * in; the WAV name is derived from `iso_path` ("MOVIE/WE2002.STR"). */
void audio_native_xa_configure(uint32_t str_lba, uint32_t str_sectors,
                               const char* iso_path);
/* Replace `frames` stereo frames of emulated XA PCM with native PCM.
 * `emulated` is what the CD-XA decoder produced for this push: an all-zero
 * buffer means the drive is muted (or the decode produced silence) and is
 * passed through untouched. `stream_active` is cdrom_xa_stream_active() and
 * `delivered_lba` is cdrom_get_delivered_lba(), both sampled by spu.c so this
 * module keeps no dependency on the CD model. Returns 1 when `out` holds
 * native samples. */
int audio_native_xa_fill(const int16_t* emulated, int frames, int16_t* out,
                         int stream_active, int delivered_lba);

/* ---- Raw STR sectors (called from iso_reader_c.cpp) ------------------- */
/* A streamed STR's sectors are Mode 2 Form 2 (2324 payload bytes), which the
 * extracted 2048-byte tree cannot carry, so its XA audio cannot decode from
 * there. `<str_raw>/WE2002.STR.raw` (see tools/assets/extract_str_raw.py)
 * holds the extent's raw 2352-byte sectors, video and interleaved audio
 * together, behind a 32-byte header: "PSTR", version 1, start LBA, sector
 * count, sector size 2352, 12 reserved bytes. Absent or invalid file => the
 * disc image path, exactly as before. */
void audio_native_str_raw_configure(uint32_t start_lba, uint32_t sectors);
/* 1 = the sector was served from the raw file. */
int  audio_native_str_raw_read(uint32_t lba, uint8_t* out, int size);

/* ---- State ------------------------------------------------------------ */
/* {"cdda_dir":..,"cdda":{source,file,rate,channels,frames,underruns,track,
 *  tracks,disc_audio_tracks},"xa":{source,file,rate,out_rate,channels,frames,
 *  underruns,active,str_lba},"str_raw":{source,file,sectors,served}} */
int  audio_native_state_json(char* out, int cap);
/* Rewrite <exe_dir>/audio_state.json (transitions + every ~2 s of stream). */
void audio_native_state_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_AUDIO_NATIVE_H */
