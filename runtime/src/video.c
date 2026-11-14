/* Psikyo PS6406B video (PS5 board), ported from MAME 0.264
 * src/mame/psikyo/psikyosh_v.cpp (David Haywood, Paul Priest; BSD-3-Clause)
 * and the parts of src/emu/drawgfx.cpp it relies on (gfx_element::transpen,
 * alpha, alphatable, alphastore).  The picture is composed exactly as MAME
 * does, including its ARGB intermediate values, so frames can be compared
 * pixel for pixel. */
#include "db/board.h"

#include <string.h>

#define BG_TRANSPEN 0x00ff00ffu
#define CLIP_L 0
#define CLIP_R (DB_SCREEN_W - 1)
#define CLIP_T 0
#define CLIP_B (DB_SCREEN_H - 1)

#define GFX4_ELEMENTS (DB_GFX_SIZE / 128)     /* gfx_16x16x4_packed_msb */
#define GFX8_ELEMENTS (DB_GFX_SIZE / 256)     /* gfx_16x16x8_raw */

typedef struct rect { int l, r, t, b; } rect;

static inline uint8_t pal6bit(uint8_t bits) { bits &= 0x3f; return (uint8_t)((bits << 2) | (bits >> 4)); }

/* drawgfx.h alpha_blend_r32 */
static inline uint32_t alpha_blend_r32(uint32_t d, uint32_t s, uint8_t level)
{
    return ((((s & 0x0000ff) * level + (d & 0x0000ff) * (uint32_t)(256 - level)) >> 8)) |
           ((((s & 0x00ff00) * level + (d & 0x00ff00) * (uint32_t)(256 - level)) >> 8) & 0x00ff00) |
           ((((s & 0xff0000) * level + (d & 0xff0000) * (uint32_t)(256 - level)) >> 8) & 0xff0000);
}

static uint8_t alphatable[256];
static uint16_t bg_zoom[256];
static int tables_ready;

static void init_tables(void)
{
    if (tables_ready) return;
    /* pens 0xc0-0xff have a gradient of alpha values associated with them */
    for (int i = 0; i < 0xc0; i++) alphatable[i] = 0xff;
    for (int i = 0; i < 0x40; i++) alphatable[i + 0xc0] = pal6bit((uint8_t)(0x3f - i));
    /* background zoom table, .10 fixed point */
    for (int i = 0; i < 0x100; i++) bg_zoom[i] = (uint16_t)((64 * 0x400) / (i + 64));
    tables_ready = 1;
}

void db_palette_write(db_board *b, unsigned index)
{
    /* palette_device::write32, RGBx_888 */
    uint32_t v = b->paletteram[index];
    b->pens[index] = 0xff000000u | ((v >> 24) & 0xff) << 16 | ((v >> 16) & 0xff) << 8 | ((v >> 8) & 0xff);
}

void db_video_reset(db_board *b)
{
    init_tables();
    b->sprite_count = 0;
    memset(b->zoom_bitmap, 0, sizeof(b->zoom_bitmap));
    memset(b->z_bitmap, 0, sizeof(b->z_bitmap));
    memset(b->bg_bitmap, 0, sizeof(b->bg_bitmap));
    memset(b->bitmap, 0, sizeof(b->bitmap));
    memset(b->screen, 0, sizeof(b->screen));
}

/* --- gfx elements: decoded 16x16 tile, 16 bytes per row ---------------- */

static const uint8_t *tile_data(const db_board *b, int depth8, uint32_t code, uint8_t *buf)
{
    if (depth8) return b->gfx + (size_t)(code % GFX8_ELEMENTS) * 256u;
    const uint8_t *src = b->gfx + (size_t)(code % GFX4_ELEMENTS) * 128u;
    for (int i = 0; i < 128; i++) { buf[2 * i] = (uint8_t)(src[i] >> 4); buf[2 * i + 1] = (uint8_t)(src[i] & 15); }
    return buf;
}

static inline const uint32_t *palbase(const db_board *b, uint32_t color)
{
    return &b->pens[16u * (color % 0x100u)];   /* colorbase 0, granularity 16, 0x100 colours */
}

/* drawgfx_core without flipping: calls op for every pixel inside clip */
enum { OP_TRANSPEN, OP_ALPHA, OP_ALPHATABLE, OP_ALPHATABLESTORE };

static void drawgfx(uint32_t *dest, int pitch, rect clip, const uint8_t *src, const uint32_t *pal,
                    int destx, int desty, int op, uint8_t alpha)
{
    int srcx = 0, srcy = 0, ex = destx + 15, ey = desty + 15;
    if (destx < clip.l) { srcx += clip.l - destx; destx = clip.l; }
    if (ex > clip.r) ex = clip.r;
    if (destx > ex) return;
    if (desty < clip.t) { srcy += clip.t - desty; desty = clip.t; }
    if (ey > clip.b) ey = clip.b;
    if (desty > ey) return;
    for (int y = desty; y <= ey; y++, srcy++) {
        const uint8_t *s = src + srcy * 16 + srcx;
        uint32_t *d = dest + (size_t)y * pitch + destx;
        for (int x = destx; x <= ex; x++, s++, d++) {
            uint8_t p = *s;
            if (!p) continue;
            switch (op) {
            case OP_TRANSPEN: *d = pal[p]; break;
            case OP_ALPHA: *d = alpha_blend_r32(*d, pal[p], alpha); break;
            case OP_ALPHATABLE: *d = alpha_blend_r32(*d, pal[p], alphatable[p]); break;
            case OP_ALPHATABLESTORE: *d = (pal[p] & 0x00ffffffu) | (uint32_t)alphatable[p] << 24; break;
            }
        }
    }
}

/* gfx_element::alphatable (fixed alpha >= 0 goes through alpha(), which
 * turns 0xff into transpen) */
static void gfx_alphatable(db_board *b, int depth8, uint32_t code, uint32_t color, int x, int y, int alpha)
{
    uint8_t buf[256];
    const uint8_t *src = tile_data(b, depth8, code, buf);
    rect clip = { CLIP_L, CLIP_R, CLIP_T, CLIP_B };
    if (alpha >= 0)
        drawgfx(b->bitmap, DB_BITMAP_W, clip, src, palbase(b, color), x, y, alpha == 0xff ? OP_TRANSPEN : OP_ALPHA, (uint8_t)alpha);
    else
        drawgfx(b->bitmap, DB_BITMAP_W, clip, src, palbase(b, color), x, y, OP_ALPHATABLE, 0);
}

/* gfx_element::alphastore into the 512x512 bg bitmap (fixed alpha is
 * always 0xff here, i.e. transpen) */
static void gfx_alphastore(db_board *b, int depth8, uint32_t code, uint32_t color, int x, int y, int alpha)
{
    uint8_t buf[256];
    const uint8_t *src = tile_data(b, depth8, code, buf);
    rect clip = { 0, 511, 0, 511 };
    drawgfx(b->bg_bitmap, 512, clip, src, palbase(b, color), x, y, alpha == 0xff ? OP_TRANSPEN : OP_ALPHATABLESTORE, 0);
}

/* --- register helpers (psikyosh.h) -------------------------------------- */

#define VR(n) (b->vidregs[n])
static inline int BG_LARGE(const db_board *b, int n) { return ((VR(7) << (4 * n)) & 0x00001000u) != 0; }
static inline int BG_DEPTH_8BPP(const db_board *b, int n) { return ((VR(7) << (4 * n)) & 0x00004000u) != 0; }
static inline int BG_LAYER_ENABLE(const db_board *b, int n) { return ((VR(7) << (4 * n)) & 0x00008000u) != 0; }
static inline uint8_t BG_TYPE(const db_board *b, int n) { return (uint8_t)(((VR(6) << (8 * n)) & 0x7f000000u) >> 24); }
static inline int BG_LINE(const db_board *b, int n) { return ((VR(6) << (8 * n)) & 0x80000000u) != 0; }
static inline uint8_t SPRITE_PRI(const db_board *b, int n) { return (uint8_t)(((VR(2) << (4 * n)) & 0xf0000000u) >> 28); }

/* sprite RAM word; MAME reads past the end for out-of-range banks (undefined) */
static inline uint32_t sr_at(const db_board *b, unsigned index) { return index < 0x4000 ? b->spriteram[index] : 0; }

/* --- backgrounds ---------------------------------------------------------- */

static void draw_bglayer(db_board *b, int layer, uint8_t req_pri)
{
    int depth8 = BG_DEPTH_8BPP(b, layer);
    uint8_t size = BG_LARGE(b, layer) ? 32 : 16;
    uint16_t height = (uint16_t)(16 * size);
    uint8_t regbank = BG_TYPE(b, layer);
    const uint32_t *sr = b->spriteram;
    uint32_t r0 = sr_at(b, (regbank * 0x800) / 4 + 0x3f0 / 4 + (layer * 0x04) / 4);
    uint32_t r1 = sr_at(b, (regbank * 0x800) / 4 + 0x7f0 / 4 + (layer * 0x04) / 4);
    uint16_t scrollx = (uint16_t)(r0 & 0x000001ff);
    uint16_t scrolly = (uint16_t)((r0 & 0x03ff0000) >> 16);
    uint8_t tilebank = (uint8_t)(r1 & 0xff);
    int alpha = (int)((r1 & 0x00003f00) >> 8);
    uint8_t alphamap = (uint8_t)((r1 & 0x00008000) >> 15);
    uint8_t pri = (uint8_t)((r1 & 0xff000000) >> 24);
    if (pri != req_pri) return;
    alpha = alphamap ? -1 : pal6bit((uint8_t)(0x3f - alpha));
    /* zoom is not implemented for non-line layers (as in MAME) */
    if (tilebank >= 0x0a && tilebank <= 0x1f) {
        uint8_t basey = (uint8_t)(((0x400 - scrolly + CLIP_T - 1) & (height - 1)) >> 4);
        for (int sy = (CLIP_T - 1) >> 4; sy <= CLIP_B >> 4; sy++) {
            uint8_t basex = (uint8_t)(((0x200 - scrollx + CLIP_L - 1) & 0x1ff) >> 4);
            for (int sx = (CLIP_L - 1) >> 4; sx <= CLIP_R >> 4; sx++) {
                uint32_t w = sr[(tilebank * 0x800) / 4 + (((basey & 0x1f) << 5) | (basex & 0x1f))];
                uint32_t tileno = w & 0x0007ffff;
                uint8_t colour = (uint8_t)((w & 0xff000000) >> 24);
                gfx_alphatable(b, depth8, tileno, colour, (16 * sx) + (scrollx & 0xf), (16 * sy) + (scrolly & 0xf), alpha);
                basex++;
            }
            basey = (uint8_t)((basey + 1) & ((height >> 4) - 1));
        }
    }
}

static void cache_bitmap(db_board *b, int16_t scanline, int depth8, uint8_t size, uint8_t tilebank, int alpha, uint8_t *last_bank)
{
    uint8_t sy = (uint8_t)(scanline / 16);
    if (tilebank != last_bank[sy]) {
        int minsy = sy * 16, maxsy = minsy + 15;
        if (maxsy > 511) maxsy = 511;
        for (int y = minsy; y <= maxsy; y++)
            for (int x = 0; x < 512; x++) b->bg_bitmap[y * 512 + x] = BG_TRANSPEN;
        uint16_t height = (uint16_t)(size * 16);
        uint32_t offs = 32u * sy;
        for (int sx = 0; sx < 32; sx++) {
            uint32_t w = b->spriteram[(tilebank * 0x800) / 4 + offs];
            uint32_t tileno = w & 0x0007ffff;
            uint32_t colour = (w & 0xff000000) >> 24;
            int need_alpha = alpha < 0 ? -1 : 0xff;
            if (tileno)
                gfx_alphastore(b, depth8, tileno, colour, (16 * sx) & 0x1ff, (16 * sy) & (height - 1), need_alpha);
            offs++;
        }
        last_bank[sy] = tilebank;
    }
}

static void draw_bglayerscroll(db_board *b, int layer, uint8_t req_pri)
{
    int depth8 = BG_DEPTH_8BPP(b, layer);
    uint8_t size = BG_LARGE(b, layer) ? 32 : 16;
    uint16_t height = (uint16_t)(size * 16);
    uint8_t linebank = BG_TYPE(b, layer);
    uint8_t last_bank[32];
    memset(last_bank, 0xff, sizeof(last_bank));
    unsigned scroll_reg = (linebank * 0x800) / 4 + CLIP_T;
    unsigned pzab_reg = (linebank * 0x800) / 4 + CLIP_T + 0x400 / 4;
    for (int scanline = CLIP_T; scanline <= CLIP_B; scanline++) {
        uint32_t pzab = sr_at(b, pzab_reg), scr = sr_at(b, scroll_reg);
        uint8_t pri = (uint8_t)((pzab & 0xff000000) >> 24);
        if (pri == req_pri) {
            uint16_t scrollx = (uint16_t)(scr & 0x000001ff);
            uint16_t scrolly = (uint16_t)((scr & 0x03ff0000) >> 16);
            uint8_t zoom = (uint8_t)((pzab & 0x00ff0000) >> 16);
            uint8_t alphamap = (uint8_t)((pzab & 0x00008000) >> 15);
            int alpha = (int)((pzab & 0x00003f00) >> 8);
            uint8_t tilebank = (uint8_t)(pzab & 0xff);
            alpha = alphamap ? -1 : pal6bit((uint8_t)(0x3f - alpha));
            if (tilebank >= 0x0a && tilebank <= 0x1f) {
                uint16_t tilemap_scanline = (uint16_t)((scanline - scrolly + 0x400) & (height - 1));
                cache_bitmap(b, (int16_t)tilemap_scanline, depth8, size, tilebank, alpha, last_bank);
                const uint32_t *tilemap_line = &b->bg_bitmap[tilemap_scanline * 512];
                uint32_t scr_line[64 * 8];
                if (zoom) {
                    uint16_t step = bg_zoom[zoom];
                    int jj = (0x400 << 10) + (step * CLIP_L);
                    for (int ii = CLIP_L; ii <= CLIP_R; ii++) {
                        scr_line[ii] = tilemap_line[((jj >> 10) - scrollx) & 0x1ff];
                        jj += step;
                    }
                } else {
                    for (int ii = CLIP_L; ii <= CLIP_R; ii++) scr_line[ii] = tilemap_line[(ii - scrollx + 0x400) & 0x1ff];
                }
                uint32_t *dst = &b->bitmap[scanline * DB_BITMAP_W];
                if (alpha == 0xff) {
                    for (int x = CLIP_L; x <= CLIP_R; x++) if (scr_line[x] != BG_TRANSPEN) dst[x] = scr_line[x];
                } else if (alpha > 0) {
                    for (int x = CLIP_L; x <= CLIP_R; x++)
                        if (scr_line[x] != BG_TRANSPEN) dst[x] = alpha_blend_r32(dst[x], scr_line[x], (uint8_t)alpha);
                } else if (alpha < 0) {
                    for (int x = CLIP_L; x <= CLIP_R; x++)
                        if (scr_line[x] != BG_TRANSPEN) dst[x] = alpha_blend_r32(dst[x], scr_line[x], (uint8_t)(scr_line[x] >> 24));
                }
            }
        }
        scroll_reg++;
        pzab_reg++;
    }
}

static void draw_background(db_board *b, uint8_t req_pri)
{
    for (int i = 0; i <= 3; i++) {
        if (!BG_LAYER_ENABLE(b, i)) continue;
        if (BG_LINE(b, i)) draw_bglayerscroll(b, i, req_pri);
        else draw_bglayer(b, i, req_pri);
    }
}

/* --- sprites -------------------------------------------------------------- */

static inline void put_pixel(db_board *b, uint8_t source, uint32_t *dest, uint16_t *pri, const uint32_t *pal, uint16_t z, int alpha)
{
    if (z > 0) {
        if (z < *pri) return;
        if (source == 0) return;
        if (alpha == 0xff) *dest = pal[source];
        else if (alpha >= 0) *dest = alpha_blend_r32(*dest, pal[source], (uint8_t)alpha);
        else *dest = alphatable[source] == 0xff ? pal[source] : alpha_blend_r32(*dest, pal[source], alphatable[source]);
        *pri = z;
    } else {
        if (source == 0) return;
        if (alpha == 0xff) *dest = pal[source];
        else if (alpha >= 0) *dest = alpha_blend_r32(*dest, pal[source], (uint8_t)alpha);
        else *dest = alphatable[source] == 0xff ? pal[source] : alpha_blend_r32(*dest, pal[source], alphatable[source]);
    }
    (void)b;
}

static void drawgfxzoom(db_board *b, int depth8, uint32_t code, uint16_t color, uint8_t flipx, uint8_t flipy,
                        int32_t offsx, int32_t offsy, int alpha, uint32_t zoomx, uint32_t zoomy,
                        uint8_t wide, uint8_t high, uint16_t z)
{
    rect myclip = { CLIP_L, CLIP_R, CLIP_T, CLIP_B };
    int code_offset = 0;
    uint8_t buf[256];
    if (!zoomx || !zoomy) return;
    const uint32_t *pal = palbase(b, color);

    if (zoomx == 0x400 && zoomy == 0x400) {
        int xstart, xend, xinc, ystart, yend, yinc;
        if (flipx) { xstart = wide - 1; xend = -1; xinc = -1; } else { xstart = 0; xend = wide; xinc = +1; }
        if (flipy) { ystart = high - 1; yend = -1; yinc = -1; } else { ystart = 0; yend = high; yinc = +1; }
        for (int ytile = ystart; ytile != yend; ytile += yinc) {
            for (int xtile = xstart; xtile != xend; xtile += xinc) {
                const uint8_t *code_base = tile_data(b, depth8, code + (uint32_t)code_offset++, buf);
                int x_index_base = flipx ? 15 : 0;
                int y_index = flipy ? 15 : 0;
                int sx = offsx + xtile * 16, sy = offsy + ytile * 16;
                int ex = sx + 16, ey = sy + 16;
                if (sx < myclip.l) { int pixels = myclip.l - sx; sx += pixels; x_index_base += xinc * pixels; }
                if (sy < myclip.t) { int pixels = myclip.t - sy; sy += pixels; y_index += yinc * pixels; }
                if (ex > myclip.r + 1) ex -= ex - myclip.r - 1;
                if (ey > myclip.b + 1) ey -= ey - myclip.b - 1;
                if (ex > sx) {
                    for (int y = sy; y < ey; y++, y_index += yinc) {
                        const uint8_t *source = code_base + y_index * 16 + x_index_base;
                        uint32_t *dest = &b->bitmap[y * DB_BITMAP_W + sx];
                        uint16_t *pri = &b->z_bitmap[y * DB_BITMAP_W + sx];
                        for (int x = sx; x < ex; x++, dest++, pri++, source += xinc)
                            put_pixel(b, *source, dest, pri, pal, z, alpha);
                    }
                }
            }
        }
        return;
    }

    /* zoomed: assemble the whole sprite in zoom_bitmap first */
    for (int ytile = 0; ytile < high; ytile++) {
        for (int xtile = 0; xtile < wide; xtile++) {
            const uint8_t *code_base = tile_data(b, depth8, code + (uint32_t)code_offset++, buf);
            for (int ypixel = 0; ypixel < 16; ypixel++) {
                uint8_t *dest = &b->zoom_bitmap[(ypixel + ytile * 16) * 256 + xtile * 16];
                memcpy(dest, code_base + ypixel * 16, 16);
            }
        }
    }
    int sprite_screen_height = (int)((((uint64_t)high * 16 * (0x400 * 0x400)) / zoomy + 0x200) >> 10);
    int sprite_screen_width = (int)((((uint64_t)wide * 16 * (0x400 * 0x400)) / zoomx + 0x200) >> 10);
    if (!sprite_screen_width || !sprite_screen_height) return;
    int sx = offsx, sy = offsy, ex = sx + sprite_screen_width, ey = sy + sprite_screen_height;
    int x_index_base, y_index, dx, dy;
    if (flipx) { x_index_base = (sprite_screen_width - 1) * (int)zoomx; dx = -(int)zoomx; } else { x_index_base = 0; dx = (int)zoomx; }
    if (flipy) { y_index = (sprite_screen_height - 1) * (int)zoomy; dy = -(int)zoomy; } else { y_index = 0; dy = (int)zoomy; }
    if (sx < myclip.l) { int pixels = myclip.l - sx; sx += pixels; x_index_base += pixels * dx; }
    if (sy < myclip.t) { int pixels = myclip.t - sy; sy += pixels; y_index += pixels * dy; }
    if (ex > myclip.r + 1) ex -= ex - myclip.r - 1;
    if (ey > myclip.b + 1) ey -= ey - myclip.b - 1;
    if (ex > sx) {
        for (int y = sy; y < ey; y++) {
            const uint8_t *source = &b->zoom_bitmap[((y_index >> 10) & 0xff) * 256];
            uint32_t *dest = &b->bitmap[y * DB_BITMAP_W];
            uint16_t *pri = &b->z_bitmap[y * DB_BITMAP_W];
            int x_index = x_index_base;
            for (int x = sx; x < ex; x++) {
                put_pixel(b, source[(x_index >> 10) & 0xff], &dest[x], &pri[x], pal, z, alpha);
                x_index += dx;
            }
            y_index += dy;
        }
    }
}

/* screen_vblank: latch the sprite list (format at 0x3800, 16-bit entries) */
void db_video_get_sprites(db_board *b)
{
    const uint32_t *sr = b->spriteram;
    int n = 0;
    for (int listcntr = 0; listcntr < 0x800 / 2; listcntr++) {
        uint32_t w = sr[0x3800 / 4 + (listcntr >> 1)];
        uint16_t listdat = (uint16_t)((listcntr & 1) ? w : w >> 16);
        uint32_t sprnum = (listdat & 0x03ffu) * 4;
        struct db_sprite *s = &b->sprites[n];
        int32_t ypos = (int32_t)((sr[sprnum + 0] & 0x03ff0000) >> 16);
        int32_t xpos = (int32_t)(sr[sprnum + 0] & 0x000003ff);
        if (ypos & 0x200) ypos -= 0x400;
        if (xpos & 0x200) xpos -= 0x400;
        s->ypos = ypos;
        s->xpos = xpos;
        s->high = (uint8_t)(((sr[sprnum + 1] & 0x0f000000) >> 24) + 1);
        s->wide = (uint8_t)(((sr[sprnum + 1] & 0x00000f00) >> 8) + 1);
        s->flpy = (uint8_t)((sr[sprnum + 1] & 0x80000000u) >> 31);
        s->spr_pri = (uint8_t)((sr[sprnum + 1] & 0x30000000) >> 28);
        s->flpx = (uint8_t)((sr[sprnum + 1] & 0x00008000) >> 15);
        s->bg_pri = (uint8_t)((sr[sprnum + 1] & 0x00003000) >> 12);
        s->zoomy = (uint8_t)((sr[sprnum + 1] & 0x00ff0000) >> 16);
        s->zoomx = (uint8_t)(sr[sprnum + 1] & 0x000000ff);
        s->tnum = sr[sprnum + 2] & 0x0007ffff;
        s->dpth = (uint8_t)((sr[sprnum + 2] & 0x00800000) >> 23);
        s->colr = (uint16_t)((sr[sprnum + 2] & 0xff000000) >> 24);
        s->alpha = (int16_t)((sr[sprnum + 2] & 0x00700000) >> 20);
        n++;
        if (listdat & 0x4000) break;
    }
    b->sprite_count = n;
}

static void draw_sprites(db_board *b, uint8_t req_pri)
{
    for (int i = 0; i < b->sprite_count; i++) {
        const struct db_sprite *s = &b->sprites[i];
        if (SPRITE_PRI(b, s->bg_pri) != req_pri) continue;
        uint32_t zw = b->zoomram[s->zoomy >> 1], xw = b->zoomram[s->zoomx >> 1];
        uint32_t zoomy = (s->zoomy & 1) ? (zw & 0xffff) : (zw >> 16);
        uint32_t zoomx = (s->zoomx & 1) ? (xw & 0xffff) : (xw >> 16);
        uint8_t at = (uint8_t)(b->vidregs[s->alpha >> 2] >> (24 - 8 * (s->alpha & 3)));
        int alphamap = (at >> 7) & 1;
        int alpha = at & 0x3f;
        alpha = alphamap ? -1 : pal6bit((uint8_t)(0x3f - alpha));
        if (zoomy && zoomx)
            drawgfxzoom(b, s->dpth, s->tnum, s->colr, s->flpx, s->flpy, s->xpos, s->ypos, alpha,
                        zoomx, zoomy, s->wide, s->high, (uint16_t)i);
    }
}

/* --- line fill / blend ---------------------------------------------------- */

static void prelineblend(db_board *b)
{
    uint8_t bank = (uint8_t)((b->vidregs[7] & 0xff000000) >> 24);
    for (int y = CLIP_T; y <= CLIP_B; y++) {
        uint32_t v = sr_at(b, (bank * 0x800) / 4 + y) >> 8;
        uint32_t *dst = &b->bitmap[y * DB_BITMAP_W];
        for (int x = CLIP_L; x <= CLIP_R; x++) dst[x] = v;
    }
}

static void postlineblend(db_board *b, uint8_t req_pri)
{
    uint8_t bank = (uint8_t)((b->vidregs[7] & 0xff000000) >> 24);
    if ((b->vidregs[2] & 0xf) != req_pri) return;
    for (int y = CLIP_T; y <= CLIP_B; y++) {
        uint32_t lb = sr_at(b, (bank * 0x800) / 4 + 0x400 / 4 + y);
        uint32_t *dst = &b->bitmap[y * DB_BITMAP_W];
        if (lb & 0x80) {
            for (int x = CLIP_L; x <= CLIP_R; x++) dst[x] = lb >> 8;
        } else if (lb & 0x7f) {
            for (int x = CLIP_L; x <= CLIP_R; x++) dst[x] = alpha_blend_r32(dst[x], lb >> 8, (uint8_t)(2 * (lb & 0x7f)));
        }
    }
}

void db_video_update(db_board *b)
{
    init_tables();
    for (int y = CLIP_T; y <= CLIP_B; y++)
        memset(&b->z_bitmap[y * DB_BITMAP_W + CLIP_L], 0, (CLIP_R - CLIP_L + 1) * sizeof(uint16_t));
    prelineblend(b);
    for (uint8_t i = 0; i <= 7; i++) {
        draw_sprites(b, i);
        draw_background(b, i);
        postlineblend(b, i);
    }
    for (int y = 0; y < DB_SCREEN_H; y++)
        for (int x = 0; x < DB_SCREEN_W; x++)
            b->screen[y * DB_SCREEN_W + x] = b->bitmap[y * DB_BITMAP_W + x] & 0x00ffffffu;
}

void db_video_render(const db_board *b, uint32_t *pixels)
{
    memcpy(pixels, b->screen, sizeof(b->screen));
}
