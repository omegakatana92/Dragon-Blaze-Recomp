/* Dragon Blaze - native port, SDL2 frontend adapter.
 *
 * Connects the Psikyo PS5 board runtime (runtime/) and the statically
 * translated SH-2 program (build/generated/, or the fallback interpreter in a
 * generic build) to the shared frontend (frontend/).  The ROM set is read
 * from the player's archive at run time; no ROM data is built in. */
#include "fe.h"
#include "db/board.h"
#include "db/state.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DB_VERSION
#define DB_VERSION "0.1.0"
#endif

static db_board *board;
static int loaded;
static uint16_t dip_word = 0x0001;   /* bits 0-1: region (International Ver. B), bit 2: debug DIP */

#define K(x) SDL_SCANCODE_##x
#define P(x) SDL_CONTROLLER_BUTTON_##x
/* MAME psikyosh.cpp INPUT_PORTS_START(common): two players, 8-way stick and
 * three buttons each (shot, bomb, button 3), start, two coin slots, the
 * service coin and the service (test) switch. */
static const fe_input_def INPUTS[] = {
    { "p1_up", "P1 Up", 1, DB_IN_P1_UP, K(UP), K(W), P(DPAD_UP), 0 },
    { "p1_down", "P1 Down", 1, DB_IN_P1_DOWN, K(DOWN), K(S), P(DPAD_DOWN), 0 },
    { "p1_left", "P1 Left", 1, DB_IN_P1_LEFT, K(LEFT), K(A), P(DPAD_LEFT), 0 },
    { "p1_right", "P1 Right", 1, DB_IN_P1_RIGHT, K(RIGHT), K(D), P(DPAD_RIGHT), 0 },
    { "p1_b1", "P1 Shot (Button 1)", 1, DB_IN_P1_B1, K(Z), K(J), P(A), 0 },
    { "p1_b2", "P1 Bomb (Button 2)", 1, DB_IN_P1_B2, K(X), K(K), P(B), 0 },
    { "p1_b3", "P1 Button 3", 1, DB_IN_P1_B3, K(C), K(L), P(X), 0 },
    { "p1_start", "P1 Start", 1, DB_IN_P1_START, K(RETURN), K(1), P(START), 0 },
    { "p2_up", "P2 Up", 2, DB_IN_P2_UP, K(I), -1, P(DPAD_UP), 0 },
    { "p2_down", "P2 Down", 2, DB_IN_P2_DOWN, K(COMMA), -1, P(DPAD_DOWN), 0 },
    { "p2_left", "P2 Left", 2, DB_IN_P2_LEFT, K(U), -1, P(DPAD_LEFT), 0 },
    { "p2_right", "P2 Right", 2, DB_IN_P2_RIGHT, K(O), -1, P(DPAD_RIGHT), 0 },
    { "p2_b1", "P2 Shot (Button 1)", 2, DB_IN_P2_B1, K(KP_1), -1, P(A), 0 },
    { "p2_b2", "P2 Bomb (Button 2)", 2, DB_IN_P2_B2, K(KP_2), -1, P(B), 0 },
    { "p2_b3", "P2 Button 3", 2, DB_IN_P2_B3, K(KP_3), -1, P(X), 0 },
    { "p2_start", "P2 Start", 2, DB_IN_P2_START, K(2), -1, P(START), 0 },
    /* coins are pulses, like a real coin switch */
    { "coin1", "Coin 1", 1, DB_IN_COIN1, K(5), -1, P(BACK), 5 },
    { "coin2", "Coin 2", 2, DB_IN_COIN2, K(6), -1, P(BACK), 5 },
    { "service1", "Service Coin", 0, DB_IN_SERVICE1, K(9), -1, -1, 5 },
    { "test", "Service Mode (Test)", 0, DB_IN_TEST, K(F2), -1, -1, 0 },
};

/* The board has no DIP switches for game settings: difficulty, lives,
 * coinage etc. are set in the game's own service menu (F2) and kept in the
 * EEPROM.  What is left are the region jumpers (JP4, read at power-on) and
 * the debug switch MAME labels "Tilt (Enables Debug Mode)". */
static const fe_dip_def DIPS[] = {
    { "region", "Region (JP4, applies after reset)", 0, 0x0003, 0x0001, 3,
      { { "Japan", 0x0000 }, { "International Ver. A", 0x0002 }, { "International Ver. B", 0x0001 } } },
    { "debug", "Debug switch (MAME: Tilt / Debug Mode)", 0, 0x0004, 0x0000, 2,
      { { "Off", 0x0000 }, { "On", 0x0004 } } },
};

static void apply_dips(void)
{
    board->region = (uint32_t)(dip_word & 3) << 24;
    board->debug_dip = (dip_word & 4) ? 1 : 0;
}

/* Scores: 32-bit binary at 0x06070F34 (P1) and 0x0607123C (P2).  Checked
 * against the HUD in MAME 0.264 reference runs: one-player game at frames
 * 1800-4200 (0, 600, 2200, 3200, 4300) and a two-player game at frames
 * 2400-4200 (P1 1600/4400/18300, P2 1200/4200/6600/14600); these are the
 * only work-RAM words that match (the HUD shows the value one frame late). */
static uint32_t ram32(uint32_t address) { return board->ram[(address - 0x06000000u) >> 2]; }
static int gadget_score1(char *t, size_t n) { if (!loaded) return 0; snprintf(t, n, "1P %9u", ram32(0x06070F34u)); return 1; }
static int gadget_score2(char *t, size_t n) { if (!loaded) return 0; snprintf(t, n, "2P %9u", ram32(0x0607123Cu)); return 1; }

static const fe_gadget_def GADGETS[] = {
    { "score_p1", "Score (Player 1)", gadget_score1 },
    { "score_p2", "Score (Player 2)", gadget_score2 },
};

static int g_validate(const char *path, char *err, size_t n) { return db_rom_check(path, err, n); }

static int g_load(const char *path, char *err, size_t n)
{
    if (!board) board = (db_board *)calloc(1, sizeof(db_board));
    if (!board) { snprintf(err, n, "out of memory"); return 0; }
    if (loaded) { db_board_free(board); loaded = 0; }
    sh2_program_init();
    if (!db_board_load(board, path)) { snprintf(err, n, "%s", board->error); return 0; }
    apply_dips();
    db_board_reset(board);
    loaded = 1;
    return 1;
}

static void g_unload(void) { if (loaded) db_board_free(board); loaded = 0; }
static void g_reset(void) { if (loaded) { apply_dips(); db_board_reset(board); } }
static void g_set_dips(const uint16_t *w) { dip_word = w[0]; if (loaded) apply_dips(); }
static void g_set_inputs(uint32_t m) { if (loaded) db_board_set_inputs(board, m); }

static int g_run(int16_t *audio, int *frames, char *err, size_t n)
{
    static int16_t mono[DB_MAX_FRAME_SAMPLES];
    int count = 0;
    int rc = db_board_run_frame(board, mono, &count);
    /* the board has one (mono) speaker; the frontend takes stereo */
    for (int i = 0; i < count; i++) audio[2 * i] = audio[2 * i + 1] = mono[i];
    if (frames) *frames = count;
    if (rc != DB_RUN_OK) {
        snprintf(err, n, "The SH-2 program stopped at address %08X (an instruction the runtime cannot execute).",
                 sh2_last_missing_pc());
        return 1;
    }
    return 0;
}

static void g_render32(uint32_t *px) { db_video_render(board, px); }
static uint8_t *g_save(size_t *size) { return db_state_save(board, size); }
static int g_validate_state(const uint8_t *d, size_t s, char *e, size_t n) { return db_state_validate(board, d, s, e, n); }
static int g_load_state(const uint8_t *d, size_t s, char *e, size_t n) { return db_state_load(board, d, s, e, n); }
static uint32_t g_fingerprint(void) { return loaded ? db_board_rom_fingerprint(board) : 0; }

/* 93C56 EEPROM (256 bytes): settings, bookkeeping and high scores.  The
 * file has the same layout as MAME's nvram/dragnblz/eeprom. */
static size_t g_nvram_save(uint8_t *buf, size_t max)
{
    if (!loaded || max < DB_EEPROM_SIZE) return 0;
    memcpy(buf, board->eeprom.data, DB_EEPROM_SIZE);
    board->eeprom.dirty = 0;
    return DB_EEPROM_SIZE;
}
static int g_nvram_load(const uint8_t *d, size_t size)
{
    if (!loaded || size != DB_EEPROM_SIZE) return 0;
    memcpy(board->eeprom.data, d, size);
    board->eeprom.dirty = 0;
    return 1;
}
static int g_nvram_dirty(void) { return loaded && board->eeprom.dirty; }

static const fe_game GAME = {
    .id = "dragnblz",
    .title = "Dragon Blaze",
    .subtitle = "native port",
    .about =
        "Dragon Blaze  (Psikyo, 2000, PS5 hardware: SH-2, PS6406B video, YMF278B)\n"
        "Native port " DB_VERSION ": the SH-2 game program is translated ahead of time to C;\n"
        "code the static analysis did not reach runs on a fallback SH-2 interpreter built from\n"
        "the same decoder, timing and instruction semantics. The board (sprite/tilemap video with\n"
        "zoom and alpha, line effects, EEPROM, SH7604 timer/divider) is reimplemented natively.\n"
        "\n"
        "Requires your own MAME 'dragnblz' ROM set (24 files). No game data is distributed.\n"
        "Hardware behaviour references MAME (BSD-3-Clause). Sound chip: ymfm (BSD-3-Clause).\n"
        "Frontend font: Inter (SIL Open Font License 1.1). SDL2 (zlib). miniz (MIT).\n"
        "This project's own code is GPL-2.0. See the LICENSES folder.",
    .rom_hint = "dragnblz.zip",
    .width = DB_SCREEN_W,
    .height = DB_SCREEN_H,
    .display_aspect = 4.0 / 3.0,
    .rotation = 3,                       /* ROT270: vertical monitor */
    .refresh_hz = DB_REFRESH,
    .audio_rate = DB_AUDIO_RATE,
    .max_audio_frames = DB_MAX_FRAME_SAMPLES,
    .dip_words = 1,
    .players = 2,
    .inputs = INPUTS, .input_count = (int)(sizeof(INPUTS) / sizeof(INPUTS[0])),
    .dips = DIPS, .dip_count = (int)(sizeof(DIPS) / sizeof(DIPS[0])),
    .gadgets = GADGETS, .gadget_count = (int)(sizeof(GADGETS) / sizeof(GADGETS[0])),
    .validate_rom = g_validate,
    .load = g_load,
    .unload = g_unload,
    .reset = g_reset,
    .set_dips = g_set_dips,
    .set_inputs = g_set_inputs,
    .run_frame = g_run,
    .render = NULL,
    .render32 = g_render32,
    .save_state = g_save,
    .validate_state = g_validate_state,
    .load_state = g_load_state,
    .rom_fingerprint = g_fingerprint,
    .runtime_version = DB_VERSION,
    .nvram_save = g_nvram_save,
    .nvram_load = g_nvram_load,
    .nvram_dirty = g_nvram_dirty,
    .profile_name = "dragnblz",
};

int main(int argc, char **argv)
{
    int rc = fe_run(&GAME, argc, argv);
    if (loaded) db_board_free(board);
    free(board);
    return rc;
}
