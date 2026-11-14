/* C interface to the YMF278B (OPL4) sound chip (runtime/src/ymf278b.cpp,
 * built on the ymfm library). Time is counted in chip clocks, which on the
 * PS5 board are also SH-2 clocks (both MASTER_CLOCK / 2). */
#ifndef DB_YMF278B_H
#define DB_YMF278B_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct db_ymf db_ymf;

typedef struct db_ymf_host {
    void *user;
    uint64_t (*now)(void *user);                              /* current time in chip clocks */
    void (*set_timer)(void *user, int tnum, int64_t expire);  /* absolute clock, -1 = stop */
    void (*irq)(void *user, int asserted);
} db_ymf_host;

db_ymf *db_ymf_create(const uint8_t *rom, uint32_t rom_size, const db_ymf_host *host);
void db_ymf_destroy(db_ymf *y);
void db_ymf_reset(db_ymf *y);
uint32_t db_ymf_sample_rate(uint32_t clock);
uint8_t db_ymf_read(db_ymf *y, unsigned offset);
void db_ymf_write(db_ymf *y, unsigned offset, uint8_t data);
void db_ymf_timer_expired(db_ymf *y, int tnum);
void db_ymf_generate(db_ymf *y, int32_t *mono, int samples);
size_t db_ymf_save(db_ymf *y, uint8_t *buffer, size_t capacity);   /* returns the size needed */
int db_ymf_load(db_ymf *y, const uint8_t *buffer, size_t size);

#ifdef __cplusplus
}
#endif
#endif
