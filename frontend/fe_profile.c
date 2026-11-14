/* Minimal JSON reader for game profiles (see fe_profile.h).  Supports
 * objects, arrays, strings (with \" \\ \/ \n \t \uXXXX escapes, non-ASCII
 * passed through), numbers, true/false/null.  Only the keys listed in the
 * profile structure are stored. */
#include "fe_profile.h"
#include "fe_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const FEATURE_NAMES[FE_FEAT_COUNT] = {
    "save_states", "rewind", "nvram", "input_movies", "gadgets", "input_display", "screenshots", "widescreen"
};

const char *fe_feature_name(int f) { return f >= 0 && f < FE_FEAT_COUNT ? FEATURE_NAMES[f] : "?"; }

typedef struct jr { const char *s, *end; int bad; char err[160]; } jr;

static void ws(jr *r) { while (r->s < r->end && isspace((unsigned char)*r->s)) r->s++; }
static void fail(jr *r, const char *what)
{
    if (!r->bad) snprintf(r->err, sizeof(r->err), "%s", what);
    r->bad = 1;
}

static void str(jr *r, char *out, size_t n)
{
    size_t k = 0;
    if (r->s >= r->end || *r->s != '"') { fail(r, "expected a string"); return; }
    r->s++;
    while (r->s < r->end && *r->s != '"') {
        char c = *r->s++;
        if (c == '\\' && r->s < r->end) {
            char e = *r->s++;
            if (e == 'n') c = '\n'; else if (e == 't') c = '\t'; else if (e == 'r') c = '\r';
            else if (e == 'u') {
                unsigned v = 0;
                for (int i = 0; i < 4 && r->s < r->end; i++) { v = v * 16 + (unsigned)(isdigit((unsigned char)*r->s) ? *r->s - '0' : (tolower((unsigned char)*r->s) - 'a' + 10)); r->s++; }
                c = v < 128 ? (char)v : '?';
            } else c = e;
        }
        if (out && k + 1 < n) out[k++] = c;
    }
    if (out && n) out[k] = 0;
    if (r->s >= r->end) { fail(r, "unterminated string"); return; }
    r->s++;
}

static double num(jr *r)
{
    char *e;
    double v = strtod(r->s, &e);
    if (e == r->s) { fail(r, "expected a number"); return 0; }
    r->s = e;
    return v;
}

static int lit(jr *r, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(r->end - r->s) >= n && !strncmp(r->s, w, n)) { r->s += n; return 1; }
    return 0;
}

static void skip_value(jr *r, int depth);

static void skip_value(jr *r, int depth)
{
    ws(r);
    if (depth > 32) { fail(r, "nesting too deep"); return; }
    if (r->s >= r->end) { fail(r, "unexpected end"); return; }
    char c = *r->s;
    if (c == '"') { str(r, NULL, 0); return; }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        r->s++; ws(r);
        if (r->s < r->end && *r->s == close) { r->s++; return; }
        for (;;) {
            if (c == '{') { ws(r); str(r, NULL, 0); ws(r); if (r->s >= r->end || *r->s != ':') { fail(r, "expected ':'"); return; } r->s++; }
            skip_value(r, depth + 1);
            if (r->bad) return;
            ws(r);
            if (r->s < r->end && *r->s == ',') { r->s++; continue; }
            if (r->s < r->end && *r->s == close) { r->s++; return; }
            fail(r, "expected ',' or a closing bracket"); return;
        }
    }
    if (lit(r, "true") || lit(r, "false") || lit(r, "null")) return;
    num(r);
}

/* bool or null: returns 1/0, or -1 for null */
static int boolean(jr *r)
{
    ws(r);
    if (lit(r, "true")) return 1;
    if (lit(r, "false")) return 0;
    if (lit(r, "null")) return -1;
    fail(r, "expected true or false");
    return -1;
}

typedef void (*member_fn)(jr *r, const char *key, fe_profile *p);

static void object(jr *r, fe_profile *p, member_fn fn)
{
    ws(r);
    if (r->s >= r->end || *r->s != '{') { fail(r, "expected an object"); return; }
    r->s++; ws(r);
    if (r->s < r->end && *r->s == '}') { r->s++; return; }
    for (;;) {
        char key[64];
        ws(r); str(r, key, sizeof(key)); ws(r);
        if (r->bad) return;
        if (r->s >= r->end || *r->s != ':') { fail(r, "expected ':'"); return; }
        r->s++; ws(r);
        fn(r, key, p);
        if (r->bad) return;
        ws(r);
        if (r->s < r->end && *r->s == ',') { r->s++; continue; }
        if (r->s < r->end && *r->s == '}') { r->s++; return; }
        fail(r, "expected ',' or '}'"); return;
    }
}

static void display_member(jr *r, const char *key, fe_profile *p)
{
    if (!strcmp(key, "aspect")) {
        ws(r);
        if (r->s < r->end && *r->s == '"') {          /* "4:3" */
            char t[32]; str(r, t, sizeof(t));
            double a = 0, b = 0;
            if (sscanf(t, "%lf:%lf", &a, &b) == 2 && a > 0 && b > 0) p->aspect = a / b;
            else fail(r, "display.aspect must look like \"4:3\"");
        } else p->aspect = num(r);
    } else if (!strcmp(key, "rotation")) p->rotation = (int)num(r);
    else if (!strcmp(key, "integer_scale")) p->integer_scale = boolean(r);
    else skip_value(r, 0);
}

static void features_member(jr *r, const char *key, fe_profile *p)
{
    for (int f = 0; f < FE_FEAT_COUNT; f++)
        if (!strcmp(key, FEATURE_NAMES[f])) { p->requested[f] = boolean(r); return; }
    skip_value(r, 0);
}

static void top_member(jr *r, const char *key, fe_profile *p)
{
    if (!strcmp(key, "id")) str(r, p->id, sizeof(p->id));
    else if (!strcmp(key, "title")) str(r, p->title, sizeof(p->title));
    else if (!strcmp(key, "subtitle")) str(r, p->subtitle, sizeof(p->subtitle));
    else if (!strcmp(key, "manufacturer")) str(r, p->manufacturer, sizeof(p->manufacturer));
    else if (!strcmp(key, "year")) { ws(r); if (r->s < r->end && *r->s == '"') str(r, p->year, sizeof(p->year)); else snprintf(p->year, sizeof(p->year), "%d", (int)num(r)); }
    else if (!strcmp(key, "rom_hint")) str(r, p->rom_hint, sizeof(p->rom_hint));
    else if (!strcmp(key, "players")) p->players = (int)num(r);
    else if (!strcmp(key, "display")) object(r, p, display_member);
    else if (!strcmp(key, "features")) object(r, p, features_member);
    else skip_value(r, 0);
}

int fe_profile_parse(fe_profile *p, const char *json, size_t size)
{
    memset(p, 0, sizeof(*p));
    p->rotation = -1;
    p->integer_scale = -1;
    for (int f = 0; f < FE_FEAT_COUNT; f++) p->requested[f] = -1;
    jr r = { json, json + size, 0, "" };
    object(&r, p, top_member);
    ws(&r);
    if (!r.bad && r.s != r.end) fail(&r, "trailing data after the profile object");
    if (r.bad) {
        int line = 1;
        for (const char *c = json; c < r.s && c < json + size; c++) if (*c == '\n') line++;
        snprintf(p->error, sizeof(p->error), "line %d: %s", line, r.err);
        return 0;
    }
    if (p->players < 0 || p->players > 8) { snprintf(p->error, sizeof(p->error), "players must be 1-8"); return 0; }
    if (p->aspect < 0 || p->aspect > 4) { snprintf(p->error, sizeof(p->error), "display.aspect is out of range"); return 0; }
    p->loaded = 1;
    return 1;
}

int fe_profile_load(fe_profile *p, const char *path)
{
    size_t size = 0;
    uint8_t *data = fe_read_file(path, &size);
    if (!data) { memset(p, 0, sizeof(*p)); snprintf(p->error, sizeof(p->error), "cannot read %s", path); return 0; }
    int ok = fe_profile_parse(p, (const char *)data, size);
    free(data);
    if (ok) snprintf(p->path, sizeof(p->path), "%s", path);
    return ok;
}
