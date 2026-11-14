/* Internal frontend interfaces (not part of the game-facing API). */
#ifndef FE_INTERNAL_H
#define FE_INTERNAL_H

#include "fe.h"
#include <SDL.h>
#include <stdio.h>

#define FE_PATH_MAX 2048

/* ---- platform: paths, directories, files, logging (fe_platform.c) ---- */
void fe_log(const char *fmt, ...);
void fe_log_open(const char *path);
void fe_log_close(void);
int fe_path_join(char *out, size_t n, const char *a, const char *b);
int fe_mkdirs(const char *path);
int fe_file_exists(const char *path);
int fe_dir_exists(const char *path);
uint8_t *fe_read_file(const char *path, size_t *size);
int fe_write_file_atomic(const char *path, const void *data, size_t size);
const char *fe_basename(const char *path);
void fe_dirname(const char *path, char *out, size_t n);
int fe_home_dir(char *out, size_t n);
char fe_path_sep(void);

typedef struct fe_dirent { char name[512]; int is_dir; } fe_dirent;
/* Lists a directory; returns count (malloc'd array in *out) or -1. */
int fe_list_dir(const char *path, fe_dirent **out);
/* Windows: drive roots ("C:\\"), elsewhere "/" and common mount points. */
int fe_list_roots(fe_dirent **out);

/* Minimal INI store (key=value under [section]). */
typedef struct fe_ini_entry { char section[48]; char key[64]; char value[FE_PATH_MAX]; } fe_ini_entry;
typedef struct fe_ini { fe_ini_entry *e; int count, cap; } fe_ini;
void fe_ini_free(fe_ini *ini);
int fe_ini_load(fe_ini *ini, const char *path);
int fe_ini_save(const fe_ini *ini, const char *path);
const char *fe_ini_get(const fe_ini *ini, const char *section, const char *key);
void fe_ini_set(fe_ini *ini, const char *section, const char *key, const char *value);
int fe_ini_get_int(const fe_ini *ini, const char *section, const char *key, int fallback);
void fe_ini_set_int(fe_ini *ini, const char *section, const char *key, int value);

/* PNG writer for RGB888 rows (uses miniz). */
int fe_write_png(const char *path, const uint8_t *rgb, int width, int height);

/* ---- drawing (fe_draw.c) ---- */
typedef struct fe_color { uint8_t r, g, b, a; } fe_color;
int fe_draw_init(SDL_Renderer *renderer);
void fe_draw_shutdown(void);
void fe_fill(SDL_Renderer *r, int x, int y, int w, int h, fe_color c);
void fe_frame_rect(SDL_Renderer *r, int x, int y, int w, int h, int t, fe_color c);
void fe_round_rect(SDL_Renderer *r, int x, int y, int w, int h, int radius, fe_color c);
/* px: text cap size in pixels. align: 0 left, 1 center, 2 right. Returns width. */
int fe_text(SDL_Renderer *r, const char *s, int x, int y, int px, fe_color c, int align);
int fe_text_width(const char *s, int px);
int fe_text_line_height(int px);
void fe_gradient_v(SDL_Renderer *r, int x, int y, int w, int h, fe_color top, fe_color bottom);

#endif
