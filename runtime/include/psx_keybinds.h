#pragma once
#include <stdint.h>
#include "psx_sdl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * psx_keybinds — configurable KEYBOARD -> PlayStation DualShock keybinds.
 *
 * INI-driven, generated next to the exe as `keybinds.ini` on first run. Covers
 * every DualShock input: the D-pad, the four face buttons (Cross/Circle/
 * Square/Triangle), the shoulders (L1/L2/R1/R2), Start/Select, the stick
 * clicks (L3/R3), and — for the keyboard analog path — the left and right
 * analog-stick directions. Each input maps to one SDL_Scancode; "None"
 * (SDL_SCANCODE_UNKNOWN) leaves it unbound.
 *
 * This is the KEYBOARD map only. Game-controller (gamepad) button mapping is a
 * separate concern handled by input.ini (see main.cpp load_input_config).
 *
 * Up to PSXKB_MAX_PLAYERS keyboard players (P1..P5). Every player ships with the
 * same default keyboard map (the framework's historical P1 layout); "Reset to
 * Defaults" restores that map for any slot. Simultaneous multi-keyboard play
 * still needs distinct binds per slot — only one physical keyboard.
 *
 * The launcher's Controls page edits these live through the rebind API below
 * (get/set/reset/save) and persists to the same keybinds.ini; the runtime
 * re-reads the file at startup via psx_keybinds_init.
 */

#define PSXKB_MAX_PLAYERS 5

/* Shared pseudo-scancodes for INI Mouse1..Mouse5. Keep them outside SDL's
 * keyboard array and check them before indexing it. Mouse3 is right. */
#define PSXKB_MOUSE_SC_BASE 512
#define PSXKB_MOUSE_SC(btn) ((SDL_Scancode)(PSXKB_MOUSE_SC_BASE + (btn)))
#define PSXKB_IS_MOUSE_SC(sc) \
    ((int)(sc) > PSXKB_MOUSE_SC_BASE && (int)(sc) <= PSXKB_MOUSE_SC_BASE + 5)

/* Button indices — stable order, matches the order keybinds.ini writes and the
 * order the launcher's rebind chips are laid out. Keep in sync with kButtons[]
 * in psx_keybinds.c. */
enum PsxKeybindButton {
    PSX_KB_UP = 0, PSX_KB_DOWN, PSX_KB_LEFT, PSX_KB_RIGHT,
    PSX_KB_CROSS, PSX_KB_CIRCLE, PSX_KB_SQUARE, PSX_KB_TRIANGLE,
    PSX_KB_L1, PSX_KB_R1, PSX_KB_L2, PSX_KB_R2,
    PSX_KB_L3, PSX_KB_R3,
    PSX_KB_START, PSX_KB_SELECT,
    PSX_KB_LS_UP, PSX_KB_LS_DOWN, PSX_KB_LS_LEFT, PSX_KB_LS_RIGHT,
    PSX_KB_RS_UP, PSX_KB_RS_DOWN, PSX_KB_RS_LEFT, PSX_KB_RS_RIGHT,
    PSX_KB_COUNT
};

typedef struct {
    SDL_Scancode up, down, left, right;
    SDL_Scancode cross, circle, square, triangle;
    SDL_Scancode l1, r1, l2, r2;
    SDL_Scancode l3, r3;
    SDL_Scancode start, select;
    SDL_Scancode ls_up, ls_down, ls_left, ls_right;   /* left analog stick */
    SDL_Scancode rs_up, rs_down, rs_left, rs_right;   /* right analog stick */
} PsxPlayerBinds;

typedef struct {
    PsxPlayerBinds player[PSXKB_MAX_PLAYERS]; /* [0]=P1 .. [4]=P5 */
} PsxKeyBinds;

/* Initialize from <exe_dir>/keybinds.ini. Generates a default file if one does
 * not exist. exe_path may be NULL/argv[0]/the exe directory itself — the .ini is
 * placed in that file's directory (or the path itself when it is a directory). */
void psx_keybinds_init(const char *exe_path);

/* Read-only view of the current bindings. */
const PsxKeyBinds *psx_keybinds_get(void);

/* Runtime-owned host controls consumed by optional local input capture.
 * Suppression is per source/player, before PSX folding, and lasts through
 * release. It never clears a merged PSX button held by another source. */
void psx_keybinds_suppress_control(int player, SDL_Scancode sc);
void psx_keybinds_release_control(int player, SDL_Scancode sc);
int psx_keybinds_control_suppressed(int player, SDL_Scancode sc);
/* Host hotkey polling gets a keyboard view excluding consumed P1 sources.
 * The original SDL state and other players' PSX folds remain unchanged. */
void psx_keybinds_host_keys(const uint8_t *keys, uint8_t out[SDL_NUM_SCANCODES]);

/* ── Runtime read helpers (keyboard -> PSX pad), player is 1..PSXKB_MAX_PLAYERS */

/* Build the 16-bit ACTIVE-LOW PSX button word for `player` from the SDL
 * keyboard state (bits per the standard PSX pad word; unbound inputs never
 * assert). Returns 0xFFFF (all released) if `keys` is NULL or player is out of
 * range. AND this into the pad word (an unpressed source leaves bits high). */
uint16_t psx_keybinds_pad_word(const uint8_t *keys, int player);

/* Fill analog-stick bytes (0x80 centred): out[0]=LX, out[1]=LY, out[2]=RX,
 * out[3]=RY, from the player's stick-direction binds. A bound direction held
 * drives that axis to the extreme (0x00 / 0xFF). Untouched axes stay centred.
 * Only overwrites axes that have a live bound press, so callers can pre-seed. */
void psx_keybinds_sticks(const uint8_t *keys, int player, uint8_t out[4]);

/* True if any of the player's D-pad direction binds is currently held (used by
 * the hybrid pad-mode auto-switch: a D-pad press flips the pad to digital). */
int psx_keybinds_dpad_active(const uint8_t *keys, int player);

/* ── Synthetic key events (tests / debug injection, no device) ────────────── */
/* An explicit per-scancode overlay ORed into every keyboard read below (pad
 * word, stick folds, D-pad activity). It is the same input the SDL keyboard
 * state would report, so a synthetic key and a physical key converge before the
 * binding test; it is consumed by the pad path only, not by host hotkeys. The
 * debug server's `input_event` command calls these. Zero cost when unused: the
 * overlay index is only read after the `s_injected_any` gate. */
void psx_keybinds_inject_key(SDL_Scancode sc, int down);
void psx_keybinds_clear_injected(void);
int  psx_keybinds_injected_held(SDL_Scancode sc);
/* True while at least one synthetic key is down (headless host-input gate). */
int  psx_keybinds_has_injected(void);

/* ── Rebind API (launcher Controls page) ──────────────────────────────────── */
/* Buttons are indexed 0..PSX_KB_COUNT-1 (see PsxKeybindButton). Scancodes, not
 * keycodes (keybinds.ini stores SDL scancode names). Players are 1-based. */
int          psx_keybinds_button_count(void);
const char  *psx_keybinds_button_name(int button);          /* "up".."rs_right" */
const char  *psx_keybinds_button_label(int button);         /* pretty, e.g. "Cross" */
SDL_Scancode psx_keybinds_get_button(int player, int button);
void         psx_keybinds_set_button(int player, int button, SDL_Scancode sc);
/* Alternate (secondary) binding per control: both the primary and the alt
 * assert the input (e.g. cross = X, Mouse1). Stored in keybinds.ini as a
 * comma-separated second name; SDL_SCANCODE_UNKNOWN = no alt. */
SDL_Scancode psx_keybinds_get_button_alt(int player, int button);
void         psx_keybinds_set_button_alt(int player, int button, SDL_Scancode sc);
/* Reset one player's bindings to the shared built-in primary defaults and
 * clear that player's alternate bindings. */
void         psx_keybinds_reset_player(int player);
/* Persist the current bindings to keybinds.ini (path resolved by
 * psx_keybinds_init; call that first). */
void         psx_keybinds_save(void);

#ifdef __cplusplus
}
#endif
