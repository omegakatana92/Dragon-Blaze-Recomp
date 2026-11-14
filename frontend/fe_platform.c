/* Frontend platform helpers: UTF-8 paths on Windows and POSIX, directory
 * listing, atomic file writes, INI persistence, PNG output and logging. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#endif
#include "fe_internal.h"
#include "miniz.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- logging */

static FILE *log_file;

void fe_log_open(const char *path)
{
    if (log_file) fclose(log_file);
#ifdef _WIN32
    wchar_t w[FE_PATH_MAX];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, FE_PATH_MAX);
    log_file = _wfopen(w, L"w");
#else
    log_file = fopen(path, "w");
#endif
}

void fe_log_close(void)
{
    if (log_file) fclose(log_file);
    log_file = NULL;
}

void fe_log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);
    fprintf(stderr, "[%s] %s\n", stamp, line);
    if (log_file) { fprintf(log_file, "[%s] %s\n", stamp, line); fflush(log_file); }
}

/* ------------------------------------------------------------------ paths */

char fe_path_sep(void)
{
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

int fe_path_join(char *out, size_t n, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (la && is_sep(a[la - 1])) return snprintf(out, n, "%s%s", a, b) < (int)n;
    return snprintf(out, n, "%s%c%s", a, fe_path_sep(), b) < (int)n;
}

const char *fe_basename(const char *path)
{
    const char *p = path, *last = path;
    for (; *p; p++) if (is_sep(*p)) last = p + 1;
    return last;
}

void fe_dirname(const char *path, char *out, size_t n)
{
    snprintf(out, n, "%s", path);
    size_t len = strlen(out);
    while (len > 0 && !is_sep(out[len - 1])) len--;
    if (len > 1 && is_sep(out[len - 1])) {
        /* keep "C:\" and "/" roots intact */
        if (!(len == 3 && out[1] == ':') && len != 1) len--;
    }
    out[len] = 0;
}

#ifdef _WIN32
static void widen(const char *s, wchar_t *w, int n) { MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); }
static void narrow(const wchar_t *w, char *s, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); }
#endif

int fe_file_exists(const char *path)
{
#ifdef _WIN32
    wchar_t w[FE_PATH_MAX]; widen(path, w, FE_PATH_MAX);
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

int fe_dir_exists(const char *path)
{
#ifdef _WIN32
    wchar_t w[FE_PATH_MAX]; widen(path, w, FE_PATH_MAX);
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int mkdir_one(const char *path)
{
#ifdef _WIN32
    wchar_t w[FE_PATH_MAX]; widen(path, w, FE_PATH_MAX);
    return CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path, 0755) == 0 || fe_dir_exists(path);
#endif
}

int fe_mkdirs(const char *path)
{
    char tmp[FE_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && is_sep(tmp[len - 1])) tmp[--len] = 0;
    for (size_t i = 1; i < len; i++) {
        if (is_sep(tmp[i])) {
            if (i == 2 && tmp[1] == ':') continue;
            char c = tmp[i];
            tmp[i] = 0;
            if (!fe_dir_exists(tmp)) mkdir_one(tmp);
            tmp[i] = c;
        }
    }
    return fe_dir_exists(tmp) || mkdir_one(tmp);
}

static FILE *open_utf8(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t w[FE_PATH_MAX], m[8];
    widen(path, w, FE_PATH_MAX);
    widen(mode, m, 8);
    return _wfopen(w, m);
#else
    return fopen(path, mode);
#endif
}

uint8_t *fe_read_file(const char *path, size_t *size)
{
    FILE *f = open_utf8(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    uint8_t *data = (uint8_t *)malloc((size_t)len + 1);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) { free(data); return NULL; }
    data[len] = 0;
    *size = (size_t)len;
    return data;
}

int fe_write_file_atomic(const char *path, const void *data, size_t size)
{
    char tmp[FE_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = open_utf8(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size;
    ok = (fflush(f) == 0) && ok;
    fclose(f);
    if (!ok) { remove(tmp); return 0; }
#ifdef _WIN32
    wchar_t wt[FE_PATH_MAX], wp[FE_PATH_MAX];
    widen(tmp, wt, FE_PATH_MAX);
    widen(path, wp, FE_PATH_MAX);
    if (!MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(wt); return 0; }
#else
    if (rename(tmp, path) != 0) { remove(tmp); return 0; }
#endif
    return 1;
}

int fe_home_dir(char *out, size_t n)
{
#ifdef _WIN32
    const wchar_t *w = _wgetenv(L"USERPROFILE");
    if (!w) return 0;
    narrow(w, out, (int)n);
    return 1;
#else
    const char *h = getenv("HOME");
    if (!h) return 0;
    snprintf(out, n, "%s", h);
    return 1;
#endif
}

static int dirent_cmp(const void *a, const void *b)
{
    const fe_dirent *x = (const fe_dirent *)a, *y = (const fe_dirent *)b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
#ifdef _WIN32
    return _stricmp(x->name, y->name);
#else
    return strcasecmp(x->name, y->name);
#endif
}

int fe_list_dir(const char *path, fe_dirent **out)
{
    int count = 0, cap = 64;
    fe_dirent *list = (fe_dirent *)malloc(sizeof(fe_dirent) * (size_t)cap);
    if (!list) return -1;
#ifdef _WIN32
    char pattern[FE_PATH_MAX];
    fe_path_join(pattern, sizeof(pattern), path, "*");
    wchar_t w[FE_PATH_MAX]; widen(pattern, w, FE_PATH_MAX);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) { free(list); return -1; }
    do {
        char name[512];
        narrow(fd.cFileName, name, sizeof(name));
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (count == cap) { cap *= 2; fe_dirent *n2 = (fe_dirent *)realloc(list, sizeof(fe_dirent) * (size_t)cap); if (!n2) break; list = n2; }
        snprintf(list[count].name, sizeof(list[count].name), "%s", name);
        list[count].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) { free(list); return -1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[FE_PATH_MAX];
        fe_path_join(full, sizeof(full), path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (count == cap) { cap *= 2; fe_dirent *n2 = (fe_dirent *)realloc(list, sizeof(fe_dirent) * (size_t)cap); if (!n2) break; list = n2; }
        snprintf(list[count].name, sizeof(list[count].name), "%s", e->d_name);
        list[count].is_dir = S_ISDIR(st.st_mode);
        count++;
    }
    closedir(d);
#endif
    qsort(list, (size_t)count, sizeof(fe_dirent), dirent_cmp);
    *out = list;
    return count;
}

int fe_list_roots(fe_dirent **out)
{
    fe_dirent *list = (fe_dirent *)calloc(32, sizeof(fe_dirent));
    int count = 0;
    if (!list) return -1;
#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (drives & (1u << i)) {
            snprintf(list[count].name, sizeof(list[count].name), "%c:\\", 'A' + i);
            list[count].is_dir = 1;
            count++;
        }
    }
#else
    const char *roots[] = { "/", "/run/media", "/media", "/mnt", "/home" };
    char home[FE_PATH_MAX];
    if (fe_home_dir(home, sizeof(home))) {
        snprintf(list[count].name, sizeof(list[count].name), "%s", home);
        list[count++].is_dir = 1;
    }
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (fe_dir_exists(roots[i])) {
            snprintf(list[count].name, sizeof(list[count].name), "%s", roots[i]);
            list[count++].is_dir = 1;
        }
    }
#endif
    *out = list;
    return count;
}

/* -------------------------------------------------------------------- INI */

void fe_ini_free(fe_ini *ini)
{
    free(ini->e);
    memset(ini, 0, sizeof(*ini));
}

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

int fe_ini_load(fe_ini *ini, const char *path)
{
    size_t size;
    char *data = (char *)fe_read_file(path, &size);
    if (!data) return 0;
    char section[48] = "";
    char *line = data;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        trim(line);
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) { *end = 0; snprintf(section, sizeof(section), "%s", line + 1); }
        } else if (line[0] && line[0] != '#' && line[0] != ';') {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                char key[64];
                snprintf(key, sizeof(key), "%s", line);
                trim(key);
                char *value = eq + 1;
                trim(value);
                fe_ini_set(ini, section, key, value);
            }
        }
        line = next;
    }
    free(data);
    return 1;
}

int fe_ini_save(const fe_ini *ini, const char *path)
{
    size_t cap = 4096 + (size_t)ini->count * 160, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return 0;
    n += (size_t)snprintf(out + n, cap - n, "# Settings written by the frontend. Safe to edit while the game is closed.\n");
    /* emit section by section in first-seen order */
    for (int i = 0; i < ini->count; i++) {
        int first = 1;
        for (int j = 0; j < i; j++) if (!strcmp(ini->e[j].section, ini->e[i].section)) { first = 0; break; }
        if (!first) continue;
        n += (size_t)snprintf(out + n, cap - n, "\n[%s]\n", ini->e[i].section);
        for (int j = i; j < ini->count; j++) {
            if (strcmp(ini->e[j].section, ini->e[i].section)) continue;
            size_t need = strlen(ini->e[j].key) + strlen(ini->e[j].value) + 4;
            if (n + need + 64 > cap) {
                cap = cap * 2 + need;
                char *o2 = (char *)realloc(out, cap);
                if (!o2) { free(out); return 0; }
                out = o2;
            }
            n += (size_t)snprintf(out + n, cap - n, "%s = %s\n", ini->e[j].key, ini->e[j].value);
        }
    }
    int ok = fe_write_file_atomic(path, out, n);
    free(out);
    return ok;
}

const char *fe_ini_get(const fe_ini *ini, const char *section, const char *key)
{
    for (int i = 0; i < ini->count; i++)
        if (!strcmp(ini->e[i].section, section) && !strcmp(ini->e[i].key, key)) return ini->e[i].value;
    return NULL;
}

void fe_ini_set(fe_ini *ini, const char *section, const char *key, const char *value)
{
    for (int i = 0; i < ini->count; i++) {
        if (!strcmp(ini->e[i].section, section) && !strcmp(ini->e[i].key, key)) {
            snprintf(ini->e[i].value, sizeof(ini->e[i].value), "%s", value);
            return;
        }
    }
    if (ini->count == ini->cap) {
        int cap = ini->cap ? ini->cap * 2 : 64;
        fe_ini_entry *e = (fe_ini_entry *)realloc(ini->e, sizeof(fe_ini_entry) * (size_t)cap);
        if (!e) return;
        ini->e = e; ini->cap = cap;
    }
    fe_ini_entry *e = &ini->e[ini->count++];
    snprintf(e->section, sizeof(e->section), "%s", section);
    snprintf(e->key, sizeof(e->key), "%s", key);
    snprintf(e->value, sizeof(e->value), "%s", value);
}

int fe_ini_get_int(const fe_ini *ini, const char *section, const char *key, int fallback)
{
    const char *v = fe_ini_get(ini, section, key);
    if (!v || !*v) return fallback;
    char *end;
    long x = strtol(v, &end, 0);
    return end == v ? fallback : (int)x;
}

void fe_ini_set_int(fe_ini *ini, const char *section, const char *key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    fe_ini_set(ini, section, key, buf);
}

/* -------------------------------------------------------------------- PNG */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

int fe_write_png(const char *path, const uint8_t *rgb, int width, int height)
{
    size_t stride = (size_t)width * 3 + 1, raw_size = stride * (size_t)height;
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    if (!raw) return 0;
    for (int y = 0; y < height; y++) {
        raw[(size_t)y * stride] = 0;
        memcpy(raw + (size_t)y * stride + 1, rgb + (size_t)y * (size_t)width * 3, (size_t)width * 3);
    }
    mz_ulong zsize = mz_compressBound((mz_ulong)raw_size);
    uint8_t *z = (uint8_t *)malloc(zsize);
    int ok = z && mz_compress2(z, &zsize, raw, (mz_ulong)raw_size, 6) == MZ_OK;
    free(raw);
    if (!ok) { free(z); return 0; }
    size_t total = 8 + 25 + 12 + zsize + 12;
    uint8_t *png = (uint8_t *)malloc(total);
    if (!png) { free(z); return 0; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    size_t n = 0;
    memcpy(png, sig, 8); n = 8;
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)width); put32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    const struct { const char *type; const uint8_t *data; uint32_t len; } chunks[3] = {
        { "IHDR", ihdr, 13 }, { "IDAT", z, (uint32_t)zsize }, { "IEND", NULL, 0 } };
    for (int i = 0; i < 3; i++) {
        put32(png + n, chunks[i].len); n += 4;
        memcpy(png + n, chunks[i].type, 4);
        if (chunks[i].len) memcpy(png + n + 4, chunks[i].data, chunks[i].len);
        uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, png + n, chunks[i].len + 4);
        n += 4 + chunks[i].len;
        put32(png + n, crc); n += 4;
    }
    free(z);
    ok = fe_write_file_atomic(path, png, n);
    free(png);
    return ok;
}
