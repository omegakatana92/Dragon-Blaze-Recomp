/* Psikyo PS5 board model (Dragon Blaze).
 *
 * Reference: MAME 0.264 src/mame/psikyo/psikyosh.cpp, psikyosh_v.cpp,
 * src/devices/machine/eepromser.cpp, src/devices/sound/ymopl.cpp and
 * src/devices/cpu/sh/sh7604.cpp (behaviour only; none of it is linked).
 * The YMF278B is the ymfm library (BSD-3-Clause), as in MAME. */
#ifndef DB_BOARD_H
#define DB_BOARD_H

#include <stddef.h>
#include <stdint.h>
#include "sh2/sh2.h"
#include "db/ymf278b.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DB_MASTER_CLOCK 57272700u
#define DB_CPU_CLOCK (DB_MASTER_CLOCK / 2)     /* SH-2 and YMF278B: 28.63635 MHz */
#define DB_REFRESH 60.0
#define DB_SCREEN_W 320                         /* visible area, before the 270-degree rotation */
#define DB_SCREEN_H 224
#define DB_BITMAP_W 512                         /* MAME screen bitmap (set_size 64*8 x 32*8) */
#define DB_BITMAP_H 256
#define DB_AUDIO_RATE (DB_CPU_CLOCK / 768)      /* ymf278b::sample_rate(): 37286 Hz */
#define DB_MAX_FRAME_SAMPLES 1024

#define DB_PROG_SIZE 0x180000u                  /* "maincpu" region: program + data ROM window */
#define DB_GFX_SIZE 0x2c00000u
#define DB_YMF_SIZE 0x400000u
#define DB_EEPROM_SIZE 256

/* inputs, active high */
enum {
    DB_IN_P1_UP = 1u << 0, DB_IN_P1_DOWN = 1u << 1, DB_IN_P1_LEFT = 1u << 2, DB_IN_P1_RIGHT = 1u << 3,
    DB_IN_P1_B1 = 1u << 4, DB_IN_P1_B2 = 1u << 5, DB_IN_P1_B3 = 1u << 6, DB_IN_P1_START = 1u << 7,
    DB_IN_P2_UP = 1u << 8, DB_IN_P2_DOWN = 1u << 9, DB_IN_P2_LEFT = 1u << 10, DB_IN_P2_RIGHT = 1u << 11,
    DB_IN_P2_B1 = 1u << 12, DB_IN_P2_B2 = 1u << 13, DB_IN_P2_B3 = 1u << 14, DB_IN_P2_START = 1u << 15,
    DB_IN_COIN1 = 1u << 16, DB_IN_COIN2 = 1u << 17, DB_IN_SERVICE1 = 1u << 18, DB_IN_TEST = 1u << 19
};

/* JP4 region jumpers (MAME PORT_DIPNAME 0x03000000, default International Ver B.) */
#define DB_REGION_JAPAN 0x00000000u
#define DB_REGION_INTL_A 0x02000000u
#define DB_REGION_INTL_B 0x01000000u
#define DB_DIP_DEBUG 0x40u        /* INPUTS bit 6 "Tilt (Enables Debug Mode)", active low */

/* 93C56 serial EEPROM, 256 x 8 bits (MAME eeprom_serial_93cxx_device) */
typedef struct db_eeprom {
    uint8_t data[DB_EEPROM_SIZE];
    uint8_t state, cs, clk, di, locked, command;
    uint32_t bits_accum, command_address_accum, address, shift_register;
    uint64_t completion_time;        /* CPU clock */
    uint64_t last_cs_rising_edge;    /* CPU clock */
    uint8_t dirty;
} db_eeprom;

/* SH7604 on-chip modules used by the board software */
typedef struct db_onchip {
    uint8_t regs[0x200];               /* 0xFFFFFE00-0xFFFFFFFF raw register file */
    uint32_t dvsr, dvdntl, dvdnth;
    uint8_t divu_ovf, divu_ovfie;
    uint8_t tier, ftcsr, frc_tcr, tocr;
    uint16_t frc, ocra, ocrb;
    uint64_t frc_base;
    uint64_t frt_event;
} db_onchip;

typedef struct db_board {
    sh2_state cpu;

    uint32_t *prog;         /* DB_PROG_SIZE/4 words as the SH-2 reads them */
    uint8_t *gfx;           /* "gfx1" region, byte order as MAME's region */
    uint8_t *ymf_rom;       /* "ymf" region */
    uint8_t eeprom_default[DB_EEPROM_SIZE];

    uint32_t ram[0x100000 / 4];
    uint32_t spriteram[0x10000 / 4];
    uint32_t paletteram[0x5000 / 4];
    uint32_t zoomram[0x200 / 4];
    uint32_t vidregs[0x20 / 4];
    uint32_t pens[0x5000 / 4];         /* rgb_t (0xFF rr gg bb) as MAME's palette */

    /* video (MAME psikyosh_v.cpp state) */
    struct db_sprite { int32_t ypos, xpos; uint8_t high, wide, flpy, flpx, spr_pri, bg_pri, zoomy, zoomx;
                       uint32_t tnum; uint16_t colr; uint8_t dpth; int16_t alpha; } sprites[0x800 / 2];
    int sprite_count;
    uint8_t zoom_bitmap[256 * 256];
    uint16_t z_bitmap[DB_BITMAP_H * DB_BITMAP_W];
    uint32_t bg_bitmap[512 * 512];
    uint32_t bitmap[DB_BITMAP_H * DB_BITMAP_W];   /* screen bitmap (ARGB as MAME's rgb32) */
    uint32_t screen[DB_SCREEN_H * DB_SCREEN_W];   /* last frame, xRGB8888 */

    db_eeprom eeprom;
    db_onchip onchip;
    db_ymf *ymf;
    int64_t ymf_timer[2];             /* CPU clock of expiry, -1 = off */
    int64_t ymf_now;                  /* time seen by the chip during a timer callback (-1: CPU time) */
    uint64_t audio_samples;           /* samples generated since power-on */

    /* translated program in RAM (see sh2.h): differing words per 4 KiB page */
    uint32_t ram_code_size, ram_code_src;
    uint32_t ram_code_dirty[0x100000 / 4096];

    uint32_t inputs;                  /* active-high DB_IN_* */
    uint32_t region;                  /* DB_REGION_* */
    uint8_t debug_dip;                /* 1 = "Tilt (Enables Debug Mode)" on */
    uint64_t frames_run;

    char error[512];
} db_board;

int  db_board_load(db_board *b, const char *zip_path);
int  db_rom_check(const char *zip_path, char *err, size_t n);
uint32_t db_board_rom_fingerprint(const db_board *b);
void db_board_free(db_board *b);
void db_board_reset(db_board *b);
void db_board_attach(db_board *b);
void db_board_set_inputs(db_board *b, uint32_t active_high);
void db_board_ram_code_recount(db_board *b);

#define DB_RUN_OK 0
#define DB_RUN_MISSING 1
int db_board_run_frame(db_board *b, int16_t *mono, int *frames);

/* video.c */
void db_video_reset(db_board *b);
void db_video_update(db_board *b);     /* MAME screen_update at VBLANK */
void db_video_get_sprites(db_board *b);/* MAME screen_vblank */
void db_video_render(const db_board *b, uint32_t *pixels);
void db_palette_write(db_board *b, unsigned index);

/* eeprom.c */
void db_eeprom_reset(db_eeprom *e);
void db_eeprom_write_lines(db_eeprom *e, uint64_t now, int di, int cs, int clk);
int db_eeprom_do_read(const db_eeprom *e, uint64_t now);

#ifdef __cplusplus
}
#endif
#endif
