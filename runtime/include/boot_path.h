/* Public surface of runtime/boot_path.c ("just play" card seeding).
 *
 * Installed next to the plugin C files in WE2002Recomp/src/mods/, so main.cpp
 * can declare the report struct instead of repeating it: the .c keeps every
 * definition, this header keeps only what a caller outside the file needs.
 * That directory is already on psx-runtime's include path (runtime/boot_path.cmake
 * adds it explicitly; the plugin-file glob does not).
 */
#ifndef WE2002_BOOT_PATH_H
#define WE2002_BOOT_PATH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result of we2002_boot_card_state(): the three answers the game's SELECT
 * (FUN_8009BEC0) produces, in the same order (1 / -1 / -2). */
enum {
    WE2002_BOOT_CARD_ABSENT = 0,
    WE2002_BOOT_CARD_NOT_FORMATTED = 1,
    WE2002_BOOT_CARD_READY = 2
};

/* What we2002_boot_apply() did, for one log line. */
typedef struct {
    int seeded;      /* 1 = the option record was written */
    int blocks_used; /* blocks the option file occupies after the call */
    int free_blocks; /* free blocks after the call */
    int wrote_bytes; /* bytes written into the card image (0 when inert) */
} We2002BootSeedReport;

/* [saves] seed_option_file. Every other entry point is inert until this ran
 * with a nonzero argument, so a stock run reads, writes and returns nothing
 * differently. */
void we2002_boot_configure(int enabled);
int  we2002_boot_enabled(void);

/* Card model (validated 128 KiB image, BISLPM-87056WEW-OPT block chain). */
int we2002_boot_card_state(const uint8_t* card, size_t size);
int we2002_boot_option_blocks(const uint8_t* card, size_t size);
int we2002_boot_option_present(const uint8_t* card, size_t size);
int we2002_boot_free_blocks(const uint8_t* card, size_t size);

/* Write the 0x4000-byte option record into `card` (never over an existing
 * option file). 1 = written, 0 = nothing written. */
int we2002_boot_seed_option(uint8_t* card, size_t size, const uint8_t* seed,
                            size_t seed_len, We2002BootSeedReport* report);

/* Configure, then apply to a live card image: the one call main.cpp makes.
 * Returns the card state (WE2002_BOOT_CARD_*). With the flag off it only
 * returns the state. */
int we2002_boot_apply(uint8_t* card, size_t size, const uint8_t* seed,
                      size_t seed_len, We2002BootSeedReport* report);

#ifdef __cplusplus
}
#endif

#endif /* WE2002_BOOT_PATH_H */
