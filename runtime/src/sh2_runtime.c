/* SH-2 runtime: reset, interrupt acceptance, SH7604-specific instructions and
 * the dispatch loop for the statically translated program.  Behaviour follows
 * MAME 0.264 src/devices/cpu/sh/sh2.cpp (interpreter path). */
#include "sh2/sh2.h"

#include <stdio.h>
#include <string.h>

static uint32_t last_missing;
static uint32_t history[64];
static unsigned history_at;

void sh2_reset(sh2_state *s)
{
    uint32_t held = s->held_lines;
    memset(s, 0, sizeof(*s));
    s->held_lines = held;
    s->internal_irq_level = -1;
    s->sr = SH_I;
    s->pc = sh2_rd32(0);
    s->r[15] = sh2_rd32(4);
}

static void exception(sh2_state *s, int irqline, int vector)
{
    s->r[15] -= 4;
    sh2_wr32(s->r[15], s->sr);
    s->r[15] -= 4;
    sh2_wr32(s->r[15], s->pc);
    if (irqline > 15) s->sr |= SH_I;
    else s->sr = (s->sr & ~SH_I) | ((uint32_t)irqline << 4);
    s->pc = sh2_rd32(s->vbr + (uint32_t)vector * 4u);
    if (s->sleep_mode == 1) s->sleep_mode = 2;
}

/* sh2_device::check_pending_irq + sh2_exception */
static void check_pending_irq(sh2_state *s)
{
    if (s->pending_nmi) {
        s->pending_nmi = 0;
        exception(s, 16, 11);
        return;
    }
    int irq = s->internal_irq_level;
    if (s->pending_irq) {
        int ext = 31;
        while (!(s->pending_irq & (1u << ext))) ext--;
        if (ext >= irq) irq = ext;
    }
    if (irq < 0) return;
    if (irq <= (int)((s->sr >> 4) & 15)) return;          /* masked */
    int vector;
    if (s->internal_irq_level == irq) {
        vector = (int)s->internal_irq_vector;
        s->internal_irq_level = -1;
    } else {
        vector = 64 + irq / 2;                              /* autovector */
        if (!(s->held_lines & (1u << irq)))
            s->pending_irq &= ~(1u << irq);   /* HOLD_LINE: cleared on acknowledge */
    }
    exception(s, irq, vector);
}

void sh2_set_irq_line(sh2_state *s, int level, int asserted)
{
    uint32_t bit = 1u << level;
    if (asserted) {
        s->spin = 0;
        if (s->pending_irq & bit) return;
        s->pending_irq |= bit;
        s->test_irq = 1;
    } else {
        s->pending_irq &= ~bit;
    }
}

void sh2_set_internal_irq(sh2_state *s, int level, int vector)
{
    s->internal_irq_level = level;
    s->internal_irq_vector = (uint32_t)vector;
    if (level >= 0) s->test_irq = 1;
}

void sh2_LDCSR(sh2_state *s, uint32_t n)
{
    s->sr = s->r[n] & SH_FLAGS;
    s->test_irq = 1;
}

void sh2_LDCMSR(sh2_state *s, uint32_t n)
{
    s->ea = s->r[n];
    s->sr = sh2_rd32(s->ea) & SH_FLAGS;
    s->r[n] += 4;
    s->cycles += 2;
    s->test_irq = 1;
}

uint32_t sh2_rte_pop(sh2_state *s)
{
    uint32_t pc = sh2_rd32(s->r[15]);
    s->r[15] += 4;
    s->sr = sh2_rd32(s->r[15]) & SH_FLAGS;
    s->r[15] += 4;
    s->cycles += 3;
    s->test_irq = 1;
    return pc;
}

uint32_t sh2_trapa(sh2_state *s, uint32_t imm, uint32_t next_pc)
{
    s->ea = s->vbr + (imm & 0xFF) * 4u;
    s->r[15] -= 4;
    sh2_wr32(s->r[15], s->sr);
    s->r[15] -= 4;
    sh2_wr32(s->r[15], next_pc);
    s->cycles += 7;
    return sh2_rd32(s->ea);
}

uint32_t sh2_illegal(sh2_state *s, uint32_t pc)
{
    s->r[15] -= 4;
    sh2_wr32(s->r[15], s->sr);
    s->r[15] -= 4;
    sh2_wr32(s->r[15], pc);
    s->cycles += 5;
    return sh2_rd32(s->vbr + 4u * 4u);
}

/* SLEEP: MAME re-executes the instruction until an exception has been taken. */
uint32_t sh2_sleep(sh2_state *s, uint32_t pc)
{
    s->cycles += 2;
    uint32_t next = s->sleep_mode == 2 ? pc + 2 : pc;
    if (s->sleep_mode == 0) s->sleep_mode = 1;
    else if (s->sleep_mode == 2) s->sleep_mode = 0;
    return next;
}

uint32_t sh2_unsupported(sh2_state *s, uint32_t pc)
{
    (void)s;
    fprintf(stderr, "SH-2: reached an instruction the translator does not model at %08X\n", pc);
    return 0xFFFFFFFFu;
}

uint32_t sh2_last_missing_pc(void) { return last_missing; }

static int strict_mode;
static uint64_t fallback_instructions;
static uint32_t fallback_entries[256];
static unsigned fallback_entry_count;

void sh2_set_strict(int strict) { strict_mode = strict; }
uint64_t sh2_fallback_instructions(void) { return fallback_instructions; }
unsigned sh2_fallback_entries(const uint32_t **pcs) { if (pcs) *pcs = fallback_entries; return fallback_entry_count; }

static void note_fallback_entry(uint32_t pc)
{
    for (unsigned i = 0; i < fallback_entry_count; i++) if (fallback_entries[i] == pc) return;
    if (fallback_entry_count < 256) fallback_entries[fallback_entry_count++] = pc;
}

uint32_t sh2_ram_code_base, sh2_ram_code_size;
const uint32_t *sh2_ram_code_dirty;

static inline int ram_code_stale(uint32_t pc)
{
    uint32_t off = (pc & 0xC7FFFFFFu) - sh2_ram_code_base;
    if (off >= sh2_ram_code_size) return 0;
    if (sh2_ram_code_dirty[off >> 12]) return 1;
    off += 2;   /* a delay slot may sit on the next page */
    return off < sh2_ram_code_size && sh2_ram_code_dirty[off >> 12];
}

FILE *sh2_trace_file;   /* development: "PC cycles" per executed step */

int sh2_run_until(sh2_state *s, uint64_t target)
{
    int in_fallback = 0;
    while (s->cycles < target) {
        if (s->test_irq) { s->test_irq = 0; check_pending_irq(s); }
        if (s->sleep_mode == 1 || s->spin) { s->cycles = target; return SH2_RUN_OK; }
        history[history_at++ & 63u] = s->pc;
        if (sh2_trace_file) fprintf(sh2_trace_file, "%X %llu\n", s->pc, (unsigned long long)s->cycles);
        uint32_t next = (sh2_ram_code_size && ram_code_stale(s->pc)) ? 0xFFFFFFFEu : sh2_program_step(s, s->pc);
        if (next == 0xFFFFFFFEu) {
            /* RAM code that no longer matches its translation (or is not there yet) */
            next = sh2_interp_step(s, s->pc);
            fallback_instructions++;
            if (next == 0xFFFFFFFFu) { last_missing = s->pc; return SH2_RUN_MISSING; }
            s->pc = next;
            s->instructions++;
            continue;
        }
        if (next == 0xFFFFFFFFu) {
            if (strict_mode) {
                last_missing = s->pc;
                fprintf(stderr, "SH-2: missing translated code at %08X (after %llu instructions)\nlast PCs:",
                        s->pc, (unsigned long long)s->instructions);
                for (unsigned i = 0; i < 16; i++) fprintf(stderr, " %08X", history[(history_at - 1u - i) & 63u]);
                fprintf(stderr, "\n");
                return SH2_RUN_MISSING;
            }
            /* code the static analysis did not reach: interpret it with the same
             * decoder and semantic functions until translated code is reached */
            if (!in_fallback) note_fallback_entry(s->pc);
            in_fallback = 1;
            next = sh2_interp_step(s, s->pc);
            fallback_instructions++;
            if (next == 0xFFFFFFFFu) { last_missing = s->pc; return SH2_RUN_MISSING; }
        } else {
            in_fallback = 0;
        }
        s->pc = next;
        s->instructions++;
    }
    return SH2_RUN_OK;
}
