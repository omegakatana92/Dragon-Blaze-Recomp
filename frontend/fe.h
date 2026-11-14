/* Reusable SDL2 frontend for native arcade recompilations.
 *
 * The frontend owns the window, menus, settings, input mapping, audio output,
 * save-state slots, rewind, screenshots, input movies and gadgets.  A game
 * plugs in through fe_game: it loads/validates its ROM set, runs one frame,
 * renders xRGB555 pixels and serializes its own state.  Nothing in frontend/
 * knows about a particular board.
 */
#ifndef FE_H
#define FE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FE_MAX_INPUTS 40
#define FE_MAX_DIPS 16
#define FE_MAX_DIP_OPTIONS 20
#define FE_MAX_GADGETS 8
#define FE_MAX_PLAYERS 4

/* One arcade input bit.  default_key is an SDL_Scancode, default_pad an
 * SDL_GameControllerButton (or FE_PAD_* axis code), -1 for none. */
typedef struct fe_input_def {
    const char *id;        /* config key, e.g. "p1_up" */
    const char *label;     /* shown in Controller Settings */
    int player;            /* 1..FE_MAX_PLAYERS, or 0 for cabinet inputs (service/test) */
    uint32_t mask;         /* bit passed to fe_game.set_inputs */
    int default_key;
    int default_key2;
    int default_pad;
    int impulse_frames;    /* >0: held for this many frames per press (coin) */
} fe_input_def;

typedef struct fe_dip_option { const char *label; uint16_t value; } fe_dip_option;

/* One DIP switch group (MAME PORT_DIPNAME). */
typedef struct fe_dip_def {
    const char *id;
    const char *label;
    int word;              /* which DIP word */
    uint16_t mask;
    uint16_t default_value;
    int option_count;
    fe_dip_option options[FE_MAX_DIP_OPTIONS];
} fe_dip_def;

/* A verified read-only game value for gadgets. */
typedef struct fe_gadget_def {
    const char *id;
    const char *label;
    /* Returns 1 and writes text when the value is currently meaningful. */
    int (*read)(char *text, size_t size);
} fe_gadget_def;

typedef struct fe_game {
    const char *id;              /* e.g. "viostorm": folders, config namespaces */
    const char *title;           /* window/menu title */
    const char *subtitle;
    const char *about;           /* multi-line About text */
    const char *rom_hint;        /* archive file name to look for, e.g. "viostorm.zip" */
    int width, height;           /* native visible area */
    double display_aspect;       /* original monitor aspect of the unrotated picture, e.g. 4.0/3.0 */
    int rotation;                /* original monitor orientation, quarter turns clockwise (1 = vertical ROT90) */
    double refresh_hz;
    double audio_rate;           /* native sample rate of run_frame audio */
    int max_audio_frames;        /* per run_frame call */
    int dip_words;
    int players;                 /* simultaneous players (default 2) */

    const fe_input_def *inputs; int input_count;
    const fe_dip_def *dips; int dip_count;
    const fe_gadget_def *gadgets; int gadget_count;

    /* Lifecycle.  load() validates and loads; validate() only checks. */
    int (*validate_rom)(const char *path, char *err, size_t n);
    int (*load)(const char *path, char *err, size_t n);
    void (*unload)(void);
    void (*reset)(void);
    void (*set_dips)(const uint16_t *words);
    void (*set_inputs)(uint32_t mask);
    /* Returns 0 on success, otherwise writes a fatal error. */
    int (*run_frame)(int16_t *stereo, int *frames, char *err, size_t n);
    void (*render)(uint16_t *xrgb555);   /* width*height pixels */
    void (*render32)(uint32_t *xrgb8888); /* optional; preferred over render when set */

    /* Save states: malloc'd blob; load validates before touching state. */
    uint8_t *(*save_state)(size_t *size);
    int (*validate_state)(const uint8_t *data, size_t size, char *err, size_t n);
    int (*load_state)(const uint8_t *data, size_t size, char *err, size_t n);
    uint32_t (*rom_fingerprint)(void);
    const char *runtime_version;  /* recorded in state files */

    /* Optional non-volatile memory (EEPROM / NVRAM / high scores).  The
     * frontend stores it as <data>/nvram/<id>.nv, loads it after load()/reset
     * and saves it when nvram_dirty() reports a change and at exit.
     * nvram_save returns the number of bytes written (0 = nothing). */
    size_t (*nvram_save)(uint8_t *buf, size_t max);
    int (*nvram_load)(const uint8_t *data, size_t size);
    int (*nvram_dirty)(void);

    /* Optional external profile (profiles/<id>.json); see fe_profile.h. */
    const char *profile_name;
} fe_game;

/* Run the frontend.  argv understood: --rom PATH, --fullscreen, --windowed,
 * --scale N, --portable, --state FILE (load at start), --no-menu,
 * --frames N (exit after N frames, for automated checks), --rotate N (quarter turns),
 * --screenshot FILE (write a frame grab at exit), --help. */
int fe_run(const fe_game *game, int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
