/* Frontend drawing: embedded anti-aliased font atlas and simple shapes. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#endif
#include "fe_internal.h"
#include "assets/fe_font.h"
#include "miniz.h"

#include <stdlib.h>
#include <string.h>

static SDL_Texture *font_texture;

int fe_draw_init(SDL_Renderer *renderer)
{
    size_t size = (size_t)FE_FONT_ATLAS_W * FE_FONT_ATLAS_H;
    uint8_t *alpha = (uint8_t *)malloc(size);
    if (!alpha) return 0;
    mz_ulong out_len = (mz_ulong)size;
    if (mz_uncompress(alpha, &out_len, FE_FONT_Z, FE_FONT_Z_SIZE) != MZ_OK || out_len != size) {
        free(alpha);
        return 0;
    }
    uint32_t *rgba = (uint32_t *)malloc(size * 4);
    if (!rgba) { free(alpha); return 0; }
    for (size_t i = 0; i < size; i++) rgba[i] = ((uint32_t)alpha[i] << 24) | 0x00FFFFFFu;
    free(alpha);
    font_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                     FE_FONT_ATLAS_W, FE_FONT_ATLAS_H);
    if (font_texture) {
        SDL_UpdateTexture(font_texture, NULL, rgba, FE_FONT_ATLAS_W * 4);
        SDL_SetTextureBlendMode(font_texture, SDL_BLENDMODE_BLEND);
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetTextureScaleMode(font_texture, SDL_ScaleModeLinear);
#endif
    }
    free(rgba);
    return font_texture != NULL;
}

void fe_draw_shutdown(void)
{
    if (font_texture) SDL_DestroyTexture(font_texture);
    font_texture = NULL;
}

void fe_fill(SDL_Renderer *r, int x, int y, int w, int h, fe_color c)
{
    SDL_SetRenderDrawBlendMode(r, c.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(r, &rc);
}

void fe_frame_rect(SDL_Renderer *r, int x, int y, int w, int h, int t, fe_color c)
{
    fe_fill(r, x, y, w, t, c);
    fe_fill(r, x, y + h - t, w, t, c);
    fe_fill(r, x, y + t, t, h - 2 * t, c);
    fe_fill(r, x + w - t, y + t, t, h - 2 * t, c);
}

void fe_round_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius, fe_color c)
{
    if (radius * 2 > h) radius = h / 2;
    if (radius * 2 > w) radius = w / 2;
    fe_fill(r, x + radius, y, w - 2 * radius, h, c);
    for (int i = 0; i < radius; i++) {
        /* quarter-circle rows, cheap and good enough at UI sizes */
        double dy = radius - i - 0.5;
        int dx = (int)(radius - SDL_sqrt((double)radius * radius - dy * dy) + 0.5);
        fe_fill(r, x + dx, y + i, radius - dx, 1, c);
        fe_fill(r, x + w - radius, y + i, radius - dx, 1, c);
        fe_fill(r, x + dx, y + h - 1 - i, radius - dx, 1, c);
        fe_fill(r, x + w - radius, y + h - 1 - i, radius - dx, 1, c);
    }
    fe_fill(r, x, y + radius, radius, h - 2 * radius, c);
    fe_fill(r, x + w - radius, y + radius, radius, h - 2 * radius, c);
}

void fe_gradient_v(SDL_Renderer *r, int x, int y, int w, int h, fe_color top, fe_color bottom)
{
    if (h <= 0) return;
    int steps = h < 64 ? h : 64;
    for (int i = 0; i < steps; i++) {
        int y0 = y + h * i / steps, y1 = y + h * (i + 1) / steps;
        fe_color c = {
            (uint8_t)(top.r + (bottom.r - top.r) * i / steps),
            (uint8_t)(top.g + (bottom.g - top.g) * i / steps),
            (uint8_t)(top.b + (bottom.b - top.b) * i / steps),
            (uint8_t)(top.a + (bottom.a - top.a) * i / steps) };
        fe_fill(r, x, y0, w, y1 - y0, c);
    }
}

static double scale_for(int px) { return (double)px / (double)FE_FONT_PX; }

/* Decode one UTF-8 code point (invalid bytes become U+FFFD) and map it to
 * an atlas glyph: ASCII 32-126 then Latin-1 160-255; others draw as '?'. */
static int next_glyph(const unsigned char **ps)
{
    const unsigned char *p = *ps;
    unsigned cp = *p++;
    if (cp >= 0x80) {
        int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        cp = extra == 3 ? cp & 7 : extra == 2 ? cp & 15 : extra == 1 ? cp & 31 : 0xFFFD;
        for (int i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (*p++ & 0x3F);
        }
    }
    *ps = p;
    if (cp >= 32 && cp <= 126) return (int)cp - 32;
    if (cp >= 160 && cp <= 255 && FE_FONT_GLYPH_COUNT > 95) return 95 + (int)cp - 160;
    return '?' - 32;
}

int fe_text_width(const char *s, int px)
{
    double w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) w += FE_FONT_GLYPHS[next_glyph(&p)].advance;
    return (int)(w * scale_for(px) + 0.5);
}

int fe_text_line_height(int px) { return (int)(FE_FONT_LINE * scale_for(px) + 0.5); }

int fe_text(SDL_Renderer *r, const char *s, int x, int y, int px, fe_color c, int align)
{
    if (!font_texture || !s) return 0;
    int width = fe_text_width(s, px);
    if (align == 1) x -= width / 2;
    else if (align == 2) x -= width;
    double k = scale_for(px);
    double pen = x;
    SDL_SetTextureColorMod(font_texture, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(font_texture, c.a);
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int index = next_glyph(&p);
        const fe_glyph_def *g = &FE_FONT_GLYPHS[index];
        if (index != 0) {
            SDL_Rect src = { g->x, g->y, g->w, g->h };
            SDL_Rect dst = { (int)(pen + g->left * k + 0.5), y, (int)(g->w * k + 0.99), (int)(g->h * k + 0.99) };
            SDL_RenderCopy(r, font_texture, &src, &dst);
        }
        pen += g->advance * k;
    }
    return width;
}
