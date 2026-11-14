/* External game profiles (profiles/<id>.json).
 *
 * A profile describes a game's identity, presentation preferences and the
 * frontend features it asks for.  It never enables anything by itself: the
 * frontend combines each request with what the game adapter actually
 * implements (fe_game callbacks) and reports the outcome in the log and the
 * About screen.  Unknown keys are ignored so profiles can grow. */
#ifndef FE_PROFILE_H
#define FE_PROFILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    FE_FEAT_SAVE_STATES, FE_FEAT_REWIND, FE_FEAT_NVRAM, FE_FEAT_MOVIES, FE_FEAT_GADGETS,
    FE_FEAT_INPUT_DISPLAY, FE_FEAT_SCREENSHOTS, FE_FEAT_WIDESCREEN, FE_FEAT_COUNT
};

typedef struct fe_profile {
    int loaded;
    char path[512];
    char id[32];
    char title[96];
    char subtitle[128];
    char manufacturer[96];
    char year[16];
    char rom_hint[64];            /* the archive file name the frontend searches for */
    double aspect;                 /* 0 = keep the adapter's value */
    int rotation;                  /* degrees, -1 = not given */
    int players;                   /* 0 = not given */
    int integer_scale;             /* -1 = not given */
    int requested[FE_FEAT_COUNT];  /* -1 = not mentioned, 0 = off, 1 = requested */
    char error[256];
} fe_profile;

const char *fe_feature_name(int feature);

/* Parse a profile.  Returns 1 on success; on failure p->error explains why. */
int fe_profile_parse(fe_profile *p, const char *json, size_t size);
int fe_profile_load(fe_profile *p, const char *path);

#ifdef __cplusplus
}
#endif
#endif
