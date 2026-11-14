/* Frontend application: window, game loop, settings, menus, input mapping,
 * audio, save-state slots, rewind, input movies, screenshots and gadgets.
 *
 * The UI is immediate-mode: every frame the active screen function lays out
 * its rows and reacts to the navigation events collected for that frame.
 * Interface layout, colours and artwork are original to this project.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#endif
#include "fe_internal.h"
#include "fe_profile.h"
#include "miniz.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FE_SLOTS 10
#define FE_SLOT_AUTO 0          /* slot index 0 is the autosave */
#define FE_THUMB_DIV 2
#define FE_STATE_MAGIC "FESTATE1"
#define FE_STATE_CONTAINER 1u
#define FE_PAD_AXIS_BASE 100    /* pad binding >= this: axis*2 + (positive?1:0) */
#define FE_HOTKEY_COUNT 11

/* ------------------------------------------------------------------ types */

typedef struct binding { int key[2]; int pad; } binding;

enum hotkey {
    HK_MENU, HK_QSAVE, HK_QLOAD, HK_SLOT_PREV, HK_SLOT_NEXT, HK_SHOT,
    HK_FULLSCREEN, HK_REWIND, HK_PAUSE, HK_FPS, HK_RESET
};
static const char *const HOTKEY_ID[FE_HOTKEY_COUNT] = {
    "menu", "quick_save", "quick_load", "slot_prev", "slot_next", "screenshot",
    "fullscreen", "rewind", "pause", "toggle_fps", "reset" };
static const char *const HOTKEY_LABEL[FE_HOTKEY_COUNT] = {
    "Quick Menu", "Quick Save", "Quick Load", "Previous Slot", "Next Slot", "Screenshot",
    "Toggle Fullscreen", "Rewind (hold)", "Pause", "Toggle FPS", "Reset Game" };

typedef enum screen_id {
    SCR_MAIN, SCR_QUICK, SCR_DISPLAY, SCR_AUDIO, SCR_CONTROLS, SCR_CONTROLS_P,
    SCR_HOTKEYS, SCR_GAMEPLAY, SCR_GADGETS, SCR_EXTRAS, SCR_ABOUT, SCR_SAVE,
    SCR_LOAD, SCR_ROM, SCR_CONFIRM, SCR_MESSAGE, SCR_CAPTURE, SCR_GADGET_EDIT,
    SCR_COUNT
} screen_id;

typedef enum confirm_action {
    CF_NONE, CF_EXIT, CF_MAIN_MENU, CF_RESTART, CF_LOAD_SLOT, CF_SAVE_SLOT,
    CF_NEW_GAME, CF_RESET_SETTINGS, CF_RESET_CONTROLS, CF_DELETE_SLOT, CF_ROM_CHANGE
} confirm_action;

typedef struct nav {
    int up, down, left, right, accept, back, page_up, page_down, alt;
    int mouse_x, mouse_y, mouse_moved, click, wheel;
    int any;
} nav;

typedef struct slot_info {
    int exists, valid;
    char error[160];
    int64_t timestamp;
    uint32_t frames;
    int thumb_w, thumb_h;
    SDL_Texture *thumb;
} slot_info;

typedef struct rewind_buf {
    uint8_t **snap; size_t *size; int cap, head, count;
    size_t bytes, budget;
} rewind_buf;

typedef struct gadget_cfg { int enabled, anchor, size, dx, dy; } gadget_cfg;

typedef struct settings {
    int fullscreen, window_scale, scale_mode, aspect_mode, filter, vsync, pacing;
    int scanlines, mask, mask_strength, brightness, gamma, border, rotation, show_bezel_text;
    int volume, mute, latency_ms;
    char audio_device[256];
    int accent;
    int autosave_exit, autosave_menu, pause_on_focus, rewind_enabled, rewind_mb;
    int deadzone;
    int menu_combo;
    int pad_player[FE_MAX_PLAYERS]; /* controller order index per player (-1 auto) */
    uint16_t dips[4];
    gadget_cfg gadget[FE_MAX_GADGETS + 3];
} settings;

typedef struct app {
    const fe_game *g;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *game_tex, *scan_tex, *mask_tex, *ambient_tex;
    int mask_w, mask_h, mask_kind, mask_level;
    int win_w, win_h;

    char base_dir[FE_PATH_MAX], config_path[FE_PATH_MAX], states_dir[FE_PATH_MAX], nvram_dir[FE_PATH_MAX];
    char shots_dir[FE_PATH_MAX], movies_dir[FE_PATH_MAX], log_path[FE_PATH_MAX];
    fe_ini ini;
    settings s;
    binding bind[FE_MAX_INPUTS];
    binding hk[FE_HOTKEY_COUNT];

    fe_game game;           /* adapter description with profile overrides applied */
    fe_profile profile;
    int feat[FE_FEAT_COUNT];  /* requested by the profile and supported by the adapter */
    char feat_report[400];

    char rom_path[FE_PATH_MAX];
    int rom_ok;
    char rom_error[512];
    int session;          /* a game is in progress */
    int paused;           /* user pause (P) */
    int in_menu;          /* menu overlay active */
    int quit;
    uint32_t *pixels;     /* current frame, xRGB8888 */
    uint16_t *pixels555;  /* scratch for games that render xRGB555 */
    uint32_t *argb;
    uint8_t ramp[256];    /* brightness/gamma per 8-bit channel */
    int lut_dirty;
    int16_t *audio_buf;

    /* timing */
    uint64_t perf_freq, last_tick;
    double accumulator;
    double frame_ms_avg, fps, present_fps;
    uint64_t fps_t0; int fps_frames, present_frames;
    uint64_t frames_total;
    int auto_exit_frames;
    char auto_screenshot[FE_PATH_MAX];

    /* input */
    SDL_GameController *pads[8];
    int pad_count;
    uint32_t game_mask;
    int impulse[FE_MAX_INPUTS];
    int prev_pressed[FE_MAX_INPUTS];
    int hk_down[FE_HOTKEY_COUNT];
    int combo_latch;
    uint32_t suppress_until_release;

    /* audio */
    SDL_AudioDeviceID audio_dev;
    SDL_AudioStream *audio_stream;
    SDL_AudioSpec audio_have;
    int audio_drops, audio_underruns;

    /* states */
    int slot;              /* 1..FE_SLOTS for quick save/load */
    slot_info slots[FE_SLOTS + 1];
    int slots_dirty;
    rewind_buf rw;
    int rewinding;

    /* movies */
    FILE *movie_rec;
    uint8_t *movie_play; size_t movie_play_size, movie_play_at;
    char movie_status[128];

    /* ui */
    screen_id stack[16];
    int depth;
    int focus[SCR_COUNT];
    int scroll[SCR_COUNT];
    float anim_y;
    uint64_t screen_t0;
    nav nv;
    int row, row_count, row_y0, row_h, list_x, list_w, list_top, list_bottom, visible_rows;
    int ui_scale_px;
    char toast[160];
    uint64_t toast_until;
    confirm_action pending; int pending_arg;
    char confirm_text[256];
    char message_title[64], message_text[700];
    int capture_index, capture_kind, capture_is_hotkey; uint64_t capture_t0;
    int gadget_edit;
    char help[256];

    /* rom browser */
    char browse_dir[FE_PATH_MAX];
    fe_dirent *entries; int entry_count; int browse_roots;
    int browse_all;
} app;

static app A;

/* ------------------------------------------------------------- utilities */

static uint64_t now_ticks(void) { return SDL_GetPerformanceCounter(); }
static double ms_since(uint64_t t0) { return (double)(now_ticks() - t0) * 1000.0 / (double)A.perf_freq; }

static void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.toast, sizeof(A.toast), fmt, ap);
    va_end(ap);
    A.toast_until = now_ticks() + A.perf_freq * 2;
    fe_log("%s", A.toast);
}

static void show_message(const char *title, const char *fmt, ...);

static const fe_color ACCENTS[] = {
    { 64, 156, 255, 255 }, { 255, 176, 46, 255 }, { 236, 72, 88, 255 },
    { 64, 206, 132, 255 }, { 168, 112, 255, 255 }, { 230, 230, 236, 255 } };
static const char *const ACCENT_NAMES[] = { "Azure", "Amber", "Crimson", "Jade", "Violet", "Silver" };
#define ACCENT_COUNT 6

static fe_color accent(void) { return ACCENTS[A.s.accent % ACCENT_COUNT]; }
static fe_color rgba(int r, int g, int b, int a) { fe_color c = { (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a }; return c; }
static fe_color with_alpha(fe_color c, int a) { c.a = (uint8_t)a; return c; }

/* --------------------------------------------------------------- settings */

static const char *SEC_DISPLAY = "display", *SEC_AUDIO = "audio", *SEC_UI = "ui", *SEC_GAME = "gameplay";
static const char *SEC_INPUT = "input", *SEC_HOTKEY = "hotkeys", *SEC_GADGET = "gadgets", *SEC_PATHS = "paths";

static int gadget_total(void) { return 3 + A.g->gadget_count; }
static const char *gadget_id(int i)
{
    static const char *builtin[3] = { "fps", "frame_time", "input_display" };
    return i < 3 ? builtin[i] : A.g->gadgets[i - 3].id;
}
static const char *gadget_label(int i)
{
    static const char *builtin[3] = { "FPS", "Frame Time", "Input Display" };
    return i < 3 ? builtin[i] : A.g->gadgets[i - 3].label;
}

static void default_bindings(void)
{
    for (int i = 0; i < A.g->input_count; i++) {
        A.bind[i].key[0] = A.g->inputs[i].default_key;
        A.bind[i].key[1] = A.g->inputs[i].default_key2;
        A.bind[i].pad = A.g->inputs[i].default_pad;
    }
    static const int hk_keys[FE_HOTKEY_COUNT] = {
        SDL_SCANCODE_ESCAPE, SDL_SCANCODE_F5, SDL_SCANCODE_F9, SDL_SCANCODE_F6, SDL_SCANCODE_F7,
        SDL_SCANCODE_F12, SDL_SCANCODE_F11, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_PAUSE,
        SDL_SCANCODE_F10, SDL_SCANCODE_F3 };
    for (int i = 0; i < FE_HOTKEY_COUNT; i++) {
        A.hk[i].key[0] = hk_keys[i];
        A.hk[i].key[1] = -1;
        A.hk[i].pad = -1;
    }
    A.hk[HK_PAUSE].key[1] = SDL_SCANCODE_P;
    A.hk[HK_FULLSCREEN].key[1] = -1;
}

static void default_settings(void)
{
    settings *s = &A.s;
    memset(s, 0, sizeof(*s));
    s->fullscreen = 0;
    s->window_scale = 3;
    s->scale_mode = 0;      /* 0 fit, 1 integer */
    s->aspect_mode = 0;     /* 0 original 4:3, 1 square pixels */
    s->filter = 0;          /* 0 nearest, 1 bilinear */
    s->vsync = 1;
    s->pacing = 0;          /* 0 exact game rate timer, 1 display sync */
    s->scanlines = 0;
    s->mask = 0;
    s->mask_strength = 30;
    s->brightness = 100;
    s->gamma = 100;
    s->border = 1;          /* 0 black, 1 gradient, 2 ambient */
    s->rotation = A.g->rotation & 3;   /* the original monitor orientation */
    s->volume = 80;
    s->mute = 0;
    s->latency_ms = 64;
    s->audio_device[0] = 0;
    s->accent = 0;
    s->autosave_exit = 1;
    s->autosave_menu = 1;
    s->pause_on_focus = 1;
    s->rewind_enabled = 0;
    s->rewind_mb = 64;
    s->deadzone = 30;
    s->menu_combo = 0;      /* Back+Start */
    for (int p = 0; p < FE_MAX_PLAYERS; p++) s->pad_player[p] = p;
    for (int i = 0; i < 4; i++) s->dips[i] = 0xFFFF;
    for (int i = 0; i < A.g->dip_count; i++) {
        const fe_dip_def *d = &A.g->dips[i];
        s->dips[d->word] = (uint16_t)((s->dips[d->word] & ~d->mask) | (d->default_value & d->mask));
    }
    for (int i = 0; i < FE_MAX_GADGETS + 3; i++) {
        s->gadget[i].enabled = 0;
        s->gadget[i].anchor = i == 0 ? 1 : (i == 2 ? 3 : 0);
        s->gadget[i].size = 1;
        s->gadget[i].dx = s->gadget[i].dy = 0;
    }
    /* Steam Deck: start fullscreen. */
    if (getenv("SteamDeck") || getenv("STEAM_DECK")) s->fullscreen = 1;
}

static void key_name(int sc, char *out, size_t n)
{
    if (sc < 0) { snprintf(out, n, "-"); return; }
    const char *name = SDL_GetScancodeName((SDL_Scancode)sc);
    snprintf(out, n, "%s", name && *name ? name : "?");
}

static void pad_name(int pad, char *out, size_t n)
{
    if (pad < 0) { snprintf(out, n, "-"); return; }
    if (pad >= FE_PAD_AXIS_BASE) {
        int axis = (pad - FE_PAD_AXIS_BASE) / 2, pos = (pad - FE_PAD_AXIS_BASE) & 1;
        const char *an = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)axis);
        snprintf(out, n, "%s%s", an ? an : "axis", pos ? "+" : "-");
        return;
    }
    static const char *names[] = { "A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3",
                                   "LB", "RB", "D-Up", "D-Down", "D-Left", "D-Right",
                                   "Misc", "Paddle1", "Paddle2", "Paddle3", "Paddle4", "Touchpad" };
    if (pad < (int)(sizeof(names) / sizeof(names[0]))) snprintf(out, n, "%s", names[pad]);
    else snprintf(out, n, "Button %d", pad);
}

static void save_binding(const char *section, const char *id, const binding *b)
{
    char v[96];
    snprintf(v, sizeof(v), "%d,%d,%d", b->key[0], b->key[1], b->pad);
    fe_ini_set(&A.ini, section, id, v);
}

static void load_binding(const char *section, const char *id, binding *b)
{
    const char *v = fe_ini_get(&A.ini, section, id);
    if (!v) return;
    int k0, k1, p;
    if (sscanf(v, "%d,%d,%d", &k0, &k1, &p) == 3) { b->key[0] = k0; b->key[1] = k1; b->pad = p; }
}

static void save_settings(void)
{
    settings *s = &A.s;
    fe_ini *ini = &A.ini;
    fe_ini_set(ini, SEC_PATHS, "rom", A.rom_path);
    fe_ini_set(ini, SEC_PATHS, "browse_dir", A.browse_dir);
    fe_ini_set_int(ini, SEC_DISPLAY, "fullscreen", s->fullscreen);
    fe_ini_set_int(ini, SEC_DISPLAY, "window_scale", s->window_scale);
    fe_ini_set_int(ini, SEC_DISPLAY, "scale_mode", s->scale_mode);
    fe_ini_set_int(ini, SEC_DISPLAY, "aspect_mode", s->aspect_mode);
    fe_ini_set_int(ini, SEC_DISPLAY, "filter", s->filter);
    fe_ini_set_int(ini, SEC_DISPLAY, "vsync", s->vsync);
    fe_ini_set_int(ini, SEC_DISPLAY, "pacing", s->pacing);
    fe_ini_set_int(ini, SEC_DISPLAY, "scanlines", s->scanlines);
    fe_ini_set_int(ini, SEC_DISPLAY, "mask", s->mask);
    fe_ini_set_int(ini, SEC_DISPLAY, "mask_strength", s->mask_strength);
    fe_ini_set_int(ini, SEC_DISPLAY, "brightness", s->brightness);
    fe_ini_set_int(ini, SEC_DISPLAY, "gamma", s->gamma);
    fe_ini_set_int(ini, SEC_DISPLAY, "border", s->border);
    fe_ini_set_int(ini, SEC_DISPLAY, "rotation", s->rotation);
    fe_ini_set_int(ini, SEC_AUDIO, "volume", s->volume);
    fe_ini_set_int(ini, SEC_AUDIO, "mute", s->mute);
    fe_ini_set_int(ini, SEC_AUDIO, "latency_ms", s->latency_ms);
    fe_ini_set(ini, SEC_AUDIO, "device", s->audio_device);
    fe_ini_set_int(ini, SEC_UI, "accent", s->accent);
    fe_ini_set_int(ini, SEC_UI, "autosave_exit", s->autosave_exit);
    fe_ini_set_int(ini, SEC_UI, "autosave_menu", s->autosave_menu);
    fe_ini_set_int(ini, SEC_UI, "pause_on_focus_loss", s->pause_on_focus);
    fe_ini_set_int(ini, SEC_UI, "rewind", s->rewind_enabled);
    fe_ini_set_int(ini, SEC_UI, "rewind_mb", s->rewind_mb);
    fe_ini_set_int(ini, SEC_UI, "slot", A.slot);
    fe_ini_set_int(ini, SEC_INPUT, "deadzone", s->deadzone);
    fe_ini_set_int(ini, SEC_INPUT, "menu_combo", s->menu_combo);
    for (int p = 0; p < FE_MAX_PLAYERS; p++) {
        char key[32];
        snprintf(key, sizeof(key), "p%d_controller", p + 1);
        fe_ini_set_int(ini, SEC_INPUT, key, s->pad_player[p]);
    }
    for (int i = 0; i < A.g->input_count; i++) save_binding(SEC_INPUT, A.g->inputs[i].id, &A.bind[i]);
    for (int i = 0; i < FE_HOTKEY_COUNT; i++) save_binding(SEC_HOTKEY, HOTKEY_ID[i], &A.hk[i]);
    for (int i = 0; i < A.g->dip_words; i++) {
        char k[16], v[16];
        snprintf(k, sizeof(k), "dsw%d", i + 1);
        snprintf(v, sizeof(v), "0x%04X", s->dips[i]);
        fe_ini_set(ini, SEC_GAME, k, v);
    }
    for (int i = 0; i < gadget_total(); i++) {
        char v[64];
        gadget_cfg *g = &s->gadget[i];
        snprintf(v, sizeof(v), "%d,%d,%d,%d,%d", g->enabled, g->anchor, g->size, g->dx, g->dy);
        fe_ini_set(ini, SEC_GADGET, gadget_id(i), v);
    }
    if (!fe_ini_save(ini, A.config_path)) fe_log("could not write settings to %s", A.config_path);
}

static void load_settings(void)
{
    settings *s = &A.s;
    fe_ini *ini = &A.ini;
    default_settings();
    default_bindings();
    if (!fe_ini_load(ini, A.config_path)) return;
    const char *v = fe_ini_get(ini, SEC_PATHS, "rom");
    if (v) snprintf(A.rom_path, sizeof(A.rom_path), "%s", v);
    v = fe_ini_get(ini, SEC_PATHS, "browse_dir");
    if (v) snprintf(A.browse_dir, sizeof(A.browse_dir), "%s", v);
#define GI(sec, key, field) s->field = fe_ini_get_int(ini, sec, key, s->field)
    GI(SEC_DISPLAY, "fullscreen", fullscreen); GI(SEC_DISPLAY, "window_scale", window_scale);
    GI(SEC_DISPLAY, "scale_mode", scale_mode); GI(SEC_DISPLAY, "aspect_mode", aspect_mode);
    GI(SEC_DISPLAY, "filter", filter); GI(SEC_DISPLAY, "vsync", vsync); GI(SEC_DISPLAY, "pacing", pacing);
    GI(SEC_DISPLAY, "scanlines", scanlines); GI(SEC_DISPLAY, "mask", mask);
    GI(SEC_DISPLAY, "mask_strength", mask_strength); GI(SEC_DISPLAY, "brightness", brightness);
    GI(SEC_DISPLAY, "gamma", gamma); GI(SEC_DISPLAY, "border", border); GI(SEC_DISPLAY, "rotation", rotation);
    GI(SEC_AUDIO, "volume", volume); GI(SEC_AUDIO, "mute", mute); GI(SEC_AUDIO, "latency_ms", latency_ms);
    GI(SEC_UI, "accent", accent); GI(SEC_UI, "autosave_exit", autosave_exit);
    GI(SEC_UI, "autosave_menu", autosave_menu); GI(SEC_UI, "pause_on_focus_loss", pause_on_focus);
    GI(SEC_UI, "rewind", rewind_enabled); GI(SEC_UI, "rewind_mb", rewind_mb);
    GI(SEC_INPUT, "deadzone", deadzone); GI(SEC_INPUT, "menu_combo", menu_combo);
    for (int p = 0; p < FE_MAX_PLAYERS; p++) {
        char key[32];
        snprintf(key, sizeof(key), "p%d_controller", p + 1);
        s->pad_player[p] = fe_ini_get_int(ini, SEC_INPUT, key, s->pad_player[p]);
    }
#undef GI
    A.slot = fe_ini_get_int(ini, SEC_UI, "slot", 1);
    if (A.slot < 1 || A.slot > FE_SLOTS) A.slot = 1;
    v = fe_ini_get(ini, SEC_AUDIO, "device");
    if (v) snprintf(s->audio_device, sizeof(s->audio_device), "%s", v);
    for (int i = 0; i < A.g->input_count; i++) load_binding(SEC_INPUT, A.g->inputs[i].id, &A.bind[i]);
    for (int i = 0; i < FE_HOTKEY_COUNT; i++) load_binding(SEC_HOTKEY, HOTKEY_ID[i], &A.hk[i]);
    for (int i = 0; i < A.g->dip_words; i++) {
        char k[16];
        snprintf(k, sizeof(k), "dsw%d", i + 1);
        s->dips[i] = (uint16_t)fe_ini_get_int(ini, SEC_GAME, k, s->dips[i]);
    }
    for (int i = 0; i < gadget_total(); i++) {
        v = fe_ini_get(ini, SEC_GADGET, gadget_id(i));
        gadget_cfg *g = &s->gadget[i];
        if (v) sscanf(v, "%d,%d,%d,%d,%d", &g->enabled, &g->anchor, &g->size, &g->dx, &g->dy);
    }
    /* clamp everything read from disk */
#define CL(x, lo, hi) do { if ((x) < (lo)) (x) = (lo); if ((x) > (hi)) (x) = (hi); } while (0)
    CL(s->window_scale, 1, 8); CL(s->scale_mode, 0, 1); CL(s->aspect_mode, 0, 1); CL(s->filter, 0, 1);
    CL(s->scanlines, 0, 100); CL(s->mask, 0, 3); CL(s->mask_strength, 0, 100);
    CL(s->brightness, 50, 150); CL(s->gamma, 50, 200); CL(s->border, 0, 2); CL(s->rotation, 0, 3);
    CL(s->volume, 0, 100); CL(s->latency_ms, 16, 250); CL(s->accent, 0, ACCENT_COUNT - 1);
    CL(s->rewind_mb, 16, 512); CL(s->deadzone, 5, 90); CL(s->menu_combo, 0, 3);
#undef CL
}

/* ------------------------------------------------------------ directories */

static void setup_dirs(int portable)
{
    char *base = NULL;
    char exe_dir[FE_PATH_MAX] = "";
    char *bp = SDL_GetBasePath();
    if (bp) { snprintf(exe_dir, sizeof(exe_dir), "%s", bp); SDL_free(bp); }
    char marker[FE_PATH_MAX];
    fe_path_join(marker, sizeof(marker), exe_dir, "portable.txt");
    /* An AppImage mounts read-only, so portable mode is honoured only when
     * the directory is writable (APPIMAGE unset). */
    if ((portable || fe_file_exists(marker)) && exe_dir[0] && !getenv("APPIMAGE")) {
        snprintf(A.base_dir, sizeof(A.base_dir), "%s", exe_dir);
    } else {
        /* the folder is named after the title, minus characters Windows
         * does not allow in file names ("Cyvern: The ..." -> "Cyvern - The ...") */
        char folder[128];
        size_t o = 0;
        for (const char *c = A.g->title; *c && o + 3 < sizeof(folder); c++) {
            if (*c == ':') { if (o && folder[o - 1] == ' ') o--; memcpy(folder + o, " -", 2); o += 2; }
            else if (strchr("<>\"/\\|?*", *c)) folder[o++] = '_';
            else folder[o++] = *c;
        }
        folder[o] = 0;
        base = SDL_GetPrefPath("OmegaKatana", folder);
        if (base) { snprintf(A.base_dir, sizeof(A.base_dir), "%s", base); SDL_free(base); }
        else snprintf(A.base_dir, sizeof(A.base_dir), "%s", exe_dir);
    }
    fe_mkdirs(A.base_dir);
    fe_path_join(A.config_path, sizeof(A.config_path), A.base_dir, "settings.ini");
    fe_path_join(A.states_dir, sizeof(A.states_dir), A.base_dir, "states");
    fe_path_join(A.shots_dir, sizeof(A.shots_dir), A.base_dir, "screenshots");
    fe_path_join(A.nvram_dir, sizeof(A.nvram_dir), A.base_dir, "nvram");
    fe_path_join(A.movies_dir, sizeof(A.movies_dir), A.base_dir, "movies");
    fe_path_join(A.log_path, sizeof(A.log_path), A.base_dir, "frontend.log");
    fe_mkdirs(A.states_dir);
    fe_mkdirs(A.shots_dir);
    fe_mkdirs(A.movies_dir);
    if (A.g->nvram_save) fe_mkdirs(A.nvram_dir);
}

/* ------------------------------------------------------------------ NVRAM */

/* One file per game and ROM fingerprint, e.g. nvram/viostorm-1234ABCD.nv. */
static void nvram_path(char *out, size_t n)
{
    char name[64];
    snprintf(name, sizeof(name), "%s-%08X.nv", A.g->id, A.g->rom_fingerprint ? A.g->rom_fingerprint() : 0u);
    fe_path_join(out, n, A.nvram_dir, name);
}

static void nvram_restore(void)
{
    if (!A.g->nvram_load || !A.rom_ok) return;
    char path[FE_PATH_MAX];
    nvram_path(path, sizeof(path));
    size_t size = 0;
    uint8_t *data = fe_read_file(path, &size);
    if (!data) return;                       /* first run: the game keeps its factory defaults */
    if (A.g->nvram_load(data, size)) fe_log("NVRAM restored from %s", path);
    else fe_log("NVRAM file %s ignored (unexpected size %zu)", path, size);
    free(data);
}

static void nvram_store(int force)
{
    if (!A.g->nvram_save || !A.rom_ok) return;
    char path[FE_PATH_MAX];
    nvram_path(path, sizeof(path));
    if (!fe_file_exists(path)) force = 1;      /* first run: keep the factory contents too */
    if (!force && !(A.g->nvram_dirty && A.g->nvram_dirty())) return;
    uint8_t buf[65536];
    size_t n = A.g->nvram_save(buf, sizeof(buf));
    if (!n) return;
    if (!fe_write_file_atomic(path, buf, n)) fe_log("NVRAM save failed: %s", path);
}

/* States are kept per ROM fingerprint so different revisions never mix. */
static void slot_path(int slot, char *out, size_t n)
{
    char dir[FE_PATH_MAX], sub[32], name[32];
    snprintf(sub, sizeof(sub), "%s-%08X", A.g->id, A.g->rom_fingerprint ? A.g->rom_fingerprint() : 0u);
    fe_path_join(dir, sizeof(dir), A.states_dir, sub);
    fe_mkdirs(dir);
    if (slot == FE_SLOT_AUTO) snprintf(name, sizeof(name), "autosave.fest");
    else snprintf(name, sizeof(name), "slot%02d.fest", slot);
    fe_path_join(out, n, dir, name);
}

/* --------------------------------------------------------------- display */

static void rebuild_lut(void)
{
    double gamma = A.s.gamma / 100.0, bright = A.s.brightness / 100.0;
    for (int i = 0; i < 256; i++) {
        double v = i / 255.0;
        v = pow(v, 1.0 / gamma) * bright;
        if (v > 1.0) v = 1.0;
        A.ramp[i] = (uint8_t)(v * 255.0 + 0.5);
    }
    A.lut_dirty = 0;
}

static void apply_filter(void)
{
#if SDL_VERSION_ATLEAST(2, 0, 12)
    if (A.game_tex) SDL_SetTextureScaleMode(A.game_tex, A.s.filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
}

static void apply_vsync(void)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    SDL_RenderSetVSync(A.renderer, A.s.vsync ? 1 : 0);
#endif
}

/* Window size for a scale factor: the picture as displayed (rotation applied). */
static void window_size(int scale, int *w, int *h)
{
    int pw = (int)(A.g->height * scale * A.g->display_aspect + 0.5), ph = A.g->height * scale;
    if (A.s.rotation & 1) { *w = ph; *h = pw; } else { *w = pw; *h = ph; }
}

/* Draw a game-sized texture into a box, keeping the displayed aspect and
 * applying the current rotation (menu previews and slot thumbnails). */
static void draw_fitted(SDL_Texture *tex, int x, int y, int w, int h)
{
    int rotated = A.s.rotation & 1;
    double aspect = rotated ? 1.0 / A.g->display_aspect : A.g->display_aspect;
    int pw = w, ph = (int)(w / aspect);
    if (ph > h) { ph = h; pw = (int)(h * aspect); }
    int cx = x + w / 2, cy = y + h / 2;
    SDL_Rect r = rotated ? (SDL_Rect){ cx - ph / 2, cy - pw / 2, ph, pw } : (SDL_Rect){ cx - pw / 2, cy - ph / 2, pw, ph };
    SDL_RenderCopyEx(A.renderer, tex, NULL, &r, 90.0 * A.s.rotation, NULL, SDL_FLIP_NONE);
}

static void apply_fullscreen(void)
{
    SDL_SetWindowFullscreen(A.window, A.s.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!A.s.fullscreen) {
        int w, h;
        window_size(A.s.window_scale, &w, &h);
        SDL_SetWindowSize(A.window, w, h);
    }
}

/* Destination rectangle of the game picture (unrotated size). */
static void game_rect(int ww, int wh, SDL_Rect *out)
{
    int rotated = A.s.rotation & 1;
    double aspect = A.s.aspect_mode == 0 ? A.g->display_aspect : (double)A.g->width / (double)A.g->height;
    int avail_w = rotated ? wh : ww, avail_h = rotated ? ww : wh;
    int w, h;
    if (A.s.scale_mode == 1) {
        int k = avail_h / A.g->height;
        if (k < 1) k = 1;
        for (;;) {
            h = A.g->height * k;
            w = A.s.aspect_mode == 0 ? (int)(h * aspect + 0.5) : A.g->width * k;
            if ((w <= avail_w && h <= avail_h) || k == 1) break;
            k--;
        }
    } else {
        h = avail_h;
        w = (int)(h * aspect + 0.5);
        if (w > avail_w) { w = avail_w; h = (int)(w / aspect + 0.5); }
    }
    out->w = w; out->h = h;
    out->x = (ww - w) / 2; out->y = (wh - h) / 2;
}

static void ensure_overlays(const SDL_Rect *dst)
{
    if (!A.scan_tex) {
        int h = A.g->height * 2;
        A.scan_tex = SDL_CreateTexture(A.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1, h);
        uint32_t *px = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)h);
        for (int y = 0; y < h; y++) px[y] = (y & 1) ? 0xFF000000u : 0x00000000u;
        SDL_UpdateTexture(A.scan_tex, NULL, px, 4);
        free(px);
        SDL_SetTextureBlendMode(A.scan_tex, SDL_BLENDMODE_BLEND);
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetTextureScaleMode(A.scan_tex, SDL_ScaleModeLinear);
#endif
    }
    if (A.s.mask && (A.mask_w != dst->w || A.mask_h != dst->h || A.mask_kind != A.s.mask || A.mask_level != A.s.mask_strength)) {
        if (A.mask_tex) SDL_DestroyTexture(A.mask_tex);
        A.mask_tex = SDL_CreateTexture(A.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, dst->w, dst->h);
        if (A.mask_tex) {
            uint32_t *px = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)dst->w * (size_t)dst->h);
            int dim = 255 - A.s.mask_strength * 255 / 100 * 2 / 3;
            for (int y = 0; y < dst->h; y++) {
                for (int x = 0; x < dst->w; x++) {
                    int r = 255, g = 255, b = 255;
                    if (A.s.mask == 1) {          /* aperture grille: vertical RGB stripes */
                        int k = x % 3;
                        r = k == 0 ? 255 : dim; g = k == 1 ? 255 : dim; b = k == 2 ? 255 : dim;
                    } else if (A.s.mask == 2) {   /* shadow mask: staggered RGB triads */
                        int k = (x + (y / 2 % 2) * 2) % 3;
                        r = k == 0 ? 255 : dim; g = k == 1 ? 255 : dim; b = k == 2 ? 255 : dim;
                        if (y % 2 == 1) { r = r * 9 / 10; g = g * 9 / 10; b = b * 9 / 10; }
                    } else {                      /* slot mask */
                        int k = x % 3;
                        r = k == 0 ? 255 : dim; g = k == 1 ? 255 : dim; b = k == 2 ? 255 : dim;
                        if ((y % 4 == 3 && (x / 3) % 2 == 0) || (y % 4 == 1 && (x / 3) % 2 == 1)) { r = g = b = dim; }
                    }
                    px[(size_t)y * (size_t)dst->w + (size_t)x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                }
            }
            SDL_UpdateTexture(A.mask_tex, NULL, px, dst->w * 4);
            free(px);
            SDL_SetTextureBlendMode(A.mask_tex, SDL_BLENDMODE_MOD);
        }
        A.mask_w = dst->w; A.mask_h = dst->h; A.mask_kind = A.s.mask; A.mask_level = A.s.mask_strength;
    }
}

/* Ask the game for the current frame in xRGB8888, whichever format it renders. */
static void render_frame(void)
{
    if (A.g->render32) { A.g->render32(A.pixels); return; }
    A.g->render(A.pixels555);
    int n = A.g->width * A.g->height;
    for (int i = 0; i < n; i++) {
        uint16_t c = A.pixels555[i];
        int r = c >> 10 & 31, g = c >> 5 & 31, b = c & 31;
        A.pixels[i] = (uint32_t)(r << 3 | r >> 2) << 16 | (uint32_t)(g << 3 | g >> 2) << 8 | (uint32_t)(b << 3 | b >> 2);
    }
}

static void upload_frame(void)
{
    if (A.lut_dirty) rebuild_lut();
    int n = A.g->width * A.g->height;
    for (int i = 0; i < n; i++) {
        uint32_t c = A.pixels[i];
        A.argb[i] = 0xFF000000u | (uint32_t)A.ramp[c >> 16 & 0xFF] << 16 | (uint32_t)A.ramp[c >> 8 & 0xFF] << 8 | A.ramp[c & 0xFF];
    }
    SDL_UpdateTexture(A.game_tex, NULL, A.argb, A.g->width * 4);
}

static void draw_background(int ww, int wh)
{
    if (A.s.border == 0) {
        fe_fill(A.renderer, 0, 0, ww, wh, rgba(0, 0, 0, 255));
    } else if (A.s.border == 1 || !A.session) {
        fe_gradient_v(A.renderer, 0, 0, ww, wh, rgba(18, 21, 30, 255), rgba(6, 7, 11, 255));
        /* faint accent glow lines: original wallpaper */
        fe_color a = accent();
        for (int i = 0; i < 6; i++) {
            int y = wh * (i + 1) / 7;
            fe_fill(A.renderer, 0, y, ww, 1, with_alpha(a, 10));
        }
    } else {
        /* ambient: the frame stretched to the window, dimmed; linear filtering blurs it */
        if (!A.ambient_tex) {
            A.ambient_tex = SDL_CreateTexture(A.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 24, 18);
#if SDL_VERSION_ATLEAST(2, 0, 12)
            if (A.ambient_tex) SDL_SetTextureScaleMode(A.ambient_tex, SDL_ScaleModeLinear);
#endif
        }
        if (A.ambient_tex && SDL_SetRenderTarget(A.renderer, A.ambient_tex) == 0) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
            SDL_SetTextureScaleMode(A.game_tex, SDL_ScaleModeLinear);
#endif
            SDL_RenderCopy(A.renderer, A.game_tex, NULL, NULL);
            apply_filter();
            SDL_SetRenderTarget(A.renderer, NULL);
            SDL_SetTextureColorMod(A.ambient_tex, 70, 70, 80);
            SDL_RenderCopy(A.renderer, A.ambient_tex, NULL, NULL);
        } else {
            fe_fill(A.renderer, 0, 0, ww, wh, rgba(0, 0, 0, 255));
        }
    }
}

static void draw_game(int ww, int wh, int dim)
{
    SDL_Rect dst;
    game_rect(ww, wh, &dst);
    double angle = 90.0 * A.s.rotation;
    if (dim) SDL_SetTextureColorMod(A.game_tex, 90, 90, 100);
    else SDL_SetTextureColorMod(A.game_tex, 255, 255, 255);
    SDL_RenderCopyEx(A.renderer, A.game_tex, NULL, &dst, angle, NULL, SDL_FLIP_NONE);
    if (A.s.scanlines > 0) {
        ensure_overlays(&dst);
        SDL_SetTextureAlphaMod(A.scan_tex, (uint8_t)(A.s.scanlines * 255 / 100));
        SDL_RenderCopyEx(A.renderer, A.scan_tex, NULL, &dst, angle, NULL, SDL_FLIP_NONE);
    }
    if (A.s.mask > 0) {
        ensure_overlays(&dst);
        if (A.mask_tex) SDL_RenderCopyEx(A.renderer, A.mask_tex, NULL, &dst, angle, NULL, SDL_FLIP_NONE);
    }
}

/* ------------------------------------------------------------------ audio */

static void audio_close(void)
{
    if (A.audio_dev) SDL_CloseAudioDevice(A.audio_dev);
    A.audio_dev = 0;
    if (A.audio_stream) SDL_FreeAudioStream(A.audio_stream);
    A.audio_stream = NULL;
}

static void audio_open(void)
{
    audio_close();
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    int samples = 256;
    while (samples * 1000 / want.freq < A.s.latency_ms / 4 && samples < 4096) samples *= 2;
    want.samples = (Uint16)samples;
    const char *dev = A.s.audio_device[0] ? A.s.audio_device : NULL;
    A.audio_dev = SDL_OpenAudioDevice(dev, 0, &want, &A.audio_have,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (!A.audio_dev && dev) {
        fe_log("audio device '%s' unavailable (%s); using default", dev, SDL_GetError());
        A.audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &A.audio_have,
                                          SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    }
    if (!A.audio_dev) { fe_log("audio disabled: %s", SDL_GetError()); return; }
    A.audio_stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, (int)(A.g->audio_rate + 0.5),
                                        A.audio_have.format, A.audio_have.channels, A.audio_have.freq);
    fe_log("audio: %d Hz, %d ch, buffer %d samples, target latency %d ms",
           A.audio_have.freq, A.audio_have.channels, A.audio_have.samples, A.s.latency_ms);
    SDL_PauseAudioDevice(A.audio_dev, 0);
}

static void audio_flush(void)
{
    if (A.audio_dev) SDL_ClearQueuedAudio(A.audio_dev);
    if (A.audio_stream) SDL_AudioStreamClear(A.audio_stream);
}

static void audio_push(const int16_t *stereo, int frames)
{
    if (!A.audio_dev || !A.audio_stream || frames <= 0) return;
    static int16_t scaled[4096 * 2];
    if (frames > 4096) frames = 4096;
    int vol = A.s.mute ? 0 : A.s.volume * 256 / 100;
    for (int i = 0; i < frames * 2; i++) scaled[i] = (int16_t)((stereo[i] * vol) >> 8);
    SDL_AudioStreamPut(A.audio_stream, scaled, frames * 4);
    uint8_t out[16384];
    int bytes_per_ms = A.audio_have.freq * A.audio_have.channels * (int)(SDL_AUDIO_BITSIZE(A.audio_have.format) / 8) / 1000;
    for (;;) {
        int got = SDL_AudioStreamGet(A.audio_stream, out, sizeof(out));
        if (got <= 0) break;
        Uint32 queued = SDL_GetQueuedAudioSize(A.audio_dev);
        if (queued == 0 && A.frames_total > 120) A.audio_underruns++;
        /* Bound latency: if far above target (e.g. after a stall), drop this
         * chunk instead of letting delay accumulate. */
        if (bytes_per_ms > 0 && (int)queued > bytes_per_ms * (A.s.latency_ms * 2 + 40)) { A.audio_drops++; continue; }
        SDL_QueueAudio(A.audio_dev, out, (Uint32)got);
    }
}

/* ---------------------------------------------------------------- input */

static void close_pads(void)
{
    for (int i = 0; i < A.pad_count; i++) if (A.pads[i]) SDL_GameControllerClose(A.pads[i]);
    A.pad_count = 0;
}

static void open_pads(void)
{
    close_pads();
    for (int i = 0; i < SDL_NumJoysticks() && A.pad_count < 8; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController *p = SDL_GameControllerOpen(i);
        if (p) {
            A.pads[A.pad_count++] = p;
            fe_log("controller %d: %s", A.pad_count, SDL_GameControllerName(p));
        }
    }
}

static int pad_active(SDL_GameController *p, int code)
{
    if (!p || code < 0) return 0;
    if (code >= FE_PAD_AXIS_BASE) {
        int axis = (code - FE_PAD_AXIS_BASE) / 2, pos = (code - FE_PAD_AXIS_BASE) & 1;
        int v = SDL_GameControllerGetAxis(p, (SDL_GameControllerAxis)axis);
        int th = A.s.deadzone * 32767 / 100;
        return pos ? v > th : v < -th;
    }
    return SDL_GameControllerGetButton(p, (SDL_GameControllerButton)code);
}

/* Left stick doubles as the D-pad for directional inputs. */
static int pad_dir_from_stick(SDL_GameController *p, int code)
{
    switch (code) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return pad_active(p, FE_PAD_AXIS_BASE + SDL_CONTROLLER_AXIS_LEFTY * 2);
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return pad_active(p, FE_PAD_AXIS_BASE + SDL_CONTROLLER_AXIS_LEFTY * 2 + 1);
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return pad_active(p, FE_PAD_AXIS_BASE + SDL_CONTROLLER_AXIS_LEFTX * 2);
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return pad_active(p, FE_PAD_AXIS_BASE + SDL_CONTROLLER_AXIS_LEFTX * 2 + 1);
    default: return 0;
    }
}

static int feat(int f) { return A.feat[f]; }

static int game_players(void)
{
    int n = A.g && A.g->players > 0 ? A.g->players : 2;
    return n > FE_MAX_PLAYERS ? FE_MAX_PLAYERS : n;
}

static SDL_GameController *pad_for_player(int player)
{
    if (player < 1 || player > FE_MAX_PLAYERS) return NULL;
    int idx = A.s.pad_player[player - 1];
    if (idx < 0) idx = player - 1;
    return idx < A.pad_count ? A.pads[idx] : NULL;
}

static int binding_down(const binding *b, int player, const uint8_t *keys)
{
    for (int k = 0; k < 2; k++) if (b->key[k] >= 0 && b->key[k] < SDL_NUM_SCANCODES && keys[b->key[k]]) return 1;
    if (b->pad < 0) return 0;
    if (player == 0) {
        for (int i = 0; i < A.pad_count; i++) if (pad_active(A.pads[i], b->pad)) return 1;
        return 0;
    }
    SDL_GameController *p = pad_for_player(player);
    return pad_active(p, b->pad) || pad_dir_from_stick(p, b->pad);
}

static int menu_combo_down(void)
{
    for (int i = 0; i < A.pad_count; i++) {
        SDL_GameController *p = A.pads[i];
        switch (A.s.menu_combo) {
        case 0: if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_BACK) && SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_START)) return 1; break;
        case 1: if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_GUIDE)) return 1; break;
        case 2: if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) return 1; break;
        case 3: if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_LEFTSTICK) && SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) return 1; break;
        }
    }
    return 0;
}

static void clear_game_inputs(void)
{
    A.game_mask = 0;
    memset(A.impulse, 0, sizeof(A.impulse));
    for (int i = 0; i < A.g->input_count; i++) A.prev_pressed[i] = 1;   /* require a fresh press */
    /* only once a ROM is loaded: the adapter has no board before that */
    if (A.g->set_inputs && A.rom_ok) A.g->set_inputs(0);
}

/* Sample bindings once per emulated frame. */
static uint32_t sample_game_inputs(void)
{
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    uint32_t mask = 0;
    int combo = menu_combo_down();
    for (int i = 0; i < A.g->input_count; i++) {
        const fe_input_def *d = &A.g->inputs[i];
        int down = binding_down(&A.bind[i], d->player, keys);
        if (combo || (A.suppress_until_release && down)) down = 0;
        if (d->impulse_frames > 0) {
            if (down && !A.prev_pressed[i]) A.impulse[i] = d->impulse_frames;
            if (A.impulse[i] > 0) { mask |= d->mask; A.impulse[i]--; }
        } else if (down) {
            mask |= d->mask;
        }
        A.prev_pressed[i] = down;
    }
    if (A.suppress_until_release) {
        int any = 0;
        for (int i = 0; i < A.g->input_count; i++) any |= binding_down(&A.bind[i], A.g->inputs[i].player, keys);
        if (!any) A.suppress_until_release = 0;
    }
    return mask;
}

/* ------------------------------------------------------------- save states */

static void put_u32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Container: magic, version, game id, runtime version, ROM fingerprint,
 * timestamp, frame count, thumbnail (RGB565), core state, CRC32. */
static uint8_t *build_state_file(size_t *out_size)
{
    size_t core_size = 0;
    uint8_t *core = A.g->save_state(&core_size);
    if (!core) return NULL;
    int tw = A.g->width / FE_THUMB_DIV, th = A.g->height / FE_THUMB_DIV;
    size_t header = 8 + 4 + 16 + 32 + 4 + 8 + 4 + 4;
    size_t total = header + (size_t)tw * th * 2 + 4 + core_size + 4;
    uint8_t *f = (uint8_t *)calloc(1, total);
    if (!f) { free(core); return NULL; }
    size_t n = 0;
    memcpy(f, FE_STATE_MAGIC, 8); n = 8;
    put_u32(f + n, FE_STATE_CONTAINER); n += 4;
    snprintf((char *)f + n, 16, "%s", A.g->id); n += 16;
    snprintf((char *)f + n, 32, "%s", A.g->runtime_version ? A.g->runtime_version : ""); n += 32;
    put_u32(f + n, A.g->rom_fingerprint ? A.g->rom_fingerprint() : 0); n += 4;
    int64_t t = (int64_t)time(NULL);
    put_u32(f + n, (uint32_t)t); put_u32(f + n + 4, (uint32_t)((uint64_t)t >> 32)); n += 8;
    put_u32(f + n, (uint32_t)A.frames_total); n += 4;
    f[n] = (uint8_t)tw; f[n + 1] = (uint8_t)(tw >> 8); f[n + 2] = (uint8_t)th; f[n + 3] = (uint8_t)(th >> 8); n += 4;
    for (int y = 0; y < th; y++) {
        for (int x = 0; x < tw; x++) {
            uint32_t c = A.pixels[(y * FE_THUMB_DIV) * A.g->width + x * FE_THUMB_DIV];
            uint16_t rgb565 = (uint16_t)(((c >> 19 & 31) << 11) | ((c >> 10 & 63) << 5) | (c >> 3 & 31));
            f[n++] = (uint8_t)rgb565; f[n++] = (uint8_t)(rgb565 >> 8);
        }
    }
    put_u32(f + n, (uint32_t)core_size); n += 4;
    memcpy(f + n, core, core_size); n += core_size;
    free(core);
    put_u32(f + n, (uint32_t)mz_crc32(MZ_CRC32_INIT, f, n)); n += 4;
    *out_size = n;
    return f;
}

/* Parse and check a container; returns pointer to the core blob. */
static const uint8_t *parse_state_file(const uint8_t *f, size_t size, slot_info *info, size_t *core_size, char *err, size_t en)
{
    size_t header = 8 + 4 + 16 + 32 + 4 + 8 + 4 + 4;
    if (size < header + 8) { snprintf(err, en, "file is truncated"); return NULL; }
    if (memcmp(f, FE_STATE_MAGIC, 8)) { snprintf(err, en, "not a save state"); return NULL; }
    if (get_u32(f + size - 4) != (uint32_t)mz_crc32(MZ_CRC32_INIT, f, size - 4)) { snprintf(err, en, "file is damaged (checksum mismatch)"); return NULL; }
    if (get_u32(f + 8) != FE_STATE_CONTAINER) { snprintf(err, en, "unsupported container version %u", get_u32(f + 8)); return NULL; }
    char gid[17] = { 0 };
    memcpy(gid, f + 12, 16);
    if (strcmp(gid, A.g->id)) { snprintf(err, en, "state belongs to another game (%s)", gid); return NULL; }
    uint32_t fp = get_u32(f + 60);
    if (A.g->rom_fingerprint && fp != A.g->rom_fingerprint()) { snprintf(err, en, "state was made with a different ROM revision"); return NULL; }
    size_t n = 64;
    int64_t t = (int64_t)((uint64_t)get_u32(f + n) | ((uint64_t)get_u32(f + n + 4) << 32)); n += 8;
    uint32_t frames = get_u32(f + n); n += 4;
    int tw = f[n] | (f[n + 1] << 8), th = f[n + 2] | (f[n + 3] << 8); n += 4;
    if (tw <= 0 || th <= 0 || tw > 1024 || th > 1024 || n + (size_t)tw * th * 2 + 4 > size - 4) { snprintf(err, en, "file is damaged (thumbnail)"); return NULL; }
    if (info) { info->timestamp = t; info->frames = frames; info->thumb_w = tw; info->thumb_h = th; }
    const uint8_t *thumb = f + n;
    n += (size_t)tw * th * 2;
    size_t cs = get_u32(f + n); n += 4;
    if (n + cs + 4 != size) { snprintf(err, en, "file is damaged (size mismatch)"); return NULL; }
    if (info && A.renderer) {
        if (info->thumb) SDL_DestroyTexture(info->thumb);
        info->thumb = SDL_CreateTexture(A.renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, tw, th);
        if (info->thumb) SDL_UpdateTexture(info->thumb, NULL, thumb, tw * 2);
    }
    if (A.g->validate_state && !A.g->validate_state(f + n, cs, err, en)) return NULL;
    *core_size = cs;
    return f + n;
}

static void refresh_slots(void)
{
    for (int i = 0; i <= FE_SLOTS; i++) {
        slot_info *si = &A.slots[i];
        si->exists = si->valid = 0;
        si->error[0] = 0;
        char path[FE_PATH_MAX];
        slot_path(i, path, sizeof(path));
        size_t size;
        uint8_t *f = fe_read_file(path, &size);
        if (!f) { if (si->thumb) { SDL_DestroyTexture(si->thumb); si->thumb = NULL; } continue; }
        si->exists = 1;
        size_t cs;
        si->valid = parse_state_file(f, size, si, &cs, si->error, sizeof(si->error)) != NULL;
        free(f);
    }
    A.slots_dirty = 0;
}

static int save_slot(int slot)
{
    size_t size;
    uint8_t *f = build_state_file(&size);
    if (!f) { toast("Save failed: could not serialize the game state"); return 0; }
    char path[FE_PATH_MAX];
    slot_path(slot, path, sizeof(path));
    int ok = fe_write_file_atomic(path, f, size);
    free(f);
    A.slots_dirty = 1;
    if (!ok) toast("Save failed: cannot write %s", path);
    else if (slot == FE_SLOT_AUTO) fe_log("autosaved to %s", path);
    else toast("Saved to slot %d", slot);
    return ok;
}

static int load_slot(int slot, int quiet)
{
    char path[FE_PATH_MAX], err[200] = "";
    slot_path(slot, path, sizeof(path));
    size_t size;
    uint8_t *f = fe_read_file(path, &size);
    if (!f) { if (!quiet) toast(slot ? "Slot %d is empty" : "No autosave yet", slot); return 0; }
    size_t cs;
    const uint8_t *core = parse_state_file(f, size, NULL, &cs, err, sizeof(err));
    int ok = core && A.g->load_state(core, cs, err, sizeof(err));
    free(f);
    if (!ok) {
        show_message("Cannot load state", "%s %s could not be loaded:\n%s\n\nThe running game was not changed.",
                     slot ? "Slot" : "The autosave", slot ? "" : "", err);
        return 0;
    }
    A.session = 1;
    clear_game_inputs();
    A.suppress_until_release = 1;
    audio_flush();
    render_frame();
    upload_frame();
    A.accumulator = 0;
    if (!quiet) toast(slot ? "Loaded slot %d" : "Continued from autosave", slot);
    return 1;
}

/* ----------------------------------------------------------------- rewind */

static void rewind_free(void)
{
    for (int i = 0; i < A.rw.cap; i++) free(A.rw.snap ? A.rw.snap[i] : NULL);
    free(A.rw.snap); free(A.rw.size);
    memset(&A.rw, 0, sizeof(A.rw));
}

static void rewind_reset(void)
{
    rewind_free();
    if (!A.s.rewind_enabled) return;
    A.rw.cap = 4096;
    A.rw.snap = (uint8_t **)calloc((size_t)A.rw.cap, sizeof(uint8_t *));
    A.rw.size = (size_t *)calloc((size_t)A.rw.cap, sizeof(size_t));
    A.rw.budget = (size_t)A.s.rewind_mb * 1024u * 1024u;
}

static void rewind_capture(void)
{
    if (!A.rw.snap) return;
    size_t raw_size;
    uint8_t *raw = A.g->save_state(&raw_size);
    if (!raw) return;
    mz_ulong zs = mz_compressBound((mz_ulong)raw_size);
    uint8_t *z = (uint8_t *)malloc(zs + 8);
    if (z && mz_compress2(z + 8, &zs, raw, (mz_ulong)raw_size, 1) == MZ_OK) {
        put_u32(z, (uint32_t)raw_size);
        put_u32(z + 4, (uint32_t)zs);
        int slot = (A.rw.head + A.rw.count) % A.rw.cap;
        if (A.rw.count == A.rw.cap) { A.rw.bytes -= A.rw.size[A.rw.head]; free(A.rw.snap[A.rw.head]); A.rw.snap[A.rw.head] = NULL; A.rw.head = (A.rw.head + 1) % A.rw.cap; A.rw.count--; slot = (A.rw.head + A.rw.count) % A.rw.cap; }
        A.rw.snap[slot] = z; A.rw.size[slot] = zs + 8; A.rw.bytes += zs + 8; A.rw.count++;
        while (A.rw.bytes > A.rw.budget && A.rw.count > 1) {
            A.rw.bytes -= A.rw.size[A.rw.head];
            free(A.rw.snap[A.rw.head]); A.rw.snap[A.rw.head] = NULL;
            A.rw.head = (A.rw.head + 1) % A.rw.cap;
            A.rw.count--;
        }
    } else {
        free(z);
    }
    free(raw);
}

static int rewind_step(void)
{
    if (!A.rw.snap || A.rw.count <= 1) return 0;
    int slot = (A.rw.head + A.rw.count - 1) % A.rw.cap;
    uint8_t *z = A.rw.snap[slot];
    mz_ulong raw_size = get_u32(z), zs = get_u32(z + 4);
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    char err[128];
    int ok = raw && mz_uncompress(raw, &raw_size, z + 8, zs) == MZ_OK && A.g->load_state(raw, raw_size, err, sizeof(err));
    free(raw);
    if (A.rw.count > 1) { A.rw.bytes -= A.rw.size[slot]; free(z); A.rw.snap[slot] = NULL; A.rw.count--; }
    return ok;
}

/* ----------------------------------------------------------------- movies */

static void movie_stop(void)
{
    if (A.movie_rec) { fclose(A.movie_rec); A.movie_rec = NULL; toast("Recording saved"); }
    if (A.movie_play) { free(A.movie_play); A.movie_play = NULL; toast("Playback finished"); }
    A.movie_status[0] = 0;
}

static void movie_record_start(void)
{
    movie_stop();
    size_t size;
    uint8_t *state = build_state_file(&size);
    if (!state) { toast("Cannot start recording"); return; }
    char name[64], path[FE_PATH_MAX];
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(name, sizeof(name), "movie-%Y%m%d-%H%M%S.femv", &tmv);
    fe_path_join(path, sizeof(path), A.movies_dir, name);
    A.movie_rec = fopen(path, "wb");
    if (!A.movie_rec) { free(state); toast("Cannot write %s", path); return; }
    uint8_t hdr[12];
    memcpy(hdr, "FEMOVIE1", 8);
    put_u32(hdr + 8, (uint32_t)size);
    fwrite(hdr, 1, 12, A.movie_rec);
    fwrite(state, 1, size, A.movie_rec);
    free(state);
    snprintf(A.movie_status, sizeof(A.movie_status), "REC %s", name);
    toast("Recording inputs to %s", name);
}

static int movie_latest(char *out, size_t n)
{
    fe_dirent *list = NULL;
    int count = fe_list_dir(A.movies_dir, &list);
    int found = 0;
    char best[512] = "";
    for (int i = 0; i < count; i++) {
        const char *e = strrchr(list[i].name, '.');
        if (!list[i].is_dir && e && !strcmp(e, ".femv") && strcmp(list[i].name, best) > 0) {
            snprintf(best, sizeof(best), "%s", list[i].name);
            found = 1;
        }
    }
    free(list);
    if (found) fe_path_join(out, n, A.movies_dir, best);
    return found;
}

static void movie_play_latest(void)
{
    movie_stop();
    char path[FE_PATH_MAX], err[200] = "";
    if (!movie_latest(path, sizeof(path))) { toast("No recorded movies yet"); return; }
    size_t size;
    uint8_t *f = fe_read_file(path, &size);
    if (!f || size < 12 || memcmp(f, "FEMOVIE1", 8)) { free(f); toast("Movie file is not valid"); return; }
    size_t ssize = get_u32(f + 8);
    if (12 + ssize > size) { free(f); toast("Movie file is truncated"); return; }
    size_t cs;
    const uint8_t *core = parse_state_file(f + 12, ssize, NULL, &cs, err, sizeof(err));
    if (!core || !A.g->load_state(core, cs, err, sizeof(err))) { free(f); show_message("Cannot play movie", "%s", err); return; }
    A.movie_play = f;
    A.movie_play_size = size;
    A.movie_play_at = 12 + ssize;
    A.session = 1;
    clear_game_inputs();
    audio_flush();
    snprintf(A.movie_status, sizeof(A.movie_status), "PLAY %s", fe_basename(path));
}

/* ---------------------------------------------------------------- session */

static int load_rom(const char *path_in)
{
    char err[512] = "";
    char path[FE_PATH_MAX];
    snprintf(path, sizeof(path), "%s", path_in ? path_in : "");   /* path_in may alias A.rom_path */
    if (A.session) { A.session = 0; }
    nvram_store(0);
    if (A.g->unload) A.g->unload();
    A.rom_ok = 0;
    if (!*path) { snprintf(A.rom_error, sizeof(A.rom_error), "No ROM selected."); return 0; }
    if (!A.g->load(path, err, sizeof(err))) {
        snprintf(A.rom_error, sizeof(A.rom_error), "%s", err);
        fe_log("ROM %s rejected: %s", path, err);
        return 0;
    }
    snprintf(A.rom_path, sizeof(A.rom_path), "%s", path);
    A.rom_ok = 1;
    A.rom_error[0] = 0;
    nvram_restore();
    A.slots_dirty = 1;
    fe_log("ROM loaded: %s (fingerprint %08X)", path, A.g->rom_fingerprint ? A.g->rom_fingerprint() : 0);
    return 1;
}

static void start_new_game(void)
{
    if (!A.rom_ok) return;
    movie_stop();
    A.g->set_dips(A.s.dips);
    A.g->reset();
    A.session = 1;
    A.paused = 0;
    A.frames_total = 0;
    clear_game_inputs();
    A.suppress_until_release = 1;
    audio_flush();
    rewind_reset();
    A.accumulator = 0;
}

static void end_session(int autosave)
{
    nvram_store(0);
    if (A.session && autosave && feat(FE_FEAT_SAVE_STATES)) save_slot(FE_SLOT_AUTO);
    movie_stop();
    A.session = 0;
    clear_game_inputs();
    audio_flush();
}

static void screenshot(void)
{
    char name[64], path[FE_PATH_MAX];
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(name, sizeof(name), "shot-%Y%m%d-%H%M%S", &tmv);
    char file[80];
    snprintf(file, sizeof(file), "%s-%03u.png", name, (unsigned)(A.frames_total % 1000));
    fe_path_join(path, sizeof(path), A.shots_dir, file);
    int sw = A.g->width, sh = A.g->height, rot = A.s.rotation & 3;
    int w = (rot & 1) ? sh : sw, h = (rot & 1) ? sw : sh;   /* saved as displayed */
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx, sy;
            switch (rot) {
            case 1: sx = y; sy = sh - 1 - x; break;
            case 2: sx = sw - 1 - x; sy = sh - 1 - y; break;
            case 3: sx = sw - 1 - y; sy = x; break;
            default: sx = x; sy = y; break;
            }
            uint32_t c = A.pixels[sy * sw + sx];
            uint8_t *o = &rgb[((size_t)y * w + x) * 3];
            o[0] = (uint8_t)(c >> 16); o[1] = (uint8_t)(c >> 8); o[2] = (uint8_t)c;
        }
    }
    int ok = fe_write_png(path, rgb, w, h);
    free(rgb);
    if (ok) toast("Screenshot saved: %s", file);
    else toast("Screenshot failed: %s", path);
}

/* ------------------------------------------------------------------- UI */

static void push(screen_id s)
{
    if (A.depth < 16) A.stack[A.depth++] = s;
    A.screen_t0 = now_ticks();
    A.anim_y = -1;
    if (s == SCR_SAVE || s == SCR_LOAD) A.slots_dirty = 1;
}
static void pop(void) { if (A.depth > 0) A.depth--; A.screen_t0 = now_ticks(); A.anim_y = -1; }
static screen_id top(void) { return A.depth ? A.stack[A.depth - 1] : SCR_MAIN; }

static void open_menu(screen_id s)
{
    A.in_menu = 1;
    A.depth = 0;
    push(s);
    clear_game_inputs();
    audio_flush();
    if (A.audio_dev) SDL_PauseAudioDevice(A.audio_dev, 1);
}

static void close_menu(void)
{
    A.in_menu = 0;
    A.depth = 0;
    A.suppress_until_release = 1;
    clear_game_inputs();
    A.accumulator = 0;
    A.last_tick = now_ticks();
    if (A.audio_dev) SDL_PauseAudioDevice(A.audio_dev, 0);
}

static void show_message(const char *title, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.message_text, sizeof(A.message_text), fmt, ap);
    va_end(ap);
    snprintf(A.message_title, sizeof(A.message_title), "%s", title);
    fe_log("%s: %s", title, A.message_text);
    if (!A.in_menu) open_menu(SCR_MESSAGE);
    else push(SCR_MESSAGE);
}

static void ask(confirm_action action, int arg, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.confirm_text, sizeof(A.confirm_text), fmt, ap);
    va_end(ap);
    A.pending = action;
    A.pending_arg = arg;
    A.focus[SCR_CONFIRM] = 1;   /* default to "No" for destructive prompts */
    push(SCR_CONFIRM);
}

/* layout helpers ------------------------------------------------------- */

static int U(int v) { return v * A.ui_scale_px / 100; }

static void begin_list(const char *title, const char *subtitle)
{
    int ww = A.win_w, wh = A.win_h;
    double t = ms_since(A.screen_t0) / 160.0;
    if (t > 1) t = 1;
    int alpha = (int)(255 * t);
    int panel_w = ww * 46 / 100;
    if (panel_w < U(520)) panel_w = U(520);
    if (panel_w > ww - U(40)) panel_w = ww - U(40);
    int px = U(40), py = U(36);
    fe_color a = accent();
    fe_text(A.renderer, A.g->title, px, py, U(22), with_alpha(rgba(150, 156, 172, 255), alpha), 0);
    fe_text(A.renderer, title, px, py + U(30), U(44), with_alpha(rgba(244, 246, 250, 255), alpha), 0);
    fe_fill(A.renderer, px, py + U(86), U(64), U(4), with_alpha(a, alpha));
    if (subtitle && *subtitle) fe_text(A.renderer, subtitle, px + U(80), py + U(78), U(18), with_alpha(rgba(150, 156, 172, 255), alpha), 0);
    A.list_x = px;
    A.list_w = panel_w - px;
    A.row_h = U(48);
    A.list_top = py + U(110);
    A.list_bottom = wh - U(70);
    A.visible_rows = (A.list_bottom - A.list_top) / A.row_h;
    if (A.visible_rows < 3) A.visible_rows = 3;
    A.row = 0;
    A.row_y0 = A.list_top;
    A.help[0] = 0;
}

/* Returns 1 on activation; *delta gets -1/+1 for left/right on the focused row. */
static int row_item(const char *label, const char *value, int enabled, int *delta, const char *help)
{
    screen_id s = top();
    int index = A.row++;
    int focus = A.focus[s];
    int first = A.scroll[s];
    if (delta) *delta = 0;
    if (index < first || index >= first + A.visible_rows) return 0;
    int y = A.list_top + (index - first) * A.row_h;
    int x = A.list_x, w = A.list_w, h = A.row_h - U(6);
    /* mouse hover selects, click activates */
    int hovered = A.nv.mouse_x >= x && A.nv.mouse_x < x + w && A.nv.mouse_y >= y && A.nv.mouse_y < y + h;
    if (hovered && A.nv.mouse_moved) { A.focus[s] = index; focus = index; }
    fe_color a = accent();
    int focused = index == focus;
    if (focused) {
        if (A.anim_y < 0) A.anim_y = (float)y;
        A.anim_y += ((float)y - A.anim_y) * 0.35f;
        int ay = (int)(A.anim_y + 0.5f);
        fe_round_rect(A.renderer, x - U(12), ay, w + U(12), h, U(8), rgba(a.r / 5 + 20, a.g / 5 + 22, a.b / 5 + 30, 235));
        fe_fill(A.renderer, x - U(12), ay + U(8), U(4), h - U(16), a);
        if (help) snprintf(A.help, sizeof(A.help), "%s", help);
    }
    fe_color tc = enabled ? (focused ? rgba(255, 255, 255, 255) : rgba(206, 210, 220, 255)) : rgba(96, 100, 112, 255);
    int ty = y + (h - fe_text_line_height(U(24))) / 2;
    fe_text(A.renderer, label, x + U(8), ty, U(24), tc, 0);
    if (value) {
        fe_color vc = enabled ? (focused ? a : rgba(160, 166, 182, 255)) : rgba(96, 100, 112, 255);
        char buf[300];
        if (focused && delta && enabled) snprintf(buf, sizeof(buf), "<  %s  >", value);
        else snprintf(buf, sizeof(buf), "%s", value);
        fe_text(A.renderer, buf, x + w - U(16), ty, U(22), vc, 2);
    }
    if (!enabled || !focused) return 0;
    if (delta) {
        if (A.nv.left) *delta = -1;
        if (A.nv.right) *delta = 1;
    }
    if (A.nv.accept || (A.nv.click && hovered)) return 1;
    return 0;
}

/* Setting row: returns -1/+1 for Left/Right, +1 for Enter/click, else 0. */
static int row_choice(const char *label, const char *value, int enabled, const char *help)
{
    int d = 0;
    if (row_item(label, value, enabled, &d, help)) d = 1;
    return d;
}

static void end_list(void)
{
    screen_id s = top();
    int count = A.row;
    A.row_count = count;
    if (count == 0) return;
    if (A.nv.wheel) A.focus[s] -= A.nv.wheel;
    if (A.nv.up) A.focus[s]--;
    if (A.nv.down) A.focus[s]++;
    if (A.nv.page_up) A.focus[s] -= A.visible_rows;
    if (A.nv.page_down) A.focus[s] += A.visible_rows;
    if (A.focus[s] < 0) A.focus[s] = A.nv.up && !A.nv.page_up ? count - 1 : 0;
    if (A.focus[s] >= count) A.focus[s] = A.nv.down && !A.nv.page_down ? 0 : count - 1;
    if (A.focus[s] < A.scroll[s]) A.scroll[s] = A.focus[s];
    if (A.focus[s] >= A.scroll[s] + A.visible_rows) A.scroll[s] = A.focus[s] - A.visible_rows + 1;
    if (A.scroll[s] > count - A.visible_rows) A.scroll[s] = count - A.visible_rows > 0 ? count - A.visible_rows : 0;
    /* scrollbar */
    if (count > A.visible_rows) {
        int track = A.list_bottom - A.list_top;
        int bar = track * A.visible_rows / count;
        int by = A.list_top + track * A.scroll[s] / count;
        fe_fill(A.renderer, A.list_x + A.list_w + U(8), A.list_top, U(3), track, rgba(40, 44, 56, 255));
        fe_fill(A.renderer, A.list_x + A.list_w + U(8), by, U(3), bar, accent());
    }
    /* footer: help line and controls legend */
    int fy = A.win_h - U(52);
    fe_fill(A.renderer, 0, fy - U(10), A.win_w, 1, rgba(40, 44, 56, 255));
    if (A.help[0]) fe_text(A.renderer, A.help, U(40), fy, U(19), rgba(170, 176, 190, 255), 0);
    fe_text(A.renderer, "Enter/A  Select     Esc/B  Back     Left/Right  Change", A.win_w - U(40), fy, U(18), rgba(110, 116, 130, 255), 2);
}

static void right_panel_game_preview(void)
{
    int x = A.win_w * 50 / 100, y = U(140);
    int w = A.win_w - x - U(48), h = A.win_h - y - U(110);
    if (w < U(200) || h < U(150)) return;
    if (A.session || A.rom_ok) {
        double aspect = (A.s.rotation & 1) ? 1.0 / A.g->display_aspect : A.g->display_aspect;
        int pw = w, ph = (int)(w / aspect);
        if (ph > h) { ph = h; pw = (int)(h * aspect); }
        SDL_Rect r = { x + (w - pw) / 2, y + (h - ph) / 2, pw, ph };
        fe_round_rect(A.renderer, r.x - U(6), r.y - U(6), r.w + U(12), r.h + U(12), U(10), rgba(30, 34, 46, 255));
        if (A.session) {
            SDL_SetTextureColorMod(A.game_tex, 255, 255, 255);
            draw_fitted(A.game_tex, r.x, r.y, r.w, r.h);
        } else {
            fe_fill(A.renderer, r.x, r.y, r.w, r.h, rgba(10, 12, 18, 255));
            fe_text(A.renderer, "Ready", r.x + r.w / 2, r.y + r.h / 2 - U(20), U(30), rgba(200, 204, 214, 255), 1);
            fe_text(A.renderer, fe_basename(A.rom_path), r.x + r.w / 2, r.y + r.h / 2 + U(20), U(18), rgba(130, 136, 150, 255), 1);
        }
    }
}

/* ---------------------------------------------------------------- screens */

static void scr_main(void)
{
    begin_list("Main Menu", A.g->subtitle);
    int d;
    int has_auto = 0;
    if (A.slots_dirty) refresh_slots();
    has_auto = A.slots[FE_SLOT_AUTO].valid;
    if (A.session) {
        if (row_item("Resume", NULL, 1, NULL, "Return to the game in progress.")) close_menu();
    }
    if (row_item(A.session ? "Start New Game" : "Start Game", NULL, A.rom_ok, NULL,
                 A.rom_ok ? "Power on the board and start from the beginning." : "Select a ROM set first.")) {
        if (A.session) ask(CF_NEW_GAME, 0, "Start a new game? Unsaved progress in the current game will be lost.");
        else { start_new_game(); close_menu(); }
    }
    if (feat(FE_FEAT_SAVE_STATES) && row_item("Continue", has_auto ? "Autosave" : "none", A.rom_ok && has_auto, NULL,
                 "Resume from the automatic save made when you last left a game.")) {
        if (A.session) ask(CF_LOAD_SLOT, FE_SLOT_AUTO, "Continue from the autosave? Unsaved progress will be lost.");
        else if (load_slot(FE_SLOT_AUTO, 0)) close_menu();
    }
    if (feat(FE_FEAT_SAVE_STATES) && row_item("Load State", NULL, A.rom_ok, NULL, "Pick one of the save-state slots.")) push(SCR_LOAD);
    if (feat(FE_FEAT_SAVE_STATES) && A.session && row_item("Save State", NULL, 1, NULL, "Save the current game to a slot.")) push(SCR_SAVE);
    char rom_label[300];
    snprintf(rom_label, sizeof(rom_label), "%s", A.rom_ok ? fe_basename(A.rom_path) : "not set");
    if (row_item("ROM Selection", rom_label, 1, NULL, A.rom_ok ? A.rom_path : "Choose the ROM archive to play.")) push(SCR_ROM);
    if (row_item("Display Settings", NULL, 1, NULL, "Scaling, filtering, CRT effects, colour and borders.")) push(SCR_DISPLAY);
    if (row_item("Audio Settings", NULL, 1, NULL, "Volume, output device and latency.")) push(SCR_AUDIO);
    if (row_item("Controller Settings", NULL, 1, NULL, "Keyboard and controller mappings for both players.")) push(SCR_CONTROLS);
    if (row_item("Gameplay Settings", NULL, 1, NULL, "Cabinet DIP switches: difficulty, lives, coinage.")) push(SCR_GAMEPLAY);
    if (row_item("Gadgets", NULL, 1, NULL, "On-screen panels: score, FPS, frame time, input display.")) push(SCR_GADGETS);
    if (row_item("Extras", NULL, 1, NULL, "Rewind, input movies, screenshots and folders.")) push(SCR_EXTRAS);
    if (row_item("About", NULL, 1, NULL, "Version, credits and licences.")) push(SCR_ABOUT);
    if (row_item("Exit", NULL, 1, NULL, "Quit to the desktop.")) {
        if (A.session) ask(CF_EXIT, 0, A.s.autosave_exit ? "Exit the game? Progress will be autosaved." : "Exit the game? Unsaved progress will be lost.");
        else A.quit = 1;
    }
    (void)d;
    end_list();
    right_panel_game_preview();
    if (!A.rom_ok && A.rom_error[0]) {
        int x = A.win_w * 50 / 100;
        fe_text(A.renderer, "ROM not ready", x, U(150), U(26), rgba(255, 160, 120, 255), 0);
        fe_text(A.renderer, A.rom_error, x, U(190), U(18), rgba(200, 200, 210, 255), 0);
    }
    if (A.nv.back && A.session) close_menu();
}

static void scr_quick(void)
{
    begin_list("Paused", A.movie_status[0] ? A.movie_status : NULL);
    if (row_item("Resume", NULL, 1, NULL, "Return to the game.")) close_menu();
    if (row_item("Restart Game", NULL, 1, NULL, "Reset the board. Unsaved progress is lost."))
        ask(CF_RESTART, 0, "Restart the game? Unsaved progress will be lost.");
    char sv[32];
    snprintf(sv, sizeof(sv), "Slot %d", A.slot);
    if (feat(FE_FEAT_SAVE_STATES) && row_item("Save State", sv, 1, NULL, "Choose a slot to save to.")) push(SCR_SAVE);
    if (feat(FE_FEAT_SAVE_STATES) && row_item("Load State", sv, 1, NULL, "Choose a slot to load.")) push(SCR_LOAD);
    if (row_item("Display Settings", NULL, 1, NULL, NULL)) push(SCR_DISPLAY);
    if (row_item("Audio Settings", NULL, 1, NULL, NULL)) push(SCR_AUDIO);
    if (row_item("Controller Configuration", NULL, 1, NULL, NULL)) push(SCR_CONTROLS);
    if (row_item("Gadgets", NULL, 1, NULL, NULL)) push(SCR_GADGETS);
    if (row_item("Extras", NULL, 1, NULL, "Rewind, input movie recording, screenshots.")) push(SCR_EXTRAS);
    if (row_item("Return to Main Menu", NULL, 1, NULL, A.s.autosave_menu ? "Leaves the game; it is autosaved first." : "Leaves the game without saving."))
        ask(CF_MAIN_MENU, 0, A.s.autosave_menu ? "Return to the main menu? The game will be autosaved." : "Return to the main menu? Unsaved progress will be lost.");
    if (row_item("Exit", NULL, 1, NULL, NULL))
        ask(CF_EXIT, 0, A.s.autosave_exit ? "Exit to the desktop? The game will be autosaved." : "Exit to the desktop? Unsaved progress will be lost.");
    end_list();
    if (A.nv.back) close_menu();
}

static const char *onoff(int v) { return v ? "On" : "Off"; }

static void cycle(int *v, int d, int count) { *v = ((*v + d) % count + count) % count; }
static void step(int *v, int d, int st, int lo, int hi) { *v += d * st; if (*v < lo) *v = lo; if (*v > hi) *v = hi; }

static void scr_display(void)
{
    begin_list("Display", NULL);
    int d;
    char buf[64];
    static const char *scale_names[] = { "Fit to screen", "Integer" };
    static const char *aspect_names[] = { "Original 4:3", "Square pixels" };
    static const char *filter_names[] = { "Nearest (sharp)", "Bilinear (smooth)" };
    static const char *pacing_names[] = { "Exact game rate", "Display sync" };
    static const char *mask_names[] = { "Off", "Aperture grille", "Shadow mask", "Slot mask" };
    static const char *border_names[] = { "Black", "Gradient wallpaper", "Ambient glow" };
    static const char *rot_plain[] = { "0", "90", "180", "270" };
    static const char *rot_orig[] = { "0 (original)", "90 (original)", "180 (original)", "270 (original)" };
    const char *rot_names[4];
    for (int r = 0; r < 4; r++) rot_names[r] = r == (A.g->rotation & 3) ? rot_orig[r] : rot_plain[r];
    if (row_choice("Mode", A.s.fullscreen ? "Fullscreen" : "Windowed", 1, "Borderless fullscreen or a window.")) {
        A.s.fullscreen = !A.s.fullscreen; apply_fullscreen();
    }
    snprintf(buf, sizeof(buf), "%dx", A.s.window_scale);
    if ((d = row_choice("Window Size", buf, !A.s.fullscreen, "Initial window size as a multiple of the native height."))) {
        step(&A.s.window_scale, d, 1, 1, 8); apply_fullscreen();
    }
    if ((d = row_choice("Scaling", scale_names[A.s.scale_mode], 1, "Integer keeps every source line the same height (pixel-perfect)."))) cycle(&A.s.scale_mode, d, 2);
    if ((d = row_choice("Aspect Ratio", aspect_names[A.s.aspect_mode], 1, "4:3 matches the original arcade monitor."))) cycle(&A.s.aspect_mode, d, 2);
    if ((d = row_choice("Filtering", filter_names[A.s.filter], 1, NULL))) { cycle(&A.s.filter, d, 2); apply_filter(); }
    if ((d = row_choice("VSync", onoff(A.s.vsync), 1, "Synchronise presentation with the display refresh."))) { A.s.vsync = !A.s.vsync; apply_vsync(); }
    if ((d = row_choice("Frame Pacing", pacing_names[A.s.pacing], 1, "Exact keeps the game's own frame rate; Display sync locks one game frame per refresh when the display is 59-61 Hz."))) cycle(&A.s.pacing, d, 2);
    if ((d = row_choice("Rotation", rot_names[A.s.rotation], 1, "Rotate the picture for rotated or vertical monitors."))) cycle(&A.s.rotation, d, 4);
    snprintf(buf, sizeof(buf), "%d%%", A.s.scanlines);
    if ((d = row_choice("Scanlines", buf, 1, "Darkens every other line of the scaled picture."))) step(&A.s.scanlines, d, 10, 0, 100);
    if ((d = row_choice("Phosphor Mask", mask_names[A.s.mask], 1, "Simulated CRT phosphor pattern."))) cycle(&A.s.mask, d, 4);
    snprintf(buf, sizeof(buf), "%d%%", A.s.mask_strength);
    if ((d = row_choice("Mask Strength", buf, A.s.mask != 0, NULL))) step(&A.s.mask_strength, d, 10, 0, 100);
    snprintf(buf, sizeof(buf), "%d%%", A.s.brightness);
    if ((d = row_choice("Brightness", buf, 1, NULL))) { step(&A.s.brightness, d, 5, 50, 150); A.lut_dirty = 1; }
    snprintf(buf, sizeof(buf), "%.2f", A.s.gamma / 100.0);
    if ((d = row_choice("Gamma", buf, 1, NULL))) { step(&A.s.gamma, d, 5, 50, 200); A.lut_dirty = 1; }
    if ((d = row_choice("Background", border_names[A.s.border], 1, "What fills the space around the picture."))) cycle(&A.s.border, d, 3);
    if ((d = row_choice("Menu Accent", ACCENT_NAMES[A.s.accent], 1, NULL))) cycle(&A.s.accent, d, ACCENT_COUNT);
    if (row_item("Restore Defaults", NULL, 1, NULL, "Reset display settings only.")) {
        settings keep = A.s;
        default_settings();
        settings def = A.s;
        A.s = keep;
        A.s.fullscreen = def.fullscreen; A.s.window_scale = def.window_scale; A.s.scale_mode = def.scale_mode;
        A.s.aspect_mode = def.aspect_mode; A.s.filter = def.filter; A.s.vsync = def.vsync; A.s.pacing = def.pacing;
        A.s.scanlines = def.scanlines; A.s.mask = def.mask; A.s.mask_strength = def.mask_strength;
        A.s.brightness = def.brightness; A.s.gamma = def.gamma; A.s.border = def.border; A.s.rotation = def.rotation;
        A.lut_dirty = 1; apply_filter(); apply_vsync(); apply_fullscreen();
        toast("Display settings restored");
    }
    end_list();
    /* live preview of the picture with current settings */
    if (A.session) {
        int x = A.win_w * 52 / 100, y = U(150), w = A.win_w - x - U(48), h = A.win_h - y - U(120);
        if (w > U(200)) {
            SDL_Rect clip = { x, y, w, h };
            SDL_RenderSetViewport(A.renderer, &clip);
            draw_game(w, h, 0);
            SDL_RenderSetViewport(A.renderer, NULL);
        }
    }
    if (A.nv.back) { save_settings(); pop(); }
}

static void scr_audio(void)
{
    begin_list("Audio", NULL);
    int d;
    char buf[300];
    snprintf(buf, sizeof(buf), "%d%%", A.s.volume);
    if ((d = row_choice("Master Volume", buf, 1, NULL))) step(&A.s.volume, d, 5, 0, 100);
    if (row_choice("Mute", onoff(A.s.mute), 1, NULL)) A.s.mute = !A.s.mute;
    int count = SDL_GetNumAudioDevices(0);
    int current = -1;
    for (int i = 0; i < count; i++) { const char *n = SDL_GetAudioDeviceName(i, 0); if (n && !strcmp(n, A.s.audio_device)) current = i; }
    snprintf(buf, sizeof(buf), "%s", current < 0 ? "System default" : SDL_GetAudioDeviceName(current, 0));
    if ((d = row_choice("Output Device", buf, count > 0, "Changes take effect immediately."))) {
        current += d;
        if (current < -1) current = count - 1;
        if (current >= count) current = -1;
        snprintf(A.s.audio_device, sizeof(A.s.audio_device), "%s", current < 0 ? "" : SDL_GetAudioDeviceName(current, 0));
        audio_open();
        if (A.in_menu && A.audio_dev) SDL_PauseAudioDevice(A.audio_dev, 1);
    }
    snprintf(buf, sizeof(buf), "%d ms", A.s.latency_ms);
    if ((d = row_choice("Latency Target", buf, 1, "Lower is more responsive; raise it only if you hear crackling."))) {
        step(&A.s.latency_ms, d, 8, 16, 250);
        audio_open();
        if (A.in_menu && A.audio_dev) SDL_PauseAudioDevice(A.audio_dev, 1);
    }
    snprintf(buf, sizeof(buf), "%d Hz -> %d Hz", (int)(A.g->audio_rate + 0.5), A.audio_have.freq);
    row_item("Output Format", buf, 0, NULL, "The game's sound chip runs at its native rate and is resampled to the device.");
    snprintf(buf, sizeof(buf), "%d underruns, %d drops", A.audio_underruns, A.audio_drops);
    row_item("Diagnostics", buf, 0, NULL, "Counts since start. Non-zero values point to a timing problem; see the log.");
    row_item("Music / Effects Volume", "Not exposed", 0, NULL, "The board mixes music and effects on one chip, so separate levels are not available.");
    end_list();
    if (A.nv.back) { save_settings(); pop(); }
}

static void begin_capture(int index, int is_hotkey, int kind)
{
    A.capture_index = index;
    A.capture_is_hotkey = is_hotkey;
    A.capture_kind = kind;   /* 0 keyboard primary, 1 keyboard secondary, 2 controller */
    A.capture_t0 = now_ticks();
    push(SCR_CAPTURE);
}

static void scr_controls(void)
{
    begin_list("Controls", "Enter: keyboard   Right: alternate key   Y/Tab: controller   Del: clear");
    char buf[200], k0[48], k1[48], pd[48];
    int d;
    static const char *combo_names[] = { "Back + Start", "Guide", "Right stick click", "L3 + R3" };
    if ((d = row_choice("Quick Menu (controller)", combo_names[A.s.menu_combo], 1, "Controller shortcut that opens the in-game menu. Esc always works."))) cycle(&A.s.menu_combo, d, 4);
    if (row_item("Hotkeys", NULL, 1, NULL, "Quick save/load, slots, screenshot, rewind, pause.")) push(SCR_HOTKEYS);
    if (row_item("Player Controllers", NULL, 1, NULL, "Which connected controller drives Player 1 and Player 2.")) push(SCR_CONTROLS_P);
    snprintf(buf, sizeof(buf), "%d%%", A.s.deadzone);
    if ((d = row_choice("Stick Dead Zone", buf, 1, "Analog sticks act as the D-pad beyond this deflection."))) step(&A.s.deadzone, d, 5, 5, 90);
    const SDL_Keymod mod = SDL_GetModState();
    (void)mod;
    for (int i = 0; i < A.g->input_count; i++) {
        key_name(A.bind[i].key[0], k0, sizeof(k0));
        key_name(A.bind[i].key[1], k1, sizeof(k1));
        pad_name(A.bind[i].pad, pd, sizeof(pd));
        snprintf(buf, sizeof(buf), "%s / %s   |   %s", k0, k1, pd);
        int focused_before = A.focus[top()] == A.row;
        if (row_item(A.g->inputs[i].label, buf, 1, &d, "Enter: set key.  Right: set alternate key.  Y or Tab: set controller button.  Delete/X: clear.")) begin_capture(i, 0, 0);
        else if (focused_before && d > 0) begin_capture(i, 0, 1);
        else if (focused_before && A.nv.alt == 1) begin_capture(i, 0, 2);
        else if (focused_before && A.nv.alt == 2) { A.bind[i].key[0] = A.bind[i].key[1] = -1; A.bind[i].pad = -1; }
    }
    if (row_item("Restore Default Controls", NULL, 1, NULL, NULL)) ask(CF_RESET_CONTROLS, 0, "Restore all default keyboard and controller mappings?");
    end_list();
    if (A.nv.back) { save_settings(); pop(); }
}

static void scr_hotkeys(void)
{
    begin_list("Hotkeys", "Enter: key   Right: alternate key   Y/Tab: controller   Del: clear");
    char buf[200], k0[48], k1[48], pd[48];
    int d;
    for (int i = 0; i < FE_HOTKEY_COUNT; i++) {
        key_name(A.hk[i].key[0], k0, sizeof(k0));
        key_name(A.hk[i].key[1], k1, sizeof(k1));
        pad_name(A.hk[i].pad, pd, sizeof(pd));
        snprintf(buf, sizeof(buf), "%s / %s   |   %s", k0, k1, pd);
        int focused_before = A.focus[top()] == A.row;
        if (row_item(HOTKEY_LABEL[i], buf, 1, &d, i == HK_MENU ? "Esc is always available as a fallback for the menu." : NULL)) begin_capture(i, 1, 0);
        else if (focused_before && d > 0) begin_capture(i, 1, 1);
        else if (focused_before && A.nv.alt == 1) begin_capture(i, 1, 2);
        else if (focused_before && A.nv.alt == 2) { A.hk[i].key[0] = A.hk[i].key[1] = -1; A.hk[i].pad = -1; }
    }
    end_list();
    if (A.nv.back) { save_settings(); pop(); }
}

static void scr_controls_players(void)
{
    begin_list("Player Controllers", NULL);
    int d;
    char buf[200];
    for (int p = 0; p < game_players(); p++) {
        int idx = A.s.pad_player[p];
        if (idx < 0 || idx >= A.pad_count) snprintf(buf, sizeof(buf), idx < 0 ? "Automatic" : "Controller %d (not connected)", idx + 1);
        else snprintf(buf, sizeof(buf), "%d: %s", idx + 1, SDL_GameControllerName(A.pads[idx]));
        char label[32];
        snprintf(label, sizeof(label), "Player %d", p + 1);
        if ((d = row_choice(label, buf, 1, "Coin, service and test accept every controller."))) step(&A.s.pad_player[p], d, 1, -1, 7);
    }
    for (int i = 0; i < A.pad_count; i++) {
        snprintf(buf, sizeof(buf), "%s", SDL_GameControllerName(A.pads[i]));
        char label[32];
        snprintf(label, sizeof(label), "Connected %d", i + 1);
        row_item(label, buf, 0, NULL, NULL);
    }
    if (A.pad_count == 0) row_item("No controllers connected", NULL, 0, NULL, "Plug one in at any time; it is detected automatically.");
    end_list();
    if (A.nv.back) { save_settings(); pop(); }
}

static void scr_capture(void)
{
    int ww = A.win_w, wh = A.win_h;
    fe_fill(A.renderer, 0, 0, ww, wh, rgba(0, 0, 0, 170));
    const char *what = A.capture_is_hotkey ? HOTKEY_LABEL[A.capture_index] : A.g->inputs[A.capture_index].label;
    char line[200];
    snprintf(line, sizeof(line), A.capture_kind == 2 ? "Press a controller button or move a stick for" : "Press a key for");
    fe_round_rect(A.renderer, ww / 2 - U(360), wh / 2 - U(110), U(720), U(220), U(14), rgba(24, 27, 38, 250));
    fe_text(A.renderer, line, ww / 2, wh / 2 - U(70), U(24), rgba(200, 204, 214, 255), 1);
    fe_text(A.renderer, what, ww / 2, wh / 2 - U(30), U(36), accent(), 1);
    double left = 5.0 - ms_since(A.capture_t0) / 1000.0;
    snprintf(line, sizeof(line), "Waiting %.0f s  -  Esc cancels", left < 0 ? 0 : left);
    fe_text(A.renderer, line, ww / 2, wh / 2 + U(40), U(20), rgba(140, 146, 160, 255), 1);
    if (left <= 0) pop();
}

static void capture_event(const SDL_Event *e)
{
    binding *b = A.capture_is_hotkey ? &A.hk[A.capture_index] : &A.bind[A.capture_index];
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        if (e->key.keysym.scancode == SDL_SCANCODE_ESCAPE && !(A.capture_is_hotkey && A.capture_index == HK_MENU)) { pop(); return; }
        if (A.capture_kind == 2) return;
        b->key[A.capture_kind] = e->key.keysym.scancode;
        pop();
    } else if (A.capture_kind == 2 && e->type == SDL_CONTROLLERBUTTONDOWN) {
        b->pad = e->cbutton.button;
        pop();
    } else if (A.capture_kind == 2 && e->type == SDL_CONTROLLERAXISMOTION && abs(e->caxis.value) > 24000) {
        b->pad = FE_PAD_AXIS_BASE + e->caxis.axis * 2 + (e->caxis.value > 0 ? 1 : 0);
        pop();
    }
}

static void scr_gameplay(void)
{
    begin_list("Gameplay", "Cabinet DIP switches - applied when the game is next started or restarted");
    int d;
    for (int i = 0; i < A.g->dip_count; i++) {
        const fe_dip_def *dp = &A.g->dips[i];
        uint16_t cur = A.s.dips[dp->word] & dp->mask;
        int idx = 0;
        for (int k = 0; k < dp->option_count; k++) if (dp->options[k].value == cur) idx = k;
        if ((d = row_choice(dp->label, dp->options[idx].label, 1, NULL))) {
            cycle(&idx, d, dp->option_count);
            A.s.dips[dp->word] = (uint16_t)((A.s.dips[dp->word] & ~dp->mask) | dp->options[idx].value);
        }
    }
    if (row_item("Restore Defaults", NULL, 1, NULL, NULL)) {
        for (int i = 0; i < A.g->dip_count; i++) {
            const fe_dip_def *dp = &A.g->dips[i];
            A.s.dips[dp->word] = (uint16_t)((A.s.dips[dp->word] & ~dp->mask) | (dp->default_value & dp->mask));
        }
    }
    if (A.session && row_item("Restart Now to Apply", NULL, 1, NULL, NULL)) ask(CF_RESTART, 0, "Restart the game with the new switch settings? Unsaved progress will be lost.");
    end_list();
    if (A.nv.back) { save_settings(); pop(); }
}

static const char *const ANCHOR_NAMES[] = { "Top left", "Top right", "Bottom left", "Bottom right", "Top centre", "Bottom centre" };

static void scr_gadgets(void)
{
    begin_list("Gadgets", "Enter: position and size");
    int d;
    for (int i = 0; i < gadget_total(); i++) {
        gadget_cfg *g = &A.s.gadget[i];
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s%s", g->enabled ? "On" : "Off", g->enabled ? "  -  " : "", g->enabled ? ANCHOR_NAMES[g->anchor % 6] : "");
        const char *help = i >= 3 ? "Read from the game's verified RAM layout." : (i == 2 ? "Shows the inputs sent to the board each frame." : NULL);
        if (row_item(gadget_label(i), buf, 1, &d, help)) { A.gadget_edit = i; push(SCR_GADGET_EDIT); }
        else if (d) g->enabled = !g->enabled;
    }
    end_list();
    if (A.nv.back) { save_settings(); pop(); }
}

static void scr_gadget_edit(void)
{
    gadget_cfg *g = &A.s.gadget[A.gadget_edit];
    begin_list(gadget_label(A.gadget_edit), NULL);
    int d;
    char buf[32];
    static const char *size_names[] = { "Small", "Medium", "Large" };
    if (row_choice("Enabled", onoff(g->enabled), 1, NULL)) g->enabled = !g->enabled;
    if ((d = row_choice("Position", ANCHOR_NAMES[g->anchor % 6], 1, NULL))) cycle(&g->anchor, d, 6);
    if ((d = row_choice("Size", size_names[g->size % 3], 1, NULL))) cycle(&g->size, d, 3);
    snprintf(buf, sizeof(buf), "%+d", g->dx);
    if ((d = row_choice("Offset X", buf, 1, "Fine horizontal adjustment in UI pixels."))) step(&g->dx, d, 8, -400, 400);
    snprintf(buf, sizeof(buf), "%+d", g->dy);
    if ((d = row_choice("Offset Y", buf, 1, NULL))) step(&g->dy, d, 8, -300, 300);
    end_list();
    if (A.nv.back) { save_settings(); pop(); }
}

static void scr_extras(void)
{
    begin_list("Extras", NULL);
    int d;
    char buf[160];
    if (row_choice("Rewind", feat(FE_FEAT_REWIND) ? onoff(A.s.rewind_enabled) : "Unsupported", feat(FE_FEAT_REWIND), "Hold the Rewind hotkey (Backspace) to step back in time.")) {
        A.s.rewind_enabled = !A.s.rewind_enabled;
        if (A.session) rewind_reset();
    }
    snprintf(buf, sizeof(buf), "%d MB", A.s.rewind_mb);
    if ((d = row_choice("Rewind Memory Limit", buf, A.s.rewind_enabled, "Older history is discarded beyond this budget."))) {
        step(&A.s.rewind_mb, d, 16, 16, 512);
        A.rw.budget = (size_t)A.s.rewind_mb * 1024u * 1024u;
    }
    if (A.rw.snap) {
        snprintf(buf, sizeof(buf), "%.1f s, %.1f MB", A.rw.count * 4 / A.g->refresh_hz, A.rw.bytes / 1048576.0);
        row_item("Rewind History", buf, 0, NULL, NULL);
    }
    if (row_choice("Autosave on Exit", onoff(A.s.autosave_exit), 1, NULL)) A.s.autosave_exit = !A.s.autosave_exit;
    if (row_choice("Autosave on Main Menu", onoff(A.s.autosave_menu), 1, NULL)) A.s.autosave_menu = !A.s.autosave_menu;
    if (row_choice("Pause When Unfocused", onoff(A.s.pause_on_focus), 1, NULL)) A.s.pause_on_focus = !A.s.pause_on_focus;
    if (A.movie_rec) { if (row_item("Stop Recording", NULL, 1, NULL, A.movie_status)) movie_stop(); }
    else if (row_item("Record Input Movie", NULL, A.session && feat(FE_FEAT_MOVIES), NULL, "Records your inputs from the current moment; replays exactly.")) { movie_record_start(); close_menu(); }
    if (row_item("Play Latest Movie", NULL, A.rom_ok && !A.movie_rec, NULL, "Loads the movie's starting point and replays its inputs.")) {
        if (A.session) ask(CF_NONE, 99, "Play the latest movie? Unsaved progress will be lost.");
        else { movie_play_latest(); if (A.movie_play) close_menu(); }
    }
    if (row_item("Take Screenshot", NULL, A.session, NULL, A.shots_dir)) screenshot();
    row_item("Data Folder", A.base_dir, 0, NULL, "Settings, save states, screenshots, movies and the log live here.");
    if (row_item("Reset All Settings", NULL, 1, NULL, "Restores every setting and mapping. Save states are kept."))
        ask(CF_RESET_SETTINGS, 0, "Reset all settings and controls to defaults? Save states are kept.");
    end_list();
    if (A.nv.back) { save_settings(); pop(); }
}

static void scr_about(void)
{
    begin_list("About", NULL);
    end_list();
    int x = U(40), y = U(160);
    const char *p = A.g->about;
    char line[256];
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len); line[len] = 0;
        fe_text(A.renderer, line, x, y, U(21), rgba(206, 210, 220, 255), 0);
        y += U(30);
        p = nl ? nl + 1 : NULL;
    }
    char extra[FE_PATH_MAX + 64];
    snprintf(extra, sizeof(extra), "Data folder: %s", A.base_dir);
    fe_text(A.renderer, extra, x, y + U(16), U(18), rgba(140, 146, 160, 255), 0);
    snprintf(extra, sizeof(extra), "Profile: %s", A.profile.loaded ? A.profile.path : "none (adapter defaults)");
    fe_text(A.renderer, extra, x, y + U(72), U(18), rgba(140, 146, 160, 255), 0);
    fe_text(A.renderer, A.feat_report, x, y + U(100), U(16), rgba(140, 146, 160, 255), 0);
    SDL_version v; SDL_GetVersion(&v);
    snprintf(extra, sizeof(extra), "SDL %d.%d.%d   Renderer: %s", v.major, v.minor, v.patch, "SDL_Renderer");
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(A.renderer, &info) == 0) snprintf(extra, sizeof(extra), "SDL %d.%d.%d   Renderer: %s", v.major, v.minor, v.patch, info.name);
    fe_text(A.renderer, extra, x, y + U(44), U(18), rgba(140, 146, 160, 255), 0);
    if (A.nv.back || A.nv.accept) pop();
}

static void format_time(int64_t t, char *out, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    strftime(out, n, "%Y-%m-%d %H:%M", &tmv);
}

static void scr_slots(int saving)
{
    if (A.slots_dirty) refresh_slots();
    begin_list(saving ? "Save State" : "Load State", "Thumbnails show the moment each slot was saved");
    int first = saving ? 1 : 0;
    for (int i = first; i <= FE_SLOTS; i++) {
        slot_info *si = &A.slots[i];
        char label[32], value[96];
        if (i == 0) snprintf(label, sizeof(label), "Autosave");
        else snprintf(label, sizeof(label), "Slot %d%s", i, i == A.slot ? "  *" : "");
        if (!si->exists) snprintf(value, sizeof(value), "Empty");
        else if (!si->valid) snprintf(value, sizeof(value), "Unusable");
        else format_time(si->timestamp, value, sizeof(value));
        int focused_before = A.focus[top()] == A.row;
        int enabled = saving || si->valid;
        if (row_item(label, value, enabled, NULL, si->exists && !si->valid ? si->error : NULL)) {
            if (saving) {
                if (si->exists) ask(CF_SAVE_SLOT, i, "Overwrite slot %d (saved %s)?", i, value);
                else { save_slot(i); A.slot = i; refresh_slots(); pop(); if (A.in_menu && top() == SCR_QUICK) close_menu(); }
            } else {
                if (A.session) ask(CF_LOAD_SLOT, i, "Load %s? Unsaved progress will be lost.", label);
                else if (load_slot(i, 0)) { if (i) A.slot = i; close_menu(); }
            }
        }
        if (focused_before && A.nv.alt == 2 && si->exists && i > 0) ask(CF_DELETE_SLOT, i, "Delete slot %d?", i);
        if (focused_before) {
            /* preview panel */
            int x = A.win_w * 52 / 100, y = U(150), w = A.win_w - x - U(60);
            double shown = (A.s.rotation & 1) ? 1.0 / A.g->display_aspect : A.g->display_aspect;
            int h = (int)(w / shown);
            if (h > A.win_h - y - U(160)) { h = A.win_h - y - U(160); w = (int)(h * shown); }
            fe_round_rect(A.renderer, x - U(8), y - U(8), w + U(16), h + U(16), U(10), rgba(30, 34, 46, 255));
            if (si->valid && si->thumb) draw_fitted(si->thumb, x, y, w, h);
            else {
                fe_fill(A.renderer, x, y, w, h, rgba(10, 12, 18, 255));
                fe_text(A.renderer, si->exists ? "Unusable" : "Empty", x + w / 2, y + h / 2 - U(16), U(28), rgba(120, 126, 140, 255), 1);
            }
            if (si->valid) {
                char info[96];
                snprintf(info, sizeof(info), "%s   -   %u frames played", value, si->frames);
                fe_text(A.renderer, info, x, y + h + U(20), U(20), rgba(190, 194, 206, 255), 0);
            } else if (si->exists) {
                fe_text(A.renderer, si->error, x, y + h + U(20), U(18), rgba(255, 160, 120, 255), 0);
            }
        }
    }
    end_list();
    if (A.nv.back) pop();
}

static void browse_load(const char *dir)
{
    free(A.entries);
    A.entries = NULL;
    A.entry_count = 0;
    A.browse_roots = dir == NULL;
    if (dir) {
        snprintf(A.browse_dir, sizeof(A.browse_dir), "%s", dir);
        A.entry_count = fe_list_dir(dir, &A.entries);
        if (A.entry_count < 0) { A.entry_count = 0; toast("Cannot open %s", dir); }
    } else {
        A.entry_count = fe_list_roots(&A.entries);
        if (A.entry_count < 0) A.entry_count = 0;
    }
    A.focus[SCR_ROM] = 0;
    A.scroll[SCR_ROM] = 0;
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) {
        char x = s[a - b + i], y = suffix[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

static void try_rom(const char *path)
{
    char err[512] = "";
    if (!A.g->validate_rom(path, err, sizeof(err))) {
        show_message("ROM not usable", "%s\n\n%s\n\nExpected: %s (MAME set, unmodified).", fe_basename(path), err, A.g->rom_hint);
        return;
    }
    if (A.session) {
        snprintf(A.message_text, sizeof(A.message_text), "%s", path); /* stash */
        ask(CF_ROM_CHANGE, 0, "Switch to %s? The current game will end.", fe_basename(path));
        return;
    }
    if (load_rom(path)) {
        save_settings();
        toast("ROM ready: %s", fe_basename(path));
        pop();
    } else {
        show_message("ROM not usable", "%s", A.rom_error);
    }
}

static void scr_rom(void)
{
    if (!A.entries && !A.browse_roots) {
        char start[FE_PATH_MAX] = "";
        if (A.browse_dir[0] && fe_dir_exists(A.browse_dir)) snprintf(start, sizeof(start), "%s", A.browse_dir);
        else if (A.rom_path[0]) fe_dirname(A.rom_path, start, sizeof(start));
        if (!start[0] || !fe_dir_exists(start)) fe_home_dir(start, sizeof(start));
        browse_load(start);
    }
    begin_list("ROM Selection", A.browse_roots ? "Drives and locations" : A.browse_dir);
    if (row_item(A.browse_roots ? "(locations)" : ".. (parent folder)", NULL, 1, NULL, "Go up one level.")) {
        if (A.browse_roots) { /* stay */ }
        else {
            char parent[FE_PATH_MAX];
            fe_dirname(A.browse_dir, parent, sizeof(parent));
            if (!strcmp(parent, A.browse_dir) || !parent[0]) browse_load(NULL);
            else browse_load(parent);
        }
        end_list();
        return;
    }
    if (!A.browse_roots) {
        char hint[FE_PATH_MAX];
        fe_path_join(hint, sizeof(hint), A.browse_dir, A.g->rom_hint);
        if (fe_file_exists(hint) && row_item("Use this folder's ROM", A.g->rom_hint, 1, NULL, hint)) { try_rom(hint); end_list(); return; }
    }
    if (row_item("Show", A.browse_all ? "All files" : "ZIP archives", 1, NULL, "Toggle between ZIP archives only and every file.")) A.browse_all = !A.browse_all;
    for (int i = 0; i < A.entry_count; i++) {
        fe_dirent *e = &A.entries[i];
        int is_zip = !e->is_dir && ends_with_ci(e->name, ".zip");
        if (!e->is_dir && !is_zip && !A.browse_all) continue;
        char label[560];
        snprintf(label, sizeof(label), "%s%s", e->name, e->is_dir ? "/" : "");
        const char *tag = e->is_dir ? "folder" : (!strcmp(e->name, A.g->rom_hint) ? "expected name" : (is_zip ? "zip" : ""));
        if (row_item(label, tag, 1, NULL, NULL)) {
            char path[FE_PATH_MAX];
            if (A.browse_roots) snprintf(path, sizeof(path), "%s", e->name);
            else fe_path_join(path, sizeof(path), A.browse_dir, e->name);
            if (e->is_dir) { browse_load(path); break; }
            try_rom(path);
            break;
        }
    }
    end_list();
    if (A.nv.back) { free(A.entries); A.entries = NULL; A.browse_roots = 0; save_settings(); pop(); }
}

static void scr_confirm(void)
{
    int ww = A.win_w, wh = A.win_h;
    fe_fill(A.renderer, 0, 0, ww, wh, rgba(0, 0, 0, 160));
    int bw = U(760), bh = U(250);
    int bx = (ww - bw) / 2, by = (wh - bh) / 2;
    fe_round_rect(A.renderer, bx, by, bw, bh, U(16), rgba(24, 27, 38, 252));
    fe_fill(A.renderer, bx, by, bw, U(4), accent());
    fe_text(A.renderer, A.confirm_text, ww / 2, by + U(56), U(22), rgba(232, 234, 240, 255), 1);
    int *f = &A.focus[SCR_CONFIRM];
    if (A.nv.left || A.nv.up) *f = 0;
    if (A.nv.right || A.nv.down) *f = 1;
    const char *labels[2] = { "Yes", "No" };
    int chosen = -1;
    for (int i = 0; i < 2; i++) {
        int w = U(200), h = U(56);
        int x = ww / 2 + (i == 0 ? -w - U(20) : U(20)), y = by + bh - h - U(36);
        int hover = A.nv.mouse_x >= x && A.nv.mouse_x < x + w && A.nv.mouse_y >= y && A.nv.mouse_y < y + h;
        if (hover && A.nv.mouse_moved) *f = i;
        fe_round_rect(A.renderer, x, y, w, h, U(10), *f == i ? accent() : rgba(44, 48, 62, 255));
        fe_text(A.renderer, labels[i], x + w / 2, y + (h - fe_text_line_height(U(24))) / 2, U(24), *f == i ? rgba(10, 12, 16, 255) : rgba(220, 224, 232, 255), 1);
        if (hover && A.nv.click) chosen = i;
    }
    if (A.nv.accept) chosen = *f;
    if (A.nv.back) chosen = 1;
    if (chosen < 0) return;
    pop();
    if (chosen == 1) return;
    switch (A.pending) {
    case CF_EXIT: end_session(A.s.autosave_exit); A.quit = 1; break;
    case CF_MAIN_MENU: end_session(A.s.autosave_menu); A.depth = 0; push(SCR_MAIN); break;
    case CF_RESTART: start_new_game(); close_menu(); break;
    case CF_NEW_GAME: start_new_game(); close_menu(); break;
    case CF_LOAD_SLOT: if (load_slot(A.pending_arg, 0)) { if (A.pending_arg) A.slot = A.pending_arg; close_menu(); } break;
    case CF_SAVE_SLOT: save_slot(A.pending_arg); A.slot = A.pending_arg; refresh_slots(); pop(); if (A.depth && top() == SCR_QUICK) close_menu(); break;
    case CF_DELETE_SLOT: {
        char path[FE_PATH_MAX];
        slot_path(A.pending_arg, path, sizeof(path));
        remove(path);
        A.slots_dirty = 1;
        break;
    }
    case CF_RESET_SETTINGS: {
        char rom[FE_PATH_MAX];
        snprintf(rom, sizeof(rom), "%s", A.rom_path);
        default_settings(); default_bindings();
        snprintf(A.rom_path, sizeof(A.rom_path), "%s", rom);
        A.lut_dirty = 1; apply_filter(); apply_vsync(); apply_fullscreen(); audio_open();
        if (A.in_menu && A.audio_dev) SDL_PauseAudioDevice(A.audio_dev, 1);
        save_settings();
        toast("All settings restored");
        break;
    }
    case CF_RESET_CONTROLS: default_bindings(); A.s.menu_combo = 0; A.s.deadzone = 30; save_settings(); toast("Default controls restored"); break;
    case CF_ROM_CHANGE: {
        char path[FE_PATH_MAX];
        snprintf(path, sizeof(path), "%s", A.message_text);
        end_session(A.s.autosave_menu);
        if (load_rom(path)) { save_settings(); toast("ROM ready: %s", fe_basename(path)); A.depth = 0; push(SCR_MAIN); }
        else show_message("ROM not usable", "%s", A.rom_error);
        break;
    }
    case CF_NONE:
        if (A.pending_arg == 99) { movie_play_latest(); if (A.movie_play) close_menu(); }
        break;
    }
}

static void scr_message(void)
{
    int ww = A.win_w, wh = A.win_h;
    fe_fill(A.renderer, 0, 0, ww, wh, rgba(0, 0, 0, 170));
    int bw = U(900), bh = U(380);
    int bx = (ww - bw) / 2, by = (wh - bh) / 2;
    fe_round_rect(A.renderer, bx, by, bw, bh, U(16), rgba(24, 27, 38, 252));
    fe_fill(A.renderer, bx, by, bw, U(4), rgba(236, 120, 88, 255));
    fe_text(A.renderer, A.message_title, bx + U(36), by + U(28), U(30), rgba(244, 246, 250, 255), 0);
    int y = by + U(84);
    const char *p = A.message_text;
    char line[200];
    while (p && *p && y < by + bh - U(80)) {
        /* wrap at ~70 columns or newline */
        size_t len = strcspn(p, "\n");
        if (len == 0) { y += U(14); p++; continue; }   /* blank line: paragraph gap */
        size_t take = len;
        int maxw = bw - U(72);
        if (take >= sizeof(line)) take = sizeof(line) - 1;
        while (take > 1) {
            snprintf(line, sizeof(line), "%.*s", (int)take, p);
            if (fe_text_width(line, U(20)) <= maxw) break;
            /* break before the last space or path separator inside the span */
            size_t sp = take - 1;
            while (sp > 0 && p[sp] != ' ' && p[sp] != '\\' && p[sp] != '/') sp--;
            take = sp > 0 ? sp : take - 1;
        }
        if (take == 0) take = 1;
        snprintf(line, sizeof(line), "%.*s", (int)take, p);
        fe_text(A.renderer, line, bx + U(36), y, U(20), rgba(210, 214, 222, 255), 0);
        y += U(28);
        p += take;
        if (*p == '\n' || *p == ' ') p++;
    }
    fe_text(A.renderer, "Press Enter / A to continue", bx + bw - U(36), by + bh - U(52), U(18), rgba(140, 146, 160, 255), 2);
    if (A.nv.accept || A.nv.back || A.nv.click) {
        pop();
        if (A.depth == 0) { if (A.session) close_menu(); else push(SCR_MAIN); }
    }
}

/* --------------------------------------------------------------- gadgets */

static void draw_gadgets(void)
{
    int ww = A.win_w, wh = A.win_h;
    for (int i = 0; i < gadget_total(); i++) {
        gadget_cfg *g = &A.s.gadget[i];
        if (!g->enabled) continue;
        char text[96] = "";
        if (i == 0) snprintf(text, sizeof(text), "%.1f fps", A.fps);
        else if (i == 1) snprintf(text, sizeof(text), "%.2f ms", A.frame_ms_avg);
        else if (i == 2) {
            /* rendered below as button lamps */
        } else if (!A.g->gadgets[i - 3].read(text, sizeof(text))) continue;
        int px = (g->size == 0 ? U(18) : g->size == 1 ? U(24) : U(34));
        int w = i == 2 ? px * 14 : fe_text_width(text, px) + px;
        int h = i == 2 ? px / 3 + game_players() * (px + px / 3) : fe_text_line_height(px) + px / 2;
        int m = U(16);
        int x = 0, y = 0;
        switch (g->anchor % 6) {
        case 0: x = m; y = m; break;
        case 1: x = ww - w - m; y = m; break;
        case 2: x = m; y = wh - h - m; break;
        case 3: x = ww - w - m; y = wh - h - m; break;
        case 4: x = (ww - w) / 2; y = m; break;
        case 5: x = (ww - w) / 2; y = wh - h - m; break;
        }
        x += U(g->dx); y += U(g->dy);
        fe_round_rect(A.renderer, x, y, w, h, px / 3, rgba(8, 10, 14, 170));
        if (i != 2) {
            fe_text(A.renderer, text, x + px / 2, y + px / 4, px, rgba(240, 242, 246, 255), 0);
        } else {
            for (int p = 0; p < game_players(); p++) {
                int base_y = y + px / 3 + p * (px + px / 3);
                static const char *const tags[FE_MAX_PLAYERS] = { "1P", "2P", "3P", "4P" };
                fe_text(A.renderer, tags[p], x + px / 3, base_y - px / 8, px * 3 / 4, rgba(170, 176, 190, 255), 0);
                int bx = x + px * 2;
                int k = 0;
                for (int j = 0; j < A.g->input_count && k < 8; j++) {
                    const fe_input_def *d = &A.g->inputs[j];
                    if (d->player != p + 1) continue;
                    int on = (A.game_mask & d->mask) != 0;
                    fe_round_rect(A.renderer, bx + k * (px + px / 3), base_y, px, px * 3 / 4, px / 4, on ? accent() : rgba(50, 54, 66, 255));
                    k++;
                }
            }
        }
    }
}

static void draw_toast(void)
{
    if (!A.toast[0] || now_ticks() > A.toast_until) return;
    int px = U(22);
    int w = fe_text_width(A.toast, px) + U(40), h = fe_text_line_height(px) + U(18);
    int x = (A.win_w - w) / 2, y = A.win_h - h - U(40);
    fe_round_rect(A.renderer, x, y, w, h, U(10), rgba(16, 18, 26, 230));
    fe_fill(A.renderer, x, y + h - U(3), w, U(3), accent());
    fe_text(A.renderer, A.toast, A.win_w / 2, y + U(8), px, rgba(240, 242, 246, 255), 1);
}

/* ------------------------------------------------------------- main loop */

static void handle_menu_events(const SDL_Event *e)
{
    nav *n = &A.nv;
    switch (e->type) {
    case SDL_KEYDOWN:
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: n->up = 1; break;
        case SDL_SCANCODE_DOWN: n->down = 1; break;
        case SDL_SCANCODE_LEFT: n->left = 1; break;
        case SDL_SCANCODE_RIGHT: n->right = 1; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: if (!e->key.repeat) n->accept = 1; break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: if (!e->key.repeat) n->back = 1; break;
        case SDL_SCANCODE_PAGEUP: n->page_up = 1; break;
        case SDL_SCANCODE_PAGEDOWN: n->page_down = 1; break;
        case SDL_SCANCODE_TAB: if (!e->key.repeat) n->alt = 1; break;
        case SDL_SCANCODE_DELETE: if (!e->key.repeat) n->alt = 2; break;
        default: break;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: n->up = 1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: n->down = 1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: n->left = 1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: n->right = 1; break;
        case SDL_CONTROLLER_BUTTON_A: n->accept = 1; break;
        case SDL_CONTROLLER_BUTTON_B: n->back = 1; break;
        case SDL_CONTROLLER_BUTTON_Y: n->alt = 1; break;
        case SDL_CONTROLLER_BUTTON_X: n->alt = 2; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: n->page_up = 1; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: n->page_down = 1; break;
        default: break;
        }
        break;
    case SDL_MOUSEMOTION: {
        /* The first motion after the window appears only establishes the
         * pointer position; it must not steal keyboard/controller focus. */
        static int known, lx, ly;
        n->mouse_x = e->motion.x; n->mouse_y = e->motion.y;
        if (known && (e->motion.x != lx || e->motion.y != ly)) n->mouse_moved = 1;
        known = 1; lx = e->motion.x; ly = e->motion.y;
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
        n->mouse_x = e->button.x; n->mouse_y = e->button.y;
        if (e->button.button == SDL_BUTTON_LEFT) n->click = 1;
        else if (e->button.button == SDL_BUTTON_RIGHT) n->back = 1;
        break;
    case SDL_MOUSEWHEEL: n->wheel += e->wheel.y; break;
    default: break;
    }
}

/* Analog stick repeat for menus. */
static void stick_nav(void)
{
    static uint64_t next_repeat;
    static int last_dir;
    int dir = 0;
    for (int i = 0; i < A.pad_count; i++) {
        int y = SDL_GameControllerGetAxis(A.pads[i], SDL_CONTROLLER_AXIS_LEFTY);
        int x = SDL_GameControllerGetAxis(A.pads[i], SDL_CONTROLLER_AXIS_LEFTX);
        if (y < -20000) dir = 1; else if (y > 20000) dir = 2; else if (x < -20000) dir = 3; else if (x > 20000) dir = 4;
        if (dir) break;
    }
    uint64_t t = now_ticks();
    if (dir && (dir != last_dir || t >= next_repeat)) {
        if (dir == 1) A.nv.up = 1; else if (dir == 2) A.nv.down = 1; else if (dir == 3) A.nv.left = 1; else A.nv.right = 1;
        next_repeat = t + A.perf_freq * (dir != last_dir ? 350u : 110u) / 1000u;
    }
    last_dir = dir;
}

static int hotkey_event(const SDL_Event *e, int hk)
{
    const binding *b = &A.hk[hk];
    if (e->type == SDL_KEYDOWN && !e->key.repeat)
        return (b->key[0] >= 0 && e->key.keysym.scancode == b->key[0]) || (b->key[1] >= 0 && e->key.keysym.scancode == b->key[1]);
    if (e->type == SDL_CONTROLLERBUTTONDOWN) return b->pad >= 0 && b->pad < FE_PAD_AXIS_BASE && e->cbutton.button == b->pad;
    return 0;
}

static void run_one_frame(void)
{
    uint32_t mask;
    if (A.movie_play) {
        if (A.movie_play_at + 4 > A.movie_play_size) { movie_stop(); mask = 0; }
        else { mask = get_u32(A.movie_play + A.movie_play_at); A.movie_play_at += 4; }
    } else {
        mask = sample_game_inputs();
    }
    if (A.movie_rec) { uint8_t b[4]; put_u32(b, mask); fwrite(b, 1, 4, A.movie_rec); }
    A.game_mask = mask;
    A.g->set_inputs(mask);
    int frames = 0;
    char err[300] = "";
    uint64_t t0 = now_ticks();
    int rc = A.g->run_frame(A.audio_buf, &frames, err, sizeof(err));
    double ms = ms_since(t0);
    A.frame_ms_avg = A.frame_ms_avg * 0.95 + ms * 0.05;
    A.frames_total++;
    A.fps_frames++;
    if (rc != 0) {
        A.session = 0;
        show_message("The game stopped", "%s\n\nThe native translation does not interpret instructions, so it stops "
                     "instead of guessing. Your autosave and slots are unaffected. The log has details:\n%s", err, A.log_path);
        return;
    }
    audio_push(A.audio_buf, frames);
    if (A.rw.snap && A.frames_total % 4 == 0) rewind_capture();
    if (A.frames_total % 120 == 0) nvram_store(0);   /* settings/high scores survive a crash or power loss */
}

static void present(void)
{
    SDL_GetRendererOutputSize(A.renderer, &A.win_w, &A.win_h);
    int s1 = A.win_w * 100 / 1280, s2 = A.win_h * 100 / 760;
    A.ui_scale_px = s1 < s2 ? s1 : s2;
    if (A.ui_scale_px < 45) A.ui_scale_px = 45;
    SDL_SetRenderDrawColor(A.renderer, 0, 0, 0, 255);
    SDL_RenderClear(A.renderer);
    if (A.session && (!A.in_menu || top() == SCR_QUICK || top() == SCR_CONFIRM || top() == SCR_SAVE || top() == SCR_LOAD || top() == SCR_MESSAGE)) {
        draw_background(A.win_w, A.win_h);
        draw_game(A.win_w, A.win_h, A.in_menu);
        if (!A.in_menu) draw_gadgets();
        if (A.in_menu) fe_gradient_v(A.renderer, 0, 0, A.win_w * 55 / 100, A.win_h, rgba(8, 10, 16, 235), rgba(8, 10, 16, 200));
    } else if (A.in_menu) {
        fe_gradient_v(A.renderer, 0, 0, A.win_w, A.win_h, rgba(20, 23, 33, 255), rgba(7, 8, 12, 255));
        fe_color a = accent();
        fe_fill(A.renderer, 0, 0, A.win_w, U(3), with_alpha(a, 200));
    }
    if (A.in_menu) {
        screen_id s = top();
        /* screens below dialogs stay visible */
        if (s == SCR_CONFIRM || s == SCR_MESSAGE || s == SCR_CAPTURE) {
            nav saved = A.nv;
            memset(&A.nv, 0, sizeof(A.nv));
            A.nv.mouse_x = -1;
            A.depth--;
            screen_id under = top();
            if (A.depth > 0 && under != SCR_CONFIRM && under != SCR_MESSAGE && under != SCR_CAPTURE) {
                switch (under) {
                case SCR_MAIN: scr_main(); break; case SCR_QUICK: scr_quick(); break;
                case SCR_SAVE: scr_slots(1); break; case SCR_LOAD: scr_slots(0); break;
                case SCR_CONTROLS: scr_controls(); break; case SCR_HOTKEYS: scr_hotkeys(); break;
                case SCR_EXTRAS: scr_extras(); break; case SCR_GAMEPLAY: scr_gameplay(); break;
                case SCR_ROM: scr_rom(); break; case SCR_DISPLAY: scr_display(); break;
                case SCR_AUDIO: scr_audio(); break;
                default: break;
                }
            }
            A.depth++;
            A.nv = saved;
        }
        switch (s) {
        case SCR_MAIN: scr_main(); break;
        case SCR_QUICK: scr_quick(); break;
        case SCR_DISPLAY: scr_display(); break;
        case SCR_AUDIO: scr_audio(); break;
        case SCR_CONTROLS: scr_controls(); break;
        case SCR_CONTROLS_P: scr_controls_players(); break;
        case SCR_HOTKEYS: scr_hotkeys(); break;
        case SCR_GAMEPLAY: scr_gameplay(); break;
        case SCR_GADGETS: scr_gadgets(); break;
        case SCR_GADGET_EDIT: scr_gadget_edit(); break;
        case SCR_EXTRAS: scr_extras(); break;
        case SCR_ABOUT: scr_about(); break;
        case SCR_SAVE: scr_slots(1); break;
        case SCR_LOAD: scr_slots(0); break;
        case SCR_ROM: scr_rom(); break;
        case SCR_CONFIRM: scr_confirm(); break;
        case SCR_MESSAGE: scr_message(); break;
        case SCR_CAPTURE: scr_capture(); break;
        default: break;
        }
        if (A.depth == 0) {
            if (A.session) close_menu();
            else push(SCR_MAIN);
        }
    }
    if (A.paused && !A.in_menu) {
        fe_text(A.renderer, "PAUSED", A.win_w / 2, U(30), U(40), rgba(255, 255, 255, 230), 1);
    }
    if (A.rewinding) fe_text(A.renderer, "<< REWIND", U(30), U(30), U(30), accent(), 0);
    if (A.movie_status[0] && !A.in_menu) fe_text(A.renderer, A.movie_status, A.win_w - U(24), A.win_h - U(40), U(18), rgba(255, 90, 90, 220), 2);
    draw_toast();
    SDL_RenderPresent(A.renderer);
    A.present_frames++;
}

static void print_help(const char *exe)
{
    printf("%s\n"
           "  --rom PATH        ROM archive (%s)\n"
           "  --fullscreen      start fullscreen\n"
           "  --windowed        start in a window\n"
           "  --scale N         window size multiple\n"
           "  --window WxH      exact window size (e.g. 1280x800 to preview Steam Deck)\n"
           "  --portable        keep settings and saves beside the executable\n"
           "  --state FILE      load a save-state file at start\n"
           "  --no-menu         start the game immediately\n"
           "  --rotate N        picture rotation in quarter turns (0-3; default: original)\n"
           "  --frames N        quit after N frames (automated checks)\n"
           "  --screenshot FILE write a PNG of the last frame at exit\n"
           "  --profile FILE    use this game profile (JSON) instead of profiles/<id>.json\n", exe, A.g->rom_hint);
}

static void autodetect_rom(void)
{
    if (A.rom_path[0] && fe_file_exists(A.rom_path)) return;
    char cands[6][FE_PATH_MAX];
    int n = 0;
    char *bp = SDL_GetBasePath();
    if (bp) { fe_path_join(cands[n++], FE_PATH_MAX, bp, A.g->rom_hint); SDL_free(bp); }
    fe_path_join(cands[n++], FE_PATH_MAX, A.base_dir, A.g->rom_hint);
    char roms[FE_PATH_MAX];
    fe_path_join(roms, sizeof(roms), A.base_dir, "roms");
    fe_path_join(cands[n++], FE_PATH_MAX, roms, A.g->rom_hint);
    char home[FE_PATH_MAX];
    if (fe_home_dir(home, sizeof(home))) {
        char dl[FE_PATH_MAX];
        fe_path_join(dl, sizeof(dl), home, "Downloads");
        fe_path_join(cands[n++], FE_PATH_MAX, dl, A.g->rom_hint);
        fe_path_join(dl, sizeof(dl), home, "roms");
        fe_path_join(cands[n++], FE_PATH_MAX, dl, A.g->rom_hint);
    }
    snprintf(cands[n++], FE_PATH_MAX, "%s", A.g->rom_hint);
    for (int i = 0; i < n; i++) {
        if (fe_file_exists(cands[i])) { snprintf(A.rom_path, sizeof(A.rom_path), "%s", cands[i]); fe_log("found ROM at %s", cands[i]); return; }
    }
}

/* Find and apply profiles/<name>.json; see fe_profile.h.  Search order: the
 * --profile argument, the data folder (user override), next to the
 * executable, and the share/ folder of an installed or AppImage layout. */
static void setup_profile(const char *arg)
{
    const char *name = A.g->profile_name ? A.g->profile_name : A.g->id;
    char file[96], path[FE_PATH_MAX], tmp[FE_PATH_MAX], exe_dir[FE_PATH_MAX] = "";
    char *bp = SDL_GetBasePath();
    if (bp) { snprintf(exe_dir, sizeof(exe_dir), "%s", bp); SDL_free(bp); }
    snprintf(file, sizeof(file), "%s.json", name);
    const char *found = NULL;
    if (arg) { snprintf(path, sizeof(path), "%s", arg); found = fe_file_exists(path) ? path : NULL; }
    if (!found) { fe_path_join(tmp, sizeof(tmp), A.base_dir, "profiles"); fe_path_join(path, sizeof(path), tmp, file); if (fe_file_exists(path)) found = path; }
    if (!found && exe_dir[0]) { fe_path_join(tmp, sizeof(tmp), exe_dir, "profiles"); fe_path_join(path, sizeof(path), tmp, file); if (fe_file_exists(path)) found = path; }
    if (!found && exe_dir[0]) {
        char share[FE_PATH_MAX];
        snprintf(share, sizeof(share), "%s../share/%s/profiles", exe_dir, A.g->id);
        fe_path_join(path, sizeof(path), share, file);
        if (fe_file_exists(path)) found = path;
    }
    int supported[FE_FEAT_COUNT] = { 0 };
    int states = A.g->save_state && A.g->load_state && A.g->validate_state;
    supported[FE_FEAT_SAVE_STATES] = states;
    supported[FE_FEAT_REWIND] = states;
    supported[FE_FEAT_MOVIES] = states;            /* a movie starts from a state */
    supported[FE_FEAT_NVRAM] = A.g->nvram_save && A.g->nvram_load;
    supported[FE_FEAT_GADGETS] = 1;
    supported[FE_FEAT_INPUT_DISPLAY] = 1;
    supported[FE_FEAT_SCREENSHOTS] = 1;
    supported[FE_FEAT_WIDESCREEN] = 0;             /* no adapter offers a wider native view */

    if (found) {
        if (fe_profile_load(&A.profile, found)) fe_log("profile: %s", found);
        else { fe_log("profile %s rejected: %s (using adapter defaults)", found, A.profile.error); memset(&A.profile, 0, sizeof(A.profile)); }
    } else fe_log("profile: none found for '%s' (using adapter defaults)", name);

    fe_profile *p = &A.profile;
    if (p->loaded && p->id[0] && strcmp(p->id, A.g->id)) {
        fe_log("profile id '%s' does not match game '%s'; ignoring it", p->id, A.g->id);
        memset(p, 0, sizeof(*p));
    }
    if (p->loaded) {
        if (p->title[0]) A.game.title = p->title;
        if (p->subtitle[0]) A.game.subtitle = p->subtitle;
        if (p->rom_hint[0]) A.game.rom_hint = p->rom_hint;
        if (p->aspect > 0) A.game.display_aspect = p->aspect;
        if (p->players > 0 && p->players != A.g->players)
            fe_log("profile asks for %d players; the adapter defines %d (adapter wins)", p->players, A.g->players);
        if (p->rotation >= 0) {
            if (p->rotation % 90 == 0) A.game.rotation = (p->rotation / 90) & 3;
            else fe_log("profile rotation %d is not a multiple of 90; ignored", p->rotation);
        }
    }
    size_t at = 0;
    A.feat_report[0] = 0;
    for (int f = 0; f < FE_FEAT_COUNT; f++) {
        int req = p->loaded ? p->requested[f] : -1;
        A.feat[f] = supported[f] && req != 0;
        const char *state = A.feat[f] ? "on" : !supported[f] ? (req == 1 ? "unsupported" : "n/a") : "off";
        fe_log("feature %-14s requested=%s supported=%s -> %s", fe_feature_name(f),
               req < 0 ? "-" : req ? "yes" : "no", supported[f] ? "yes" : "no", state);
        if (at < sizeof(A.feat_report) - 40)
            at += (size_t)snprintf(A.feat_report + at, sizeof(A.feat_report) - at, "%s%s: %s", f ? ",  " : "", fe_feature_name(f), state);
    }
}

int fe_run(const fe_game *game, int argc, char **argv)
{
    memset(&A, 0, sizeof(A));
    A.game = *game;
    A.g = &A.game;
    game = &A.game;
    const char *profile_arg = NULL;
    int portable = 0, no_menu = 0, force_fs = -1, scale = 0, win_w_arg = 0, win_h_arg = 0;
    const char *rom_arg = NULL, *state_arg = NULL;
    int rotate_arg = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_arg = argv[++i];
        else if (!strcmp(argv[i], "--fullscreen")) force_fs = 1;
        else if (!strcmp(argv[i], "--windowed")) force_fs = 0;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &win_w_arg, &win_h_arg) != 2) win_w_arg = win_h_arg = 0; }
        else if (!strcmp(argv[i], "--portable")) portable = 1;
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile_arg = argv[++i];
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) state_arg = argv[++i];
        else if (!strcmp(argv[i], "--no-menu")) no_menu = 1;
        else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) rotate_arg = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) A.auto_exit_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) snprintf(A.auto_screenshot, sizeof(A.auto_screenshot), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { print_help(argv[0]); return 0; }
        else if (argv[i][0] != '-' && !rom_arg) rom_arg = argv[i];   /* drag-and-drop onto the exe */
    }

    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
#ifdef SDL_HINT_APP_NAME
    SDL_SetHint(SDL_HINT_APP_NAME, game->title);
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    setup_dirs(portable);
    fe_log_open(A.log_path);
    fe_log("%s starting; data folder %s", game->title, A.base_dir);
    setup_profile(profile_arg);   /* first: the profile can set the default orientation */
    load_settings();
    if (!A.feat[FE_FEAT_REWIND]) A.s.rewind_enabled = 0;
    if (rotate_arg >= 0) A.s.rotation = rotate_arg & 3;
    if (A.slot < 1 || A.slot > FE_SLOTS) A.slot = 1;
    if (force_fs >= 0) A.s.fullscreen = force_fs;
    if (scale > 0) A.s.window_scale = scale;
    A.perf_freq = SDL_GetPerformanceFrequency();
    A.lut_dirty = 1;

    int ww, wh;
    window_size(A.s.window_scale, &ww, &wh);
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        while ((ww > dm.w - 40 || wh > dm.h - 80) && A.s.window_scale > 1) {
            A.s.window_scale--;
            window_size(A.s.window_scale, &ww, &wh);
        }
    }
    if (win_w_arg >= 320 && win_h_arg >= 240) { ww = win_w_arg; wh = win_h_arg; }
    A.window = SDL_CreateWindow(game->title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                (A.s.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!A.window) { fe_log("SDL_CreateWindow failed: %s", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_SetWindowMinimumSize(A.window, 320, 240);
    A.renderer = SDL_CreateRenderer(A.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE | (A.s.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!A.renderer) A.renderer = SDL_CreateRenderer(A.window, -1, SDL_RENDERER_SOFTWARE);
    if (!A.renderer) { fe_log("SDL_CreateRenderer failed: %s", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(A.renderer, &ri) == 0) fe_log("renderer: %s", ri.name);
    if (!fe_draw_init(A.renderer)) fe_log("font atlas failed to initialise");
    A.game_tex = SDL_CreateTexture(A.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, game->width, game->height);
    A.pixels = (uint32_t *)calloc((size_t)game->width * game->height, sizeof(uint32_t));
    A.pixels555 = (uint16_t *)calloc((size_t)game->width * game->height, sizeof(uint16_t));
    A.argb = (uint32_t *)calloc((size_t)game->width * game->height, sizeof(uint32_t));
    A.audio_buf = (int16_t *)calloc((size_t)game->max_audio_frames * 2 + 16, sizeof(int16_t));
    if (!A.game_tex || !A.pixels || !A.argb || !A.audio_buf) { fe_log("out of memory"); SDL_Quit(); return 1; }
    apply_filter();
    open_pads();
    audio_open();

    if (rom_arg) snprintf(A.rom_path, sizeof(A.rom_path), "%s", rom_arg);
    autodetect_rom();
    if (A.rom_path[0]) load_rom(A.rom_path);
    else snprintf(A.rom_error, sizeof(A.rom_error), "No ROM selected. Choose ROM Selection and pick %s.", game->rom_hint);
    if (A.rom_ok) save_settings();
    upload_frame();

    if (A.rom_ok && state_arg) {
        size_t size; char err[200] = "";
        uint8_t *f = fe_read_file(state_arg, &size);
        size_t cs;
        const uint8_t *core = f ? parse_state_file(f, size, NULL, &cs, err, sizeof(err)) : NULL;
        if (core && game->load_state(core, cs, err, sizeof(err))) { A.session = 1; render_frame(); upload_frame(); no_menu = 1; }
        else fe_log("--state %s rejected: %s", state_arg, f ? err : "cannot read file");
        free(f);
    }
    if (A.rom_ok && no_menu && !A.session) start_new_game();
    if (!A.session) open_menu(SCR_MAIN);
    if (A.rom_ok) refresh_slots();

    A.last_tick = now_ticks();
    A.fps_t0 = A.last_tick;
    const double frame_ms = 1000.0 / game->refresh_hz;

    while (!A.quit) {
        memset(&A.nv, 0, sizeof(A.nv));
        A.nv.mouse_x = -1; A.nv.mouse_y = -1;
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        A.nv.mouse_x = mx; A.nv.mouse_y = my;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                if (A.session) { end_session(A.s.autosave_exit); }
                A.quit = 1;
            } else if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) {
                open_pads();
                clear_game_inputs();
                toast(e.type == SDL_CONTROLLERDEVICEADDED ? "Controller connected" : "Controller disconnected");
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                clear_game_inputs();
                if (A.session && !A.in_menu && A.s.pause_on_focus) open_menu(SCR_QUICK);
            } else if (e.type == SDL_DROPFILE) {
                if (e.drop.file) { push(SCR_ROM); try_rom(e.drop.file); SDL_free(e.drop.file); }
            } else if (A.in_menu && top() == SCR_CAPTURE) {
                capture_event(&e);
            } else if (A.in_menu) {
                handle_menu_events(&e);
            } else {
                if (hotkey_event(&e, HK_MENU) || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE && !e.key.repeat))
                    open_menu(SCR_QUICK);
                else if (hotkey_event(&e, HK_QSAVE)) { if (feat(FE_FEAT_SAVE_STATES)) save_slot(A.slot); }
                else if (hotkey_event(&e, HK_QLOAD)) { if (feat(FE_FEAT_SAVE_STATES)) load_slot(A.slot, 0); }
                else if (hotkey_event(&e, HK_SLOT_PREV)) { A.slot = A.slot <= 1 ? FE_SLOTS : A.slot - 1; toast("Slot %d", A.slot); }
                else if (hotkey_event(&e, HK_SLOT_NEXT)) { A.slot = A.slot >= FE_SLOTS ? 1 : A.slot + 1; toast("Slot %d", A.slot); }
                else if (hotkey_event(&e, HK_SHOT)) screenshot();
                else if (hotkey_event(&e, HK_FULLSCREEN)) { A.s.fullscreen = !A.s.fullscreen; apply_fullscreen(); save_settings(); }
                else if (hotkey_event(&e, HK_PAUSE)) { A.paused = !A.paused; clear_game_inputs(); audio_flush(); }
                else if (hotkey_event(&e, HK_FPS)) { A.s.gadget[0].enabled = !A.s.gadget[0].enabled; }
                else if (hotkey_event(&e, HK_RESET)) { ask(CF_RESTART, 0, "Restart the game? Unsaved progress will be lost."); A.in_menu = 1; A.depth = 1; A.stack[0] = SCR_QUICK; }
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
                A.s.fullscreen = !A.s.fullscreen; apply_fullscreen();
            }
        }
        if (A.in_menu) stick_nav();
        /* controller menu combo, edge-triggered */
        int combo = menu_combo_down();
        if (combo && !A.combo_latch && A.session && !A.in_menu) open_menu(SCR_QUICK);
        A.combo_latch = combo;

        uint64_t t = now_ticks();
        double elapsed = (double)(t - A.last_tick) * 1000.0 / (double)A.perf_freq;
        A.last_tick = t;
        if (A.session && !A.in_menu && !A.paused) {
            const uint8_t *keys = SDL_GetKeyboardState(NULL);
            A.rewinding = A.rw.snap && binding_down(&A.hk[HK_REWIND], 0, keys);
            if (A.rewinding) {
                if (rewind_step()) { render_frame(); upload_frame(); }
                audio_flush();
                A.accumulator = 0;
            } else {
                int refresh_ok = 0;
                if (A.s.pacing == 1 && A.s.vsync) {
                    int idx = SDL_GetWindowDisplayIndex(A.window);
                    SDL_DisplayMode m;
                    if (SDL_GetCurrentDisplayMode(idx < 0 ? 0 : idx, &m) == 0 && m.refresh_rate >= 59 && m.refresh_rate <= 61) refresh_ok = 1;
                }
                if (refresh_ok) {
                    run_one_frame();
                } else {
                    A.accumulator += elapsed;
                    if (A.accumulator > frame_ms * 4) A.accumulator = frame_ms * 4;  /* after a stall: don't spiral */
                    int ran = 0;
                    while (A.accumulator >= frame_ms && A.session && ran < 4) {
                        run_one_frame();
                        A.accumulator -= frame_ms;
                        ran++;
                    }
                }
                if (A.session) { render_frame(); upload_frame(); }
            }
        }
        double since = ms_since(A.fps_t0);
        if (since >= 1000.0) {
            A.fps = A.fps_frames * 1000.0 / since;
            A.present_fps = A.present_frames * 1000.0 / since;
            A.fps_frames = A.present_frames = 0;
            A.fps_t0 = now_ticks();
        }
        uint64_t tp = now_ticks();
        present();
        {
            static double present_ms, loop_ms; static int n; static uint64_t last;
            present_ms += ms_since(tp);
            loop_ms += last ? (double)(tp - last) * 1000.0 / (double)A.perf_freq : 0;
            last = tp;
            if (++n == 120) {
                if (getenv("FE_PROFILE")) fe_log("profile: loop %.2f ms, present %.2f ms, game frame %.2f ms, fps %.1f",
                                                 loop_ms / n, present_ms / n, A.frame_ms_avg, A.fps);
                present_ms = loop_ms = 0; n = 0;
            }
        }
        if (!A.s.vsync || A.in_menu || !A.session) {
            /* without vsync, sleep until the next game frame is due */
            double wait = A.in_menu || !A.session ? 8.0 : frame_ms - A.accumulator - 1.0;
            if (wait > 1.0) SDL_Delay((Uint32)wait);
        }
        if (A.auto_exit_frames && A.frames_total >= (uint64_t)A.auto_exit_frames) A.quit = 1;
    }

    if (A.auto_screenshot[0]) {
        int w = game->width, h = game->height;
        uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
        for (int i = 0; rgb && i < w * h; i++) {
            uint32_t c = A.pixels[i];
            rgb[i * 3] = (uint8_t)(c >> 16); rgb[i * 3 + 1] = (uint8_t)(c >> 8); rgb[i * 3 + 2] = (uint8_t)c;
        }
        if (rgb) fe_write_png(A.auto_screenshot, rgb, w, h);
        free(rgb);
    }
    fe_log("exit: %llu frames, %d audio underruns, %d drops", (unsigned long long)A.frames_total, A.audio_underruns, A.audio_drops);
    save_settings();
    nvram_store(0);
    movie_stop();
    rewind_free();
    for (int i = 0; i <= FE_SLOTS; i++) if (A.slots[i].thumb) SDL_DestroyTexture(A.slots[i].thumb);
    audio_close();
    close_pads();
    if (game->unload) game->unload();
    fe_draw_shutdown();
    if (A.mask_tex) SDL_DestroyTexture(A.mask_tex);
    if (A.scan_tex) SDL_DestroyTexture(A.scan_tex);
    if (A.ambient_tex) SDL_DestroyTexture(A.ambient_tex);
    SDL_DestroyTexture(A.game_tex);
    SDL_DestroyRenderer(A.renderer);
    SDL_DestroyWindow(A.window);
    free(A.pixels); free(A.pixels555); free(A.argb); free(A.audio_buf); free(A.entries);
    fe_ini_free(&A.ini);
    fe_log_close();
    SDL_Quit();
    return 0;
}
