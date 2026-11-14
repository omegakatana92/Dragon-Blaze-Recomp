/* Program table for the generic build (DB_GENERIC_BUILD): no code translated
 * from any ROM is linked, so every instruction runs through the fallback
 * interpreter (sh2_interp.c).  Used for redistributable packages. */
#include "sh2/sh2.h"

void sh2_program_init(void) {}
uint32_t sh2_program_step(sh2_state *s, uint32_t pc) { (void)s; (void)pc; return 0xFFFFFFFFu; }
uint32_t sh2_program_entry_count(void) { return 0; }
int sh2_program_has(uint32_t pc) { (void)pc; return 1; }
void sh2_program_ram_code(uint32_t *base, uint32_t *size, uint32_t *rom_source) { *base = 0; *size = 0; *rom_source = 0; }
