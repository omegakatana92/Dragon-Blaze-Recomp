/* Save-state serialization (see db/state.h).  One function walks the board
 * for both directions so the save and load layouts cannot drift apart. */
#include "db/state.h"
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t db_crc32(uint32_t crc, const void *data, size_t size)
{
    return (uint32_t)mz_crc32(crc, (const unsigned char *)data, size);
}

/* ---- stream ----------------------------------------------------------- */

typedef struct io {
    int writing;
    uint8_t *p;           /* write buffer, or read source */
    size_t n, cap, at;
    int bad;
} io;

static void io_bytes(io *s, void *v, size_t n)
{
    if (s->bad) { if (!s->writing) memset(v, 0, n); return; }
    if (s->writing) {
        if (s->n + n > s->cap) {
            size_t cap = s->cap ? s->cap : 1u << 18;
            while (cap < s->n + n) cap *= 2;
            uint8_t *p = (uint8_t *)realloc(s->p, cap);
            if (!p) { s->bad = 1; return; }
            s->p = p; s->cap = cap;
        }
        memcpy(s->p + s->n, v, n);
        s->n += n;
    } else {
        if (s->at + n > s->n) { s->bad = 1; memset(v, 0, n); return; }
        memcpy(v, s->p + s->at, n);
        s->at += n;
    }
}
static void io_u8(io *s, uint8_t *v) { io_bytes(s, v, 1); }
static void io_u16(io *s, uint16_t *v)
{
    uint8_t b[2] = { (uint8_t)*v, (uint8_t)(*v >> 8) };
    io_bytes(s, b, 2);
    if (!s->writing) *v = (uint16_t)(b[0] | b[1] << 8);
}
static void io_u32(io *s, uint32_t *v)
{
    uint8_t b[4] = { (uint8_t)*v, (uint8_t)(*v >> 8), (uint8_t)(*v >> 16), (uint8_t)(*v >> 24) };
    io_bytes(s, b, 4);
    if (!s->writing) *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void io_u64(io *s, uint64_t *v)
{
    uint32_t lo = (uint32_t)*v, hi = (uint32_t)(*v >> 32);
    io_u32(s, &lo); io_u32(s, &hi);
    if (!s->writing) *v = (uint64_t)hi << 32 | lo;
}
static void io_i32(io *s, int32_t *v) { uint32_t u = (uint32_t)*v; io_u32(s, &u); *v = (int32_t)u; }
static void io_int(io *s, int *v) { uint32_t u = (uint32_t)*v; io_u32(s, &u); *v = (int)(int32_t)u; }
static void io_u32a(io *s, uint32_t *a, size_t count) { for (size_t i = 0; i < count; i++) io_u32(s, &a[i]); }

/* Sections: 4-byte tag + 32-bit length; on load the tag and length must match. */
static size_t sec_begin(io *s, const char tag[4])
{
    char t[4];
    memcpy(t, tag, 4);
    io_bytes(s, t, 4);
    if (!s->writing && memcmp(t, tag, 4) != 0) s->bad = 2;
    size_t at = s->writing ? s->n : s->at;
    uint32_t len = 0;
    io_u32(s, &len);
    if (!s->writing) return (size_t)len | ((size_t)1 << 62);   /* remember the claimed length */
    return at;
}
static void sec_end(io *s, size_t mark, size_t start)
{
    if (s->bad) return;
    if (s->writing) {
        uint32_t len = (uint32_t)(s->n - mark - 4);
        memcpy(s->p + mark, &len, 4);   /* little-endian hosts only (x86-64, ARM64) */
    } else {
        size_t claimed = mark & ~((size_t)1 << 62);
        if (s->at - start != claimed) s->bad = 2;
    }
}
#define SECTION(tag, ...) do { size_t m_ = sec_begin(s, tag); size_t st_ = s->writing ? 0 : s->at; __VA_ARGS__; sec_end(s, m_, st_); } while (0)

static void io_i64(io *s, int64_t *v) { uint64_t u = (uint64_t)*v; io_u64(s, &u); *v = (int64_t)u; }

static void io_sh2(io *s, sh2_state *c)
{
    for (int i = 0; i < 16; i++) io_u32(s, &c->r[i]);
    io_u32(s, &c->pc); io_u32(s, &c->pr); io_u32(s, &c->sr); io_u32(s, &c->gbr); io_u32(s, &c->vbr);
    io_u32(s, &c->mach); io_u32(s, &c->macl); io_u32(s, &c->ea); io_u32(s, &c->delay);
    io_u64(s, &c->cycles); io_u64(s, &c->instructions);
    io_u32(s, &c->pending_irq); io_i32(s, &c->internal_irq_level); io_u32(s, &c->internal_irq_vector);
    io_u8(s, &c->pending_nmi); io_u8(s, &c->sleep_mode); io_u8(s, &c->test_irq); io_u8(s, &c->spin);
    io_u32(s, &c->held_lines);
}

static void io_eeprom(io *s, db_eeprom *e)
{
    io_bytes(s, e->data, sizeof(e->data));
    io_u8(s, &e->state); io_u8(s, &e->cs); io_u8(s, &e->clk); io_u8(s, &e->di); io_u8(s, &e->locked); io_u8(s, &e->command);
    io_u32(s, &e->bits_accum); io_u32(s, &e->command_address_accum); io_u32(s, &e->address); io_u32(s, &e->shift_register);
    io_u64(s, &e->completion_time); io_u64(s, &e->last_cs_rising_edge);
}

/* the YMF278B blob is carried separately: it belongs to the C++ chip object */
static void io_board(io *s, db_board *b, uint8_t **ymf_blob, uint32_t *ymf_size)
{
    SECTION("SH2 ", io_sh2(s, &b->cpu));
    SECTION("ONCH", {
        io_bytes(s, b->onchip.regs, sizeof(b->onchip.regs));
        io_u32(s, &b->onchip.dvsr); io_u32(s, &b->onchip.dvdntl); io_u32(s, &b->onchip.dvdnth);
        io_u8(s, &b->onchip.divu_ovf); io_u8(s, &b->onchip.divu_ovfie);
        io_u8(s, &b->onchip.tier); io_u8(s, &b->onchip.ftcsr); io_u8(s, &b->onchip.frc_tcr); io_u8(s, &b->onchip.tocr);
        io_u16(s, &b->onchip.frc); io_u16(s, &b->onchip.ocra); io_u16(s, &b->onchip.ocrb);
        io_u64(s, &b->onchip.frc_base); io_u64(s, &b->onchip.frt_event);
    });
    SECTION("RAM ", io_u32a(s, b->ram, sizeof(b->ram) / 4));
    SECTION("VRAM", {
        io_u32a(s, b->spriteram, sizeof(b->spriteram) / 4);
        io_u32a(s, b->paletteram, sizeof(b->paletteram) / 4);
        io_u32a(s, b->zoomram, sizeof(b->zoomram) / 4);
        io_u32a(s, b->vidregs, sizeof(b->vidregs) / 4);
    });
    SECTION("VID ", {
        io_int(s, &b->sprite_count);
        for (int i = 0; i < (int)(sizeof(b->sprites) / sizeof(b->sprites[0])); i++) {
            struct db_sprite *p = &b->sprites[i];
            io_i32(s, &p->ypos); io_i32(s, &p->xpos);
            io_u8(s, &p->high); io_u8(s, &p->wide); io_u8(s, &p->flpy); io_u8(s, &p->flpx);
            io_u8(s, &p->spr_pri); io_u8(s, &p->bg_pri); io_u8(s, &p->zoomy); io_u8(s, &p->zoomx);
            io_u32(s, &p->tnum); io_u16(s, &p->colr); io_u8(s, &p->dpth);
            uint16_t a = (uint16_t)p->alpha; io_u16(s, &a); p->alpha = (int16_t)a;
        }
        io_bytes(s, b->zoom_bitmap, sizeof(b->zoom_bitmap));
        io_u32a(s, b->screen, sizeof(b->screen) / 4);
    });
    SECTION("EEPR", io_eeprom(s, &b->eeprom));
    SECTION("YMF ", {
        io_i64(s, &b->ymf_timer[0]); io_i64(s, &b->ymf_timer[1]);
        io_u64(s, &b->audio_samples);
        io_u32(s, ymf_size);
        if (s->writing) io_bytes(s, *ymf_blob, *ymf_size);
        else if (!s->bad && s->at + *ymf_size <= s->n) { *ymf_blob = s->p + s->at; s->at += *ymf_size; }
        else s->bad = 1;
    });
    SECTION("MISC", {
        io_u32(s, &b->region);
        io_u8(s, &b->debug_dip);
        io_u64(s, &b->frames_run);
    });
}

uint8_t *db_state_save(const db_board *b, size_t *size)
{
    io s = { 1, NULL, 0, 0, 0, 0 };
    uint32_t version = DB_STATE_VERSION, fp = db_board_rom_fingerprint(b), entries = sh2_program_entry_count();
    uint32_t ymf_size = (uint32_t)db_ymf_save(b->ymf, NULL, 0);
    uint8_t *ymf_blob = (uint8_t *)malloc(ymf_size);
    if (!ymf_blob) return NULL;
    db_ymf_save(b->ymf, ymf_blob, ymf_size);
    io_bytes(&s, (void *)DB_STATE_MAGIC, 8);
    io_u32(&s, &version); io_u32(&s, &fp); io_u32(&s, &entries);
    io_board(&s, (db_board *)b, &ymf_blob, &ymf_size);       /* writing does not modify the board */
    free(ymf_blob);
    uint32_t crc = s.bad ? 0 : db_crc32(0, s.p, s.n);
    io_u32(&s, &crc);
    if (s.bad) { free(s.p); return NULL; }
    *size = s.n;
    return s.p;
}

static int fail(char *err, size_t n, const char *m) { if (err && n) snprintf(err, n, "%s", m); return 0; }

/* Parse into a scratch board so the real one is untouched on failure. */
static db_board *parse(const db_board *b, const uint8_t *data, size_t size, const uint8_t **ymf_blob, uint32_t *ymf_size,
                       char *err, size_t n)
{
    if (!data || size < 8 + 12 + 4) { fail(err, n, "state file is truncated"); return NULL; }
    if (memcmp(data, DB_STATE_MAGIC, 8) != 0) { fail(err, n, "not a Dragon Blaze save state"); return NULL; }
    uint32_t stored = (uint32_t)data[size - 4] | (uint32_t)data[size - 3] << 8 | (uint32_t)data[size - 2] << 16 | (uint32_t)data[size - 1] << 24;
    if (db_crc32(0, data, size - 4) != stored) { fail(err, n, "state is damaged (checksum mismatch)"); return NULL; }
    io s = { 0, (uint8_t *)data, size - 4, 0, 8, 0 };
    uint32_t version = 0, fp = 0, entries = 0;
    io_u32(&s, &version); io_u32(&s, &fp); io_u32(&s, &entries);
    if (version != DB_STATE_VERSION) {
        char m[96]; snprintf(m, sizeof(m), "state format version %u is not supported (expected %u)", version, DB_STATE_VERSION);
        fail(err, n, m); return NULL;
    }
    if (fp != db_board_rom_fingerprint(b)) { fail(err, n, "state was made with a different ROM set"); return NULL; }
    db_board *t = (db_board *)malloc(sizeof(*t));
    if (!t) { fail(err, n, "out of memory"); return NULL; }
    memcpy(t, b, sizeof(*t));
    uint8_t *blob = NULL;
    io_board(&s, t, &blob, ymf_size);
    if (s.bad || s.at != s.n) { free(t); fail(err, n, "state has an unexpected layout or is truncated"); return NULL; }
    if (*ymf_size != (uint32_t)db_ymf_save(b->ymf, NULL, 0)) { free(t); fail(err, n, "state has an unexpected sound chip layout"); return NULL; }
    *ymf_blob = blob;
    /* no check on the resume address: code the build did not translate runs
     * through the fallback interpreter, so states move freely between the
     * recompiled and the generic build of the same ROM set */
    return t;
}

int db_state_validate(const db_board *b, const uint8_t *data, size_t size, char *err, size_t n)
{
    const uint8_t *blob; uint32_t bs;
    db_board *t = parse(b, data, size, &blob, &bs, err, n);
    if (!t) return 0;
    free(t);
    return 1;
}

int db_state_load(db_board *b, const uint8_t *data, size_t size, char *err, size_t n)
{
    const uint8_t *blob; uint32_t bs;
    db_board *t = parse(b, data, size, &blob, &bs, err, n);
    if (!t) return 0;
    if (!db_ymf_load(b->ymf, blob, bs)) { free(t); return fail(err, n, "sound chip state could not be restored"); }
    memcpy(b, t, sizeof(*b));
    free(t);
    for (unsigned i = 0; i < 0x5000 / 4; i++) db_palette_write(b, i);
    b->inputs = 0;   /* inputs held when the state was made must not stay latched */
    b->ymf_now = -1;
    b->eeprom.dirty = 1;   /* the EEPROM contents came from the state */
    db_board_ram_code_recount(b);
    db_board_attach(b);
    return 1;
}
