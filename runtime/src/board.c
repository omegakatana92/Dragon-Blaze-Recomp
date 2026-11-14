/* Psikyo PS5 board (Dragon Blaze): ROM loading, memory map, inputs, EEPROM
 * port, SH7604 on-chip modules, interrupts and the frame scheduler.
 * Behaviour follows MAME 0.264 psikyo/psikyosh.cpp (ps5_map, psikyo5
 * machine config) and cpu/sh/sh7604.cpp. */
#include "db/board.h"
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

static db_board *B;   /* the board the SH-2 bus talks to */

/* ------------------------------------------------------------------ ROMs */

typedef struct rom_entry { const char *name; uint32_t size, crc; } rom_entry;

static const rom_entry FILES[] = {
    { "2prog_h.u21", 0x80000, 0xFC5EADE8 }, { "1prog_l.u22", 0x80000, 0x95D6FD02 },
    { "1l.u4", 0x200000, 0xC2EB565C }, { "1h.u12", 0x200000, 0x23CB46B7 },
    { "2l.u5", 0x200000, 0xBC256AEA }, { "2h.u13", 0x200000, 0xB75F59EC },
    { "3l.u6", 0x200000, 0x4284F008 }, { "3h.u14", 0x200000, 0xABE5CBBF },
    { "4l.u7", 0x200000, 0xC9FCF2E5 }, { "4h.u15", 0x200000, 0x0AB0A12A },
    { "5l.u8", 0x200000, 0x68D03CCF }, { "5h.u16", 0x200000, 0x5450FBCA },
    { "6l.u1", 0x200000, 0x8B52C90B }, { "6h.u2", 0x200000, 0x7362F929 },
    { "7l.u19", 0x200000, 0xB4F4D86E }, { "7h.u20", 0x200000, 0x44B7B9CC },
    { "8l.u28", 0x200000, 0xCD079F89 }, { "8h.u29", 0x200000, 0x3EDB508A },
    { "9l.u41", 0x200000, 0x0B53CD78 }, { "9h.u42", 0x200000, 0xBC61998A },
    { "10l.u58", 0x200000, 0xA3F5C7F8 }, { "10h.u59", 0x200000, 0x30E304C4 },
    { "snd0.u52", 0x200000, 0x7FD1B225 },
    { "eeprom-dragnblz.u44", 0x100, 0x46E85DA9 },
};
#define FILE_COUNT (sizeof(FILES) / sizeof(FILES[0]))

/* program ROM names of the other PS5 games, so a wrong set gets a clear message */
static const struct { const char *file, *game; } OTHER_SETS[] = {
    { "2-prog_l.u21", "Gunbarich (gnbarich)" }, { "2_prog_l.u21", "Gunbird 2 / Strikers 1945 III" },
    { "2b_prog_l.u21", "Gunbird 2 / Strikers 1945 III" }, { "5_prog_h.u21", "Gunbird 2 / Strikers 1945 III" },
    { "tgm2p_2prog_l.u21", "Tetris: The Grand Master 2" }, { "2.u21", "Tetris: The Grand Master 2" },
};

static int zip_find(mz_zip_archive *z, const char *name)
{
    int n = (int)mz_zip_reader_get_num_files(z);
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(z, (mz_uint)i, &st)) continue;
        const char *base = strrchr(st.m_filename, '/');
        base = base ? base + 1 : st.m_filename;
        if (!strcasecmp(base, name)) return i;
    }
    return -1;
}

static void *zip_extract(mz_zip_archive *z, const char *name, size_t *size)
{
    int i = zip_find(z, name);
    if (i < 0) return NULL;
    return mz_zip_reader_extract_to_heap(z, (mz_uint)i, size, 0);
}

static int open_zip(mz_zip_archive *z, const char *path)
{
    memset(z, 0, sizeof(*z));
    return path && *path && mz_zip_reader_init_file(z, path, 0);
}

int db_rom_check(const char *zip_path, char *err, size_t n)
{
    mz_zip_archive z;
    if (!open_zip(&z, zip_path)) {
        snprintf(err, n, "Cannot open \"%s\" as a zip archive.", zip_path ? zip_path : "");
        return 0;
    }
    char missing[512] = "", bad[512] = "";
    int found = 0;
    for (size_t i = 0; i < FILE_COUNT; i++) {
        int idx = zip_find(&z, FILES[i].name);
        mz_zip_archive_file_stat st;
        if (idx < 0 || !mz_zip_reader_file_stat(&z, (mz_uint)idx, &st)) {
            strncat(missing, " ", sizeof(missing) - strlen(missing) - 1);
            strncat(missing, FILES[i].name, sizeof(missing) - strlen(missing) - 1);
            continue;
        }
        found++;
        if (st.m_uncomp_size != FILES[i].size || st.m_crc32 != FILES[i].crc) {
            strncat(bad, " ", sizeof(bad) - strlen(bad) - 1);
            strncat(bad, FILES[i].name, sizeof(bad) - strlen(bad) - 1);
        }
    }
    if (found == 0) {
        for (size_t i = 0; i < sizeof(OTHER_SETS) / sizeof(OTHER_SETS[0]); i++) {
            if (zip_find(&z, OTHER_SETS[i].file) >= 0) {
                snprintf(err, n, "This looks like %s, not Dragon Blaze. Select the MAME 'dragnblz' set.", OTHER_SETS[i].game);
                mz_zip_reader_end(&z);
                return 0;
            }
        }
    }
    mz_zip_reader_end(&z);
    if (missing[0] || bad[0]) {
        snprintf(err, n, "Not the MAME 'dragnblz' set.%s%s%s%s", missing[0] ? " Missing:" : "", missing,
                 bad[0] ? " Wrong size/CRC:" : "", bad);
        return 0;
    }
    return 1;
}

static uint32_t crc_of(const uint8_t *p, size_t n) { return (uint32_t)mz_crc32(MZ_CRC32_INIT, p, n); }

static uint64_t ymf_host_now(void *user);
static void ymf_host_set_timer(void *user, int tnum, int64_t expire);
static void ymf_host_irq(void *user, int asserted);

int db_board_load(db_board *b, const char *zip_path)
{
    memset(b, 0, sizeof(*b));
    b->region = DB_REGION_INTL_B;
    if (!db_rom_check(zip_path, b->error, sizeof(b->error))) return 0;
    b->prog = (uint32_t *)calloc(1, DB_PROG_SIZE);
    b->gfx = (uint8_t *)calloc(1, DB_GFX_SIZE);
    b->ymf_rom = (uint8_t *)calloc(1, DB_YMF_SIZE);
    if (!b->prog || !b->gfx || !b->ymf_rom) {
        snprintf(b->error, sizeof(b->error), "out of memory");
        db_board_free(b);
        return 0;
    }
    mz_zip_archive z;
    open_zip(&z, zip_path);
    size_t sz = 0, sz2 = 0;
    /* ROM_LOAD32_WORD_SWAP: 2prog_h.u21 at 0, 1prog_l.u22 at 2 */
    uint8_t *hi = (uint8_t *)zip_extract(&z, "2prog_h.u21", &sz), *lo = (uint8_t *)zip_extract(&z, "1prog_l.u22", &sz2);
    if (!hi || !lo) {
        snprintf(b->error, sizeof(b->error), "Could not read the program ROMs.");
        mz_free(hi); mz_free(lo); mz_zip_reader_end(&z); db_board_free(b);
        return 0;
    }
    for (size_t k = 0; k < 0x40000; k++)
        b->prog[k] = (uint32_t)hi[2 * k + 1] << 24 | (uint32_t)hi[2 * k] << 16 | (uint32_t)lo[2 * k + 1] << 8 | lo[2 * k];
    mz_free(hi); mz_free(lo);
    /* gfx1: ROM_LOAD32_WORD pairs from 0x400000 ("Nl" at +0, "Nh" at +2) */
    for (int r = 1; r <= 10; r++) {
        char nl[16], nh[16];
        static const char *ul[] = { "", "u4", "u5", "u6", "u7", "u8", "u1", "u19", "u28", "u41", "u58" };
        static const char *uh[] = { "", "u12", "u13", "u14", "u15", "u16", "u2", "u20", "u29", "u42", "u59" };
        snprintf(nl, sizeof(nl), "%dl.%s", r, ul[r]);
        snprintf(nh, sizeof(nh), "%dh.%s", r, uh[r]);
        uint8_t *l = (uint8_t *)zip_extract(&z, nl, &sz), *h = (uint8_t *)zip_extract(&z, nh, &sz2);
        uint8_t *dst = b->gfx + 0x400000u * (unsigned)r;
        for (size_t k = 0; l && h && k < 0x100000; k++) {
            dst[4 * k + 0] = l[2 * k]; dst[4 * k + 1] = l[2 * k + 1];
            dst[4 * k + 2] = h[2 * k]; dst[4 * k + 3] = h[2 * k + 1];
        }
        mz_free(l); mz_free(h);
    }
    uint8_t *snd = (uint8_t *)zip_extract(&z, "snd0.u52", &sz);
    if (snd) memcpy(b->ymf_rom, snd, sz > 0x200000 ? 0x200000 : sz);
    mz_free(snd);
    /* ROM_LOAD16_WORD_SWAP into the 8-bit "eeprom" region */
    uint8_t *ee = (uint8_t *)zip_extract(&z, "eeprom-dragnblz.u44", &sz);
    for (size_t i = 0; ee && i < DB_EEPROM_SIZE; i++) b->eeprom_default[i] = ee[i ^ 1];
    mz_free(ee);
    mz_zip_reader_end(&z);
    memcpy(b->eeprom.data, b->eeprom_default, DB_EEPROM_SIZE);
    db_eeprom_reset(&b->eeprom);   /* power-on state of the serial interface */

    db_ymf_host host = { b, ymf_host_now, ymf_host_set_timer, ymf_host_irq };
    b->ymf = db_ymf_create(b->ymf_rom, DB_YMF_SIZE, &host);
    if (!b->ymf) {
        snprintf(b->error, sizeof(b->error), "out of memory");
        db_board_free(b);
        return 0;
    }
    uint32_t base = 0, rsize = 0, src = 0;
    sh2_program_ram_code(&base, &rsize, &src);
    if (base == 0x06000000u && rsize <= 0x100000u && src + rsize <= 0x100000u) { b->ram_code_size = rsize; b->ram_code_src = src; }
    db_board_reset(b);
    return 1;
}

uint32_t db_board_rom_fingerprint(const db_board *b)
{
    return b->prog ? crc_of((const uint8_t *)b->prog, 0x100000) : 0;
}

void db_board_free(db_board *b)
{
    if (b->ymf) db_ymf_destroy(b->ymf);
    free(b->prog); free(b->gfx); free(b->ymf_rom);
    b->ymf = NULL; b->prog = NULL; b->gfx = NULL; b->ymf_rom = NULL;
}

void db_board_attach(db_board *b)
{
    B = b;
    sh2_ram_code_base = 0x06000000u;
    sh2_ram_code_size = b->ram_code_size;
    sh2_ram_code_dirty = b->ram_code_dirty;
}

/* recount the RAM words that differ from the ROM bytes the translated RAM
 * code was made from */
void db_board_ram_code_recount(db_board *b)
{
    memset(b->ram_code_dirty, 0, sizeof(b->ram_code_dirty));
    for (uint32_t off = 0; off < b->ram_code_size; off += 4)
        if (b->ram[off >> 2] != b->prog[(b->ram_code_src + off) >> 2]) b->ram_code_dirty[off >> 12]++;
}
void db_board_set_inputs(db_board *b, uint32_t m) { b->inputs = m; }

/* ---------------------------------------------------------------- YMF278B */

static uint64_t ymf_host_now(void *user)
{
    db_board *b = (db_board *)user;
    return b->ymf_now >= 0 ? (uint64_t)b->ymf_now : b->cpu.cycles;
}

static void ymf_host_set_timer(void *user, int tnum, int64_t expire)
{
    ((db_board *)user)->ymf_timer[tnum & 1] = expire;
}

/* ymf.irq_handler().set_inputline(m_maincpu, 12) */
static void ymf_host_irq(void *user, int asserted)
{
    db_board *b = (db_board *)user;
    sh2_set_irq_line(&b->cpu, 12, asserted);
}

/* Stream position: MAME's sound_stream::update() generates every sample up
 * to the current time, indexing samples within each second:
 * index = ceil(attoseconds / attoseconds_per_sample). */
#define ATTOS_PER_SECOND 1000000000000000000ull
#define ATTOS_PER_SAMPLE (ATTOS_PER_SECOND / DB_AUDIO_RATE)

static uint64_t samples_due(uint64_t cycles)
{
    unsigned __int128 t = (unsigned __int128)cycles * (ATTOS_PER_SECOND / DB_CPU_CLOCK);
    uint64_t secs = (uint64_t)(t / ATTOS_PER_SECOND), attos = (uint64_t)(t % ATTOS_PER_SECOND);
    return secs * DB_AUDIO_RATE + (attos + ATTOS_PER_SAMPLE - 1) / ATTOS_PER_SAMPLE;
}

static int32_t frame_audio[DB_MAX_FRAME_SAMPLES * 2];
static int frame_audio_count;

static void sync_audio(db_board *b, uint64_t cycles)
{
    uint64_t due = samples_due(cycles);
    while (b->audio_samples < due) {
        uint64_t n64 = due - b->audio_samples;
        int room = (int)(sizeof(frame_audio) / sizeof(frame_audio[0])) - frame_audio_count;
        int n = n64 > 256 ? 256 : (int)n64;
        if (n > room) {
            /* frame buffer full (only if a frame runs far too long): drop the oldest half */
            int keep = frame_audio_count / 2;
            memmove(frame_audio, frame_audio + frame_audio_count - keep, (size_t)keep * sizeof(int32_t));
            frame_audio_count = keep;
            continue;
        }
        db_ymf_generate(b->ymf, frame_audio + frame_audio_count, n);
        frame_audio_count += n;
        b->audio_samples += (uint64_t)n;
    }
}

static void io_trace(db_board *b, char kind, uint32_t a, uint32_t v);

static uint8_t ymf_r(db_board *b, unsigned offset)
{
    sync_audio(b, b->cpu.cycles);
    uint8_t v = db_ymf_read(b->ymf, offset);
    io_trace(b, 'r', 0x03100000u + offset, v);
    return v;
}

static void ymf_w(db_board *b, unsigned offset, uint8_t data)
{
    sync_audio(b, b->cpu.cycles);
    io_trace(b, 'w', 0x03100000u + offset, data);
    db_ymf_write(b->ymf, offset, data);
}

/* ----------------------------------------------------------- SH7604 regs */

/* ---- FRT: MAME sh7604_device sh2_timer_resync/activate/callback ---- */
enum { FRT_ICF = 0x80, FRT_OCFA = 0x08, FRT_OCFB = 0x04, FRT_OVF = 0x02, FRT_CCLRA = 0x01 };
static const int frt_div[4] = { 3, 5, 7, 0 };

static void frt_resync(db_board *b)
{
    db_onchip *o = &b->onchip;
    int divider = frt_div[o->frc_tcr & 3];
    uint64_t add = (b->cpu.cycles - o->frc_base) >> divider;
    if (add > 0) {
        if (divider) o->frc = (uint16_t)(o->frc + add);
        o->frc_base = b->cpu.cycles;
    }
}

static void frt_activate(db_board *b)
{
    db_onchip *o = &b->onchip;
    int max_delta = 0xfffff;
    o->frt_event = UINT64_MAX;
    uint16_t frc = o->frc;
    if (!(o->ftcsr & FRT_OCFA)) { uint16_t d = (uint16_t)(o->ocra - frc); if (d < max_delta) max_delta = d; }
    if (!(o->ftcsr & FRT_OCFB) && (o->ocra <= o->ocrb || !(o->ftcsr & FRT_CCLRA))) {
        uint16_t d = (uint16_t)(o->ocrb - frc); if (d < max_delta) max_delta = d;
    }
    if (!(o->ftcsr & FRT_OVF) && !(o->ftcsr & FRT_CCLRA)) { int d = 0x10000 - frc; if (d < max_delta) max_delta = d; }
    if (max_delta != 0xfffff) {
        int divider = frt_div[o->frc_tcr & 3];
        if (divider) {
            o->frc_base = b->cpu.cycles;
            o->frt_event = b->cpu.cycles + ((uint64_t)max_delta << divider);
        }
    }
}

static void onchip_recalc_irq(db_board *b)
{
    db_onchip *o = &b->onchip;
    int irq = 0, vector = -1;
    if (o->tier & o->ftcsr & (FRT_ICF | FRT_OCFA | FRT_OCFB | FRT_OVF)) {
        int level = (o->regs[0x60] & 0x0F);           /* IPRB bits 8-11 */
        if (level > irq) {
            int mask = o->tier & o->ftcsr;
            irq = level;
            if (mask & FRT_ICF) vector = o->regs[0x66] & 0x7f;
            else if (mask & (FRT_OCFA | FRT_OCFB)) vector = o->regs[0x67] & 0x7f;
            else vector = o->regs[0x66] & 0x7f;
        }
    }
    b->cpu.internal_irq_level = irq;
    b->cpu.internal_irq_vector = (uint32_t)vector;
    b->cpu.test_irq = 1;
}

static void frt_callback(db_board *b)
{
    db_onchip *o = &b->onchip;
    frt_resync(b);
    uint16_t frc = o->frc;
    if (frc == o->ocrb) o->ftcsr |= FRT_OCFB;
    if (frc == 0x0000) o->ftcsr |= FRT_OVF;
    if (frc == o->ocra) {
        o->ftcsr |= FRT_OCFA;
        if (o->ftcsr & FRT_CCLRA) o->frc = 0;
    }
    onchip_recalc_irq(b);
    frt_activate(b);
}

static uint8_t frt_r8(db_board *b, unsigned off)
{
    db_onchip *o = &b->onchip;
    switch (off) {
    case 0x10: return o->tier;
    case 0x11: return o->ftcsr;
    case 0x12: frt_resync(b); return (uint8_t)(o->frc >> 8);
    case 0x13: frt_resync(b); return (uint8_t)o->frc;
    case 0x14: return (uint8_t)(((o->tocr & 0x10) ? o->ocrb : o->ocra) >> 8);
    case 0x15: return (uint8_t)((o->tocr & 0x10) ? o->ocrb : o->ocra);
    case 0x16: return o->frc_tcr & 0x83;
    case 0x17: return (uint8_t)((o->tocr & 0x13) | 0xe0);
    default: return 0;
    }
}

static void frt_w8(db_board *b, unsigned off, uint8_t data)
{
    db_onchip *o = &b->onchip;
    frt_resync(b);
    switch (off) {
    case 0x10: o->tier = data; break;
    case 0x11: {
        uint8_t old = o->ftcsr, f = FRT_ICF | FRT_OCFA | FRT_OCFB | FRT_OVF;
        o->ftcsr = (uint8_t)((data & ~f) | (old & data & f));
        break;
    }
    case 0x12: o->frc = (uint16_t)((o->frc & 0x00ff) | data << 8); break;
    case 0x13: o->frc = (uint16_t)((o->frc & 0xff00) | data); break;
    case 0x14: case 0x15: {
        uint16_t *r = (o->tocr & 0x10) ? &o->ocrb : &o->ocra;
        *r = off == 0x14 ? (uint16_t)((*r & 0x00ff) | data << 8) : (uint16_t)((*r & 0xff00) | data);
        break;
    }
    case 0x16: o->frc_tcr = data & 0x83; break;
    case 0x17: o->tocr = data & 0x13; break;
    default: return;
    }
    frt_activate(b);
    onchip_recalc_irq(b);
}

static void divu32(db_onchip *o)
{
    int32_t a = (int32_t)o->dvdntl, d = (int32_t)o->dvsr;
    if (d == 0 || (a == INT32_MIN && d == -1)) {
        o->divu_ovf = 1;
        o->dvdntl = 0x7fffffff; o->dvdnth = 0x7fffffff;
        return;
    }
    o->dvdntl = (uint32_t)(a / d);
    o->dvdnth = (uint32_t)(a % d);
}

static void divu64(db_onchip *o)
{
    int64_t a = (int64_t)((uint64_t)o->dvdntl | ((uint64_t)o->dvdnth << 32));
    int64_t d = (int32_t)o->dvsr;
    if (d == 0 || (a == INT64_MIN && d == -1)) { o->divu_ovf = 1; o->dvdntl = o->dvdnth = 0x7fffffff; return; }
    int64_t q = a / d;
    if (q != (int32_t)q) { o->divu_ovf = 1; o->dvdntl = o->dvdnth = 0x7fffffff; return; }
    o->dvdntl = (uint32_t)q;
    o->dvdnth = (uint32_t)(a % d);
}

static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int trace_onchip = -1;
static FILE *trace_io;
static int trace_io_init;
static uint64_t trace_from, trace_to;

static void io_trace(db_board *b, char kind, uint32_t a, uint32_t v)
{
    if (!trace_io_init) {
        trace_io_init = 1;
        const char *p = getenv("DB_TRACE_IO");
        if (p) {
            trace_io = fopen(p, "w");
            const char *f = getenv("DB_TRACE_FRAMES");
            trace_from = 0; trace_to = UINT64_MAX;
            if (f) sscanf(f, "%llu-%llu", (unsigned long long *)&trace_from, (unsigned long long *)&trace_to);
        }
    }
    if (!trace_io || b->frames_run + 1 < trace_from || b->frames_run + 1 > trace_to) return;
    fprintf(trace_io, "%llu %c %08X %08X %llu\n", (unsigned long long)(b->frames_run + 1), kind, a, v, (unsigned long long)b->cpu.cycles);
}

static uint32_t onchip_r32(db_board *b, uint32_t a)
{
    db_onchip *o = &b->onchip;
    unsigned off = a & 0x1FC;
    switch (off) {
    case 0x100: return o->dvsr;
    case 0x104: return o->dvdntl;
    case 0x108: return (o->divu_ovfie ? 2u : 0u) | (o->divu_ovf ? 1u : 0u);
    case 0x110: case 0x118: return o->dvdnth;
    case 0x114: case 0x11C: return o->dvdntl;
    case 0x010: case 0x014: case 0x018:
        return (uint32_t)frt_r8(b, off) << 24 | (uint32_t)frt_r8(b, off + 1) << 16 | (uint32_t)frt_r8(b, off + 2) << 8 | frt_r8(b, off + 3);
    default: return be32(&o->regs[off]);
    }
}

static void onchip_after_write(db_board *b, unsigned off)
{
    db_onchip *o = &b->onchip;
    unsigned w = off & 0x1FC;
    uint32_t v = be32(&o->regs[w]);
    switch (w) {
    case 0x100: o->dvsr = v; break;
    case 0x104: o->dvdntl = v; divu32(o); break;
    case 0x108: if (v & 1) o->divu_ovf = 0; if (v & 2) o->divu_ovfie = 1; break;
    case 0x110: o->dvdnth = v; break;
    case 0x114: o->dvdntl = v; divu64(o); break;
    case 0x060: case 0x064: case 0x068: onchip_recalc_irq(b); break;
    default: break;
    }
}

static void onchip_w(db_board *b, uint32_t a, uint32_t v, unsigned size)
{
    unsigned off = a & 0x1FF;
    if (trace_onchip < 0) trace_onchip = getenv("DB_TRACE_ONCHIP") != NULL;
    if (trace_onchip)
        fprintf(stderr, "onchip w%u %08X = %08X pc=%08X\n", size * 8, a, v, b->cpu.pc);
    if (off >= 0x10 && off < 0x18) {
        unsigned n = size, base = size == 4 ? (off & 0x1FC) : size == 2 ? (off & 0x1FE) : off;
        for (unsigned i = 0; i < n; i++) frt_w8(b, base + i, (uint8_t)(v >> (8 * (n - 1 - i))));
        return;
    }
    if (size == 4) put32(&b->onchip.regs[off & 0x1FC], v);
    else if (size == 2) { b->onchip.regs[off & 0x1FE] = (uint8_t)(v >> 8); b->onchip.regs[(off & 0x1FE) + 1] = (uint8_t)v; }
    else b->onchip.regs[off] = (uint8_t)v;
    onchip_after_write(b, off);
}

static uint8_t onchip_r8(db_board *b, uint32_t a)
{
    unsigned off = a & 0x1FF;
    if (off >= 0x10 && off < 0x18) return frt_r8(b, off);
    if (off >= 0x100) return (uint8_t)(onchip_r32(b, a & ~3u) >> (8 * (3 - (a & 3))));
    return b->onchip.regs[off];
}

/* ------------------------------------------------------------------- I/O */

/* INPUT_PORTS_START(common): everything active low */
static uint32_t port_inputs(db_board *b)
{
    static const struct { uint32_t in, bit; } map[] = {
        { DB_IN_COIN1, 0x01 }, { DB_IN_COIN2, 0x02 }, { DB_IN_SERVICE1, 0x10 }, { DB_IN_TEST, 0x20 },
        { DB_IN_P2_START, 0x10000 }, { DB_IN_P2_B3, 0x20000 }, { DB_IN_P2_B2, 0x40000 }, { DB_IN_P2_B1, 0x80000 },
        { DB_IN_P2_LEFT, 0x100000 }, { DB_IN_P2_RIGHT, 0x200000 }, { DB_IN_P2_DOWN, 0x400000 }, { DB_IN_P2_UP, 0x800000 },
        { DB_IN_P1_START, 0x1000000 }, { DB_IN_P1_B3, 0x2000000 }, { DB_IN_P1_B2, 0x4000000 }, { DB_IN_P1_B1, 0x8000000 },
        { DB_IN_P1_LEFT, 0x10000000 }, { DB_IN_P1_RIGHT, 0x20000000 }, { DB_IN_P1_DOWN, 0x40000000 }, { DB_IN_P1_UP, 0x80000000u },
    };
    uint32_t v = 0xFFFFFFFFu, in = b->inputs;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (in & map[i].in) v &= ~map[i].bit;
    if (b->debug_dip) v &= ~(uint32_t)DB_DIP_DEBUG;
    return v;
}

/* JP4: region jumpers plus the EEPROM DO line (bit 28) */
static uint32_t port_jp4(db_board *b)
{
    return (b->region & 0x03000000u) | (db_eeprom_do_read(&b->eeprom, b->cpu.cycles) ? 0x10000000u : 0);
}

/* eeprom_w: DI bit 5, CS bit 7, CLK bit 6 */
static void eeprom_w(db_board *b, uint8_t data)
{
    db_eeprom_write_lines(&b->eeprom, b->cpu.cycles, (data >> 5) & 1, (data >> 7) & 1, (data >> 6) & 1);
}

/* irqctrl_w: the VBLANK handler writes 0x00 on entry and 0xc0 on exit */
static void irqctrl_w(db_board *b, uint32_t data)
{
    if (!(data & 0x00c00000u)) sh2_set_irq_line(&b->cpu, 4, 0);
}

/* --------------------------------------------------------------- the bus */

#define AM 0xC7FFFFFFu

static inline uint32_t canon(uint32_t a)
{
    uint32_t top = a & 0xF0000000u;
    return (top == 0 || top == 0x20000000u) ? (a & AM) : a;
}

static inline int is_ymf(uint32_t a) { return a >= 0x03100000u && a < 0x03100008u; }

/* 32-bit bus word containing a (a aligned to 4); YMF278B handled byte-wise */
static uint32_t read_word32(db_board *b, uint32_t a)
{
    if (a >= 0x06000000u && a < 0x06100000u) return b->ram[(a - 0x06000000u) >> 2];
    if (a < 0x00100000u) return b->prog[a >> 2];
    if (a >= 0x04000000u && a < 0x04010000u) return b->spriteram[(a - 0x04000000u) >> 2];
    if (a >= 0x04040000u && a < 0x04045000u) return b->paletteram[(a - 0x04040000u) >> 2];
    if (a >= 0x04050000u && a < 0x04050200u) return b->zoomram[(a - 0x04050000u) >> 2];
    if (a >= 0x0405ffe0u && a < 0x04060000u) return b->vidregs[(a - 0x0405ffe0u) >> 2];
    if (a >= 0x04060000u && a < 0x04080000u) {
        /* gfxbank: a bank of the gfx1 byte region read natively (MAME on a
         * little-endian host) */
        uint32_t off = ((b->vidregs[4] & 0xfff) * 0x20000u + (a - 0x04060000u)) % DB_GFX_SIZE;
        const uint8_t *p = b->gfx + off;
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    if (a >= 0x05000000u && a < 0x05080000u) return b->prog[(0x100000u + (a - 0x05000000u)) >> 2];
    if (a == 0x03000000u) { uint32_t v = port_inputs(b); io_trace(b, 'r', a, v); return v; }
    if (a == 0x03000004u) { uint32_t v = port_jp4(b); io_trace(b, 'r', a, v); return v; }
    if (a >= 0xFFFFFE00u) return onchip_r32(b, a);
    return 0;
}

static uint8_t read8_mmio(db_board *b, uint32_t a)
{
    if (is_ymf(a)) return ymf_r(b, a - 0x03100000u);
    return onchip_r8(b, a);
}

uint32_t sh2_rd32(uint32_t a)
{
    a = canon(a) & ~3u;
    if (is_ymf(a)) {
        uint32_t v = 0;
        for (unsigned i = 0; i < 4; i++) v = v << 8 | ymf_r(B, a + i - 0x03100000u);
        return v;
    }
    return read_word32(B, a);
}

uint16_t sh2_rd16(uint32_t a)
{
    a = canon(a) & ~1u;
    if (is_ymf(a) || (a >= 0xFFFFFE00u && a < 0xFFFFFF00u))
        return (uint16_t)(read8_mmio(B, a) << 8 | read8_mmio(B, a + 1));
    uint32_t w = read_word32(B, a & ~3u);
    return (uint16_t)((a & 2) ? w : w >> 16);
}

uint8_t sh2_rd8(uint32_t a)
{
    a = canon(a);
    if (is_ymf(a) || (a >= 0xFFFFFE00u && a < 0xFFFFFF00u)) return read8_mmio(B, a);
    uint32_t w = read_word32(B, a & ~3u);
    return (uint8_t)(w >> (8 * (3 - (a & 3))));
}

static inline void merge(uint32_t *word, uint32_t a, uint32_t v, unsigned size)
{
    if (size == 4) { *word = v; return; }
    unsigned shift = size == 2 ? ((a & 2) ? 0 : 16) : 8 * (3 - (a & 3));
    uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
    *word = (*word & ~mask) | ((v << shift) & mask);
}

/* the data as a 32-bit handler sees it (shifted into its lane, others 0) */
static inline uint32_t lane(uint32_t a, uint32_t v, unsigned size)
{
    if (size == 4) return v;
    if (size == 2) return (a & 2) ? (v & 0xFFFFu) : (v & 0xFFFFu) << 16;
    return (v & 0xFFu) << (8 * (3 - (a & 3)));
}

static void bus_write(db_board *b, uint32_t a, uint32_t v, unsigned size)
{
    a = canon(a);
    if (a >= 0x06000000u && a < 0x06100000u) {
        uint32_t off = a - 0x06000000u, *w = &b->ram[off >> 2];
        if (off < b->ram_code_size) {
            uint32_t expect = b->prog[(b->ram_code_src + (off & ~3u)) >> 2];
            int before = *w != expect;
            merge(w, a, v, size);
            int after = *w != expect;
            b->ram_code_dirty[off >> 12] += (uint32_t)(after - before);
        } else {
            merge(w, a, v, size);
        }
        return;
    }
    if (a >= 0x04000000u && a < 0x04010000u) { merge(&b->spriteram[(a - 0x04000000u) >> 2], a, v, size); return; }
    if (a >= 0x04040000u && a < 0x04045000u) {
        unsigned i = (a - 0x04040000u) >> 2;
        merge(&b->paletteram[i], a, v, size);
        db_palette_write(b, i);
        return;
    }
    if (a >= 0x04050000u && a < 0x04050200u) { merge(&b->zoomram[(a - 0x04050000u) >> 2], a, v, size); return; }
    if (a >= 0x0405ffdcu && a < 0x0405ffe0u) { io_trace(b, 'w', 0x0405ffdcu, lane(a, v, size)); irqctrl_w(b, lane(a, v, size)); return; }
    if (a >= 0x0405ffe0u && a < 0x04060000u) { merge(&b->vidregs[(a - 0x0405ffe0u) >> 2], a, v, size); return; }
    if (is_ymf(a)) {
        uint32_t d = lane(a, v, size);
        uint32_t base = a & ~3u;
        for (unsigned i = 0; i < 4; i++) {
            uint32_t ba = base + i;
            if (ba < a || ba >= a + size || !is_ymf(ba)) continue;
            ymf_w(b, ba - 0x03100000u, (uint8_t)(d >> (8 * (3 - i))));
        }
        return;
    }
    if (a >= 0x03000004u && a < 0x03000008u) {
        /* map(0x03000004, 0x03000004).w(eeprom_w): the byte lane at 0x03000004 */
        if (a == 0x03000004u) eeprom_w(b, (uint8_t)(lane(a, v, size) >> 24));
        return;
    }
    if (a >= 0xFFFFFE00u) { onchip_w(b, a, v, size); return; }
    /* ROM, input port and unmapped writes are ignored */
}

void sh2_wr32(uint32_t a, uint32_t d) { bus_write(B, a & ~3u, d, 4); }
void sh2_wr16(uint32_t a, uint16_t d) { bus_write(B, a & ~1u, d, 2); }
void sh2_wr8(uint32_t a, uint8_t d) { bus_write(B, a, d, 1); }

/* ------------------------------------------------------------- scheduler */

void db_board_reset(db_board *b)
{
    db_board_attach(b);
    memset(b->ram, 0, sizeof(b->ram));
    db_board_ram_code_recount(b);
    memset(b->spriteram, 0, sizeof(b->spriteram));
    memset(b->paletteram, 0, sizeof(b->paletteram));
    memset(b->zoomram, 0, sizeof(b->zoomram));
    memset(b->vidregs, 0, sizeof(b->vidregs));
    memset(b->pens, 0, sizeof(b->pens));
    for (unsigned i = 0; i < 0x5000 / 4; i++) db_palette_write(b, i);
    memset(&b->onchip, 0, sizeof(b->onchip));
    b->onchip.frt_event = UINT64_MAX;
    db_video_reset(b);
    b->eeprom.completion_time = 0;       /* eeprom_base_device::device_reset */
    b->ymf_timer[0] = b->ymf_timer[1] = -1;
    b->ymf_now = -1;
    b->cpu.held_lines = (1u << 4) | (1u << 12);   /* ASSERT_LINE / CLEAR_LINE driven lines */
    sh2_reset(&b->cpu);
    db_ymf_reset(b->ymf);
    b->audio_samples = 0;
    b->frames_run = 0;
}

/* VBLANK n starts at n frame periods (the screen starts in VBLANK at time 0).
 * MAME keeps time in attoseconds: the frame period is 10^18 / 60 and a CPU
 * cycle 10^18 / clock (both truncated), and the CPU stops at the first
 * instruction boundary at or after floor(time / cycle). */
#define ATTOS_PER_FRAME 16666666666666666ull
#define ATTOS_PER_CYCLE (1000000000000000000ull / DB_CPU_CLOCK)

static uint64_t vblank_cycle(uint64_t n)
{
    unsigned __int128 t = (unsigned __int128)n * ATTOS_PER_FRAME;
    return (uint64_t)(t / ATTOS_PER_CYCLE);
}

int db_board_run_frame(db_board *b, int16_t *mono, int *frames)
{
    db_board_attach(b);
    int status = DB_RUN_OK;
    frame_audio_count = 0;
    uint64_t target = vblank_cycle(b->frames_run + 1);
    while (status == DB_RUN_OK && b->cpu.cycles < target) {
        uint64_t next = target;
        if (b->onchip.frt_event < next) next = b->onchip.frt_event;
        for (int t = 0; t < 2; t++)
            if (b->ymf_timer[t] >= 0 && (uint64_t)b->ymf_timer[t] < next) next = (uint64_t)b->ymf_timer[t];
        if (b->cpu.cycles < next && sh2_run_until(&b->cpu, next) != SH2_RUN_OK) { status = DB_RUN_MISSING; break; }
        for (int t = 0; t < 2; t++) {
            if (b->ymf_timer[t] >= 0 && b->cpu.cycles >= (uint64_t)b->ymf_timer[t]) {
                b->ymf_now = b->ymf_timer[t];
                b->ymf_timer[t] = -1;
                db_ymf_timer_expired(b->ymf, t);
                b->ymf_now = -1;
            }
        }
        if (b->cpu.cycles >= b->onchip.frt_event) frt_callback(b);
    }
    if (status == DB_RUN_OK) {
        /* screen_device::vblank_begin: frame_update (screen_update), the CPU's
         * VBLANK interrupt, then screen_vblank (sprite list latch) */
        sync_audio(b, target);
        db_video_update(b);
        sh2_set_irq_line(&b->cpu, 4, 1);
        db_video_get_sprites(b);
        b->frames_run++;
    }
    for (int i = 0; i < frame_audio_count; i++) {
        int32_t s = frame_audio[i];
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        if (mono && i < DB_MAX_FRAME_SAMPLES) mono[i] = (int16_t)s;
    }
    if (frames) *frames = frame_audio_count < DB_MAX_FRAME_SAMPLES ? frame_audio_count : DB_MAX_FRAME_SAMPLES;
    return status;
}
