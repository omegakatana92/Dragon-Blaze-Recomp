/* Save-state serialization for the Dragon Blaze native runtime.
 *
 * A core state is a tagged, versioned, CRC-protected little-endian blob with
 * every piece of mutable board state: the SH-2 (registers, interrupt and sleep
 * latches, cycle counters), the SH7604 on-chip registers (DIVU, FRT), main
 * RAM, sprite/palette/zoom RAM and video registers, the latched sprite list
 * and the sprite assembly buffer, the last picture, the EEPROM (contents and
 * serial interface), the YMF278B (ymfm's own save format plus its timers and
 * busy flag) and the scheduler counters.  ROM-derived data is never stored;
 * the state carries a fingerprint of the ROM set instead and is rejected when
 * it does not match. */
#ifndef DB_STATE_H
#define DB_STATE_H

#include <stddef.h>
#include <stdint.h>
#include "db/board.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DB_STATE_MAGIC "DBLZCORE"
#define DB_STATE_VERSION 1u

uint32_t db_crc32(uint32_t crc, const void *data, size_t size);
uint8_t *db_state_save(const db_board *board, size_t *size);
int db_state_validate(const db_board *board, const uint8_t *data, size_t size, char *err, size_t err_size);
int db_state_load(db_board *board, const uint8_t *data, size_t size, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
#endif
