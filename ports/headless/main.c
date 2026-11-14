/* Deterministic headless runner for Dragon Blaze: runs the translated
 * program on the board model and writes hashes, PNG, WAV and memory dumps. */
#include "db/board.h"
#include "db/state.h"
#include "miniz.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fnv1a(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

static int write_png(const char *path, const uint32_t *px, int w, int h)
{
    uint8_t *r = (uint8_t *)malloc((size_t)w * (size_t)h * 3);
    if (!r) return 0;
    for (int i = 0; i < w * h; i++) {
        r[i * 3] = (uint8_t)(px[i] >> 16);
        r[i * 3 + 1] = (uint8_t)(px[i] >> 8);
        r[i * 3 + 2] = (uint8_t)px[i];
    }
    size_t zl = 0;
    void *z = tdefl_write_image_to_png_file_in_memory(r, w, h, 3, &zl);
    free(r);
    if (!z) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) { mz_free(z); return 0; }
    fwrite(z, 1, zl, f);
    fclose(f);
    mz_free(z);
    return 1;
}

/* work RAM as the CPU sees it (big-endian bytes) */
static void ram_bytes(const db_board *b, uint8_t *out)
{
    for (size_t i = 0; i < sizeof(b->ram) / 4; i++) {
        uint32_t v = b->ram[i];
        out[i * 4] = (uint8_t)(v >> 24); out[i * 4 + 1] = (uint8_t)(v >> 16);
        out[i * 4 + 2] = (uint8_t)(v >> 8); out[i * 4 + 3] = (uint8_t)v;
    }
}

static uint32_t script_inputs(const char *script, int frame, uint32_t *rng, uint32_t *held)
{
    uint32_t in = 0;
    if (!script || !strcmp(script, "none")) return 0;
    if (!strncmp(script, "hold:", 5)) return (uint32_t)strtoul(script + 5, NULL, 0);
    if (!strncmp(script, "random:", 7)) {
        if (frame == 0) *rng = (uint32_t)strtoul(script + 7, NULL, 0) * 2654435761u + 1u;
        int t = frame % 900;
        if (frame > 600 && t < 5) in |= DB_IN_COIN1;
        if (frame > 600 && t >= 450 && t < 455) in |= DB_IN_COIN2;
        if (frame > 600 && t >= 20 && t < 26) in |= DB_IN_P1_START;
        if (frame > 600 && t >= 470 && t < 476) in |= DB_IN_P2_START;
        if (frame % 6 == 0) { *rng = *rng * 1103515245u + 12345u; *held = (*rng >> 8) & 0x7F7Fu; }
        return in | *held;
    }
    if (!strncmp(script, "service:", 8)) {
        /* enter the service menu, then wander through it with random input */
        if (frame == 0) *rng = (uint32_t)strtoul(script + 8, NULL, 0) * 2654435761u + 7u;
        if (frame >= 600 && frame < 606) return DB_IN_TEST;
        if (frame % 10 == 0) {
            *rng = *rng * 1103515245u + 12345u;
            uint32_t r = *rng >> 8;
            static const uint32_t pick[] = { DB_IN_P1_UP, DB_IN_P1_DOWN, DB_IN_P1_LEFT, DB_IN_P1_RIGHT, DB_IN_P1_B1,
                                             DB_IN_P1_B2, DB_IN_P1_START, DB_IN_P1_DOWN, DB_IN_P1_B1, DB_IN_TEST };
            *held = (r % 7 == 0) ? pick[(r >> 4) % 10] : 0;
        }
        return frame % 10 < 3 ? *held : 0;
    }
    if (!strcmp(script, "demo")) {
        /* fixed sequence also played into MAME by the reference script
         * (tools/mame/demo_inputs.lua); MAME latches a value set from a frame
         * notifier at the next frame, hence the one-frame shift */
        frame -= 1;
        if (frame >= 1500 && frame < 1505) in |= DB_IN_COIN1;
        if (frame >= 1600 && frame < 1606) in |= DB_IN_P1_START;
        if (frame >= 1700) {
            in |= ((frame / 60) % 2) ? DB_IN_P1_RIGHT : DB_IN_P1_LEFT;
            if ((frame / 45) % 3 == 0) in |= DB_IN_P1_UP;
            if (frame % 8 < 4) in |= DB_IN_P1_B1;
            if (frame % 600 >= 300 && frame % 600 < 303) in |= DB_IN_P1_B2;
        }
        return in;
    }
    /* "auto": insert a coin, start, move and fire */
    int t = frame % 600;
    if (frame > 900 && t < 5) in |= DB_IN_COIN1;
    if (frame > 900 && t >= 30 && t < 36) in |= DB_IN_P1_START;
    if (frame > 1000) {
        in |= ((frame / 90) % 2) ? DB_IN_P1_RIGHT : DB_IN_P1_LEFT;
        if (frame % 12 < 3) in |= DB_IN_P1_B1;
        if (frame % 97 < 2) in |= DB_IN_P1_B2;
    }
    return in;
}

int main(int argc, char **argv)
{
    const char *rom = NULL, *png = NULL, *dump_ram = NULL, *wav = NULL, *hashes = NULL, *script = NULL;
    const char *nv_in = NULL, *nv_out = NULL, *dump_video = NULL, *ram_at = NULL;
    int frames = 600, state_test = 0, state_test_len = 120, png_every = 0, debug_dip = 0;
    long region = -1;
    uint32_t watch[8]; int nwatch = 0; FILE *wfile = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nvram-in") && i + 1 < argc) nv_in = argv[++i];
        else if (!strcmp(argv[i], "--nvram-out") && i + 1 < argc) nv_out = argv[++i];
        else if (!strcmp(argv[i], "--strict")) sh2_set_strict(1);
        else if (!strcmp(argv[i], "--region") && i + 1 < argc) region = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--debug-dip")) debug_dip = 1;
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc && nwatch < 8) watch[nwatch++] = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--watch-file") && i + 1 < argc) wfile = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--png-every") && i + 1 < argc) png_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-ram") && i + 1 < argc) dump_ram = argv[++i];
        else if (!strcmp(argv[i], "--dump-ram-at") && i + 1 < argc) ram_at = argv[++i];
        else if (!strcmp(argv[i], "--dump-video") && i + 1 < argc) dump_video = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--hashes") && i + 1 < argc) hashes = argv[++i];
        else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--state-test") && i + 1 < argc) state_test = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--state-test-len") && i + 1 < argc) state_test_len = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s --rom dragnblz.zip [--frames N] [--png FILE] [--png-every N]\n"
                            "          [--dump-ram FILE] [--dump-ram-at PREFIX:F1,F2,...] [--wav FILE] [--hashes FILE]\n"
                            "          [--inputs none|auto|random:N|hold:MASK] [--state-test FRAME] [--state-test-len N]\n"
                            "          [--region 0xNN000000] [--debug-dip] [--nvram-in FILE] [--nvram-out FILE] [--strict]\n", argv[0]);
            return 2;
        }
    }
    if (!rom) rom = getenv("DRAGNBLZ_ROM");
    if (!rom) { fprintf(stderr, "error: --rom is required\n"); return 2; }

    static db_board board;
    sh2_program_init();
    if (!db_board_load(&board, rom)) { fprintf(stderr, "error: %s\n", board.error); return 1; }
    if (region >= 0) board.region = (uint32_t)region;
    board.debug_dip = (uint8_t)debug_dip;
    if (nv_in) {
        FILE *f = fopen(nv_in, "rb");
        if (f) { size_t r = fread(board.eeprom.data, 1, sizeof(board.eeprom.data), f); (void)r; fclose(f); }
    }
    db_board_reset(&board);
    printf("ROM: %s\nprogram: reset PC %08X, %u SH-2 entries\n", rom, board.cpu.pc, sh2_program_entry_count());

    /* --dump-ram-at PREFIX:F1,F2: RAM dumps after the listed frames */
    char ram_prefix[512] = "";
    int ram_frames[64], ram_nframes = 0;
    if (ram_at) {
        const char *c = strrchr(ram_at, ':');
        if (c) {
            snprintf(ram_prefix, sizeof(ram_prefix), "%.*s", (int)(c - ram_at), ram_at);
            for (const char *p = c + 1; *p && ram_nframes < 64;) {
                ram_frames[ram_nframes++] = atoi(p);
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
    }

    uint32_t *pixels = (uint32_t *)calloc(DB_SCREEN_W * DB_SCREEN_H, 4);
    static uint8_t rambuf[0x100000];
    static int16_t audio[DB_MAX_FRAME_SAMPLES];
    FILE *wf = wav ? fopen(wav, "wb") : NULL;
    uint32_t wav_frames = 0;
    if (wf) { uint8_t h[44] = { 0 }; fwrite(h, 1, 44, wf); }
    FILE *hf = hashes ? fopen(hashes, "w") : NULL;
    uint8_t *saved = NULL; size_t saved_size = 0;
    uint64_t *th = state_test ? (uint64_t *)calloc((size_t)state_test_len, 8) : NULL;
    int tphase = 0, tindex = 0, tmismatch = -1;
    int failed = 0;
    uint32_t rng = 1, held = 0, saved_rng = 0, saved_held = 0;

    const char *trace_path = getenv("DB_TRACE_INSN");
    int trace_from = getenv("TR_FROM") ? atoi(getenv("TR_FROM")) : 1, trace_to = getenv("TR_TO") ? atoi(getenv("TR_TO")) : 2;
    for (int frame = 0; frame < frames; frame++) {
        if (trace_path && frame + 1 == trace_from) sh2_trace_file = fopen(trace_path, "w");
        if (trace_path && frame + 1 == trace_to && sh2_trace_file) { fclose(sh2_trace_file); sh2_trace_file = NULL; }
        if (state_test && frame == state_test && tphase == 0) {
            saved = db_state_save(&board, &saved_size);
            saved_rng = rng; saved_held = held;
            printf("state-test: saved %zu bytes at frame %d\n", saved_size, frame);
            tphase = 1; tindex = 0;
        }
        if (script) db_board_set_inputs(&board, script_inputs(script, frame, &rng, &held));
        int n = 0;
        int rc = db_board_run_frame(&board, audio, &n);
        if (wf) { fwrite(audio, 2, (size_t)n, wf); wav_frames += (uint32_t)n; }
        if (rc != DB_RUN_OK) {
            fprintf(stderr, "stopped at frame %d: SH-2 reached untranslated code at %08X\n", frame, sh2_last_missing_pc());
            failed = 1;
            break;
        }
        if (tphase == 1 || tphase == 2) {
            uint64_t h = fnv1a(0xcbf29ce484222325ull, board.ram, sizeof(board.ram));
            h = fnv1a(h, audio, (size_t)n * 2);
            h = fnv1a(h, board.screen, sizeof(board.screen));
            h = fnv1a(h, board.spriteram, sizeof(board.spriteram));
            h = fnv1a(h, board.eeprom.data, sizeof(board.eeprom.data));
            if (getenv("DB_STATE_DEBUG") && tindex < 3)
                printf("  [%d] ram %016llx audio %016llx (%d) screen %016llx sprite %016llx\n", tindex,
                       (unsigned long long)fnv1a(0xcbf29ce484222325ull, board.ram, sizeof(board.ram)),
                       (unsigned long long)fnv1a(0xcbf29ce484222325ull, audio, (size_t)n * 2), n,
                       (unsigned long long)fnv1a(0xcbf29ce484222325ull, board.screen, sizeof(board.screen)),
                       (unsigned long long)fnv1a(0xcbf29ce484222325ull, board.spriteram, sizeof(board.spriteram)));
            if (tphase == 1) th[tindex] = h; else if (th[tindex] != h && tmismatch < 0) tmismatch = tindex;
            if (++tindex == state_test_len) {
                if (tphase == 1) {
                    char err[200];
                    uint8_t *bad = (uint8_t *)malloc(saved_size);
                    memcpy(bad, saved, saved_size);
                    bad[saved_size / 2] ^= 0x5A;
                    uint32_t pc = board.cpu.pc;
                    int rej = !db_state_load(&board, bad, saved_size, err, sizeof(err));
                    printf("state-test: corrupted state %s (%s), board %s\n", rej ? "rejected" : "ACCEPTED", err,
                           board.cpu.pc == pc ? "untouched" : "MODIFIED");
                    rej = !db_state_load(&board, bad, saved_size / 3, err, sizeof(err));
                    printf("state-test: truncated state %s (%s)\n", rej ? "rejected" : "ACCEPTED", err);
                    free(bad);
                    if (!db_state_load(&board, saved, saved_size, err, sizeof(err))) { printf("state-test: load FAILED: %s\n", err); failed = 1; break; }
                    rng = saved_rng; held = saved_held;
                    { size_t rs = 0; uint8_t *re = db_state_save(&board, &rs);
                      size_t first = (size_t)-1; for (size_t q = 0; re && q < rs && q < saved_size; q++) if (re[q] != saved[q]) { first = q; break; }
                      printf("state-test: re-save %s (%zu bytes, first difference at %lld)\n", re && rs == saved_size && first == (size_t)-1 ? "identical" : "DIFFERS", rs, first == (size_t)-1 ? -1LL : (long long)first);
                      free(re); }
                    printf("state-test: restored, replaying %d frames\n", state_test_len);
                    tphase = 2; tindex = 0; frame = state_test - 1;
                    continue;
                }
                if (tmismatch < 0) printf("state-test: PASS, %d frames identical after load\n", state_test_len);
                else { printf("state-test: FAIL at +%d\n", tmismatch); failed = 1; }
                tphase = 3;
            }
        }
        for (int k = 0; k < ram_nframes; k++) {
            if (ram_frames[k] != frame + 1) continue;
            char p[600];
            snprintf(p, sizeof(p), "%s%d.bin", ram_prefix, frame + 1);
            ram_bytes(&board, rambuf);
            FILE *f = fopen(p, "wb");
            if (f) { fwrite(rambuf, 1, sizeof(rambuf), f); fclose(f); }
            snprintf(p, sizeof(p), "%s%d.png", ram_prefix, frame + 1);
            db_video_render(&board, pixels);
            write_png(p, pixels, DB_SCREEN_W, DB_SCREEN_H);
        }
        if (nwatch && wfile) {
            fprintf(wfile, "%d", frame + 1);
            for (int k = 0; k < nwatch; k++) fprintf(wfile, " %08X", board.ram[((watch[k] - 0x06000000u) & 0xFFFFCu) >> 2]);
            fprintf(wfile, "\n");
        }
        uint64_t hr = fnv1a(0xcbf29ce484222325ull, board.ram, sizeof(board.ram));
        if (hf) fprintf(hf, "%d %08X %016" PRIx64 " %08X %08X\n", frame + 1, board.cpu.pc, hr, board.cpu.r[0], board.cpu.r[15]);
        if (frame % 300 == 0) printf("frame %5d pc %08X ram %016" PRIx64 "\n", frame, board.cpu.pc, hr);
        if (png_every && png && frame % png_every == png_every - 1) {
            char p[1024];
            snprintf(p, sizeof(p), "%s_%05d.png", png, frame + 1);
            db_video_render(&board, pixels);
            write_png(p, pixels, DB_SCREEN_W, DB_SCREEN_H);
        }
    }
    if (!failed && png && !png_every) { db_video_render(&board, pixels); write_png(png, pixels, DB_SCREEN_W, DB_SCREEN_H); }
    if (dump_video) {
        FILE *f = fopen(dump_video, "wb");
        if (f) {
            fwrite(board.spriteram, 1, sizeof(board.spriteram), f);
            fwrite(board.paletteram, 1, sizeof(board.paletteram), f);
            fwrite(board.zoomram, 1, sizeof(board.zoomram), f);
            fwrite(board.vidregs, 1, sizeof(board.vidregs), f);
            fclose(f);
        }
    }
    if (nv_out) { FILE *f = fopen(nv_out, "wb"); if (f) { fwrite(board.eeprom.data, 1, sizeof(board.eeprom.data), f); fclose(f); } }
    if (dump_ram) { ram_bytes(&board, rambuf); FILE *f = fopen(dump_ram, "wb"); if (f) { fwrite(rambuf, 1, sizeof(rambuf), f); fclose(f); } }
    if (wf) {
        uint32_t rate = DB_AUDIO_RATE, data = wav_frames * 2u, riff = 36 + data, br = rate * 2;
        uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0 };
        memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4);
        h[32] = 2; h[34] = 16; memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
        fseek(wf, 0, SEEK_SET); fwrite(h, 1, 44, wf); fclose(wf);
    }
    if (hf) fclose(hf);
    if (wfile) fclose(wfile);
    {
        const uint32_t *pcs; unsigned np = sh2_fallback_entries(&pcs);
        printf("fallback: %" PRIu64 " interpreted instructions, %u entry points", sh2_fallback_instructions(), np);
        for (unsigned i = 0; i < np; i++) printf(" %08X", pcs[i]);
        printf("\n");
    }
    {
        uint32_t words = 0, pages = 0;
        for (size_t i = 0; i < sizeof(board.ram_code_dirty) / sizeof(board.ram_code_dirty[0]); i++)
            if (board.ram_code_dirty[i]) { words += board.ram_code_dirty[i]; pages++; }
        if (board.ram_code_size) printf("RAM code: %u words in %u pages differ from the translated copy\n", words, pages);
    }
    printf("CPU: %" PRIu64 " SH-2 instructions, %" PRIu64 " cycles\n", board.cpu.instructions, board.cpu.cycles);
    free(saved); free(th); free(pixels);
    db_board_free(&board);
    return failed;
}
