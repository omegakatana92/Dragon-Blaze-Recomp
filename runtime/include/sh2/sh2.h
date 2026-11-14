/* SH-2 (Hitachi SH7604) CPU state and runtime for the statically translated
 * program.  The instruction semantics follow MAME 0.264's SH-2 core (see
 * runtime/src/sh2_ops.c); the translator (tools/recompilesh2.py) emits one C
 * function per instruction that updates this state, adds its cycle count and
 * returns the address of the next instruction. */
#ifndef SH2_SH2_H
#define SH2_SH2_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_T 0x00000001u
#define SH_S 0x00000002u
#define SH_I 0x000000f0u
#define SH_Q 0x00000100u
#define SH_M 0x00000200u
#define SH_FLAGS (SH_M | SH_Q | SH_I | SH_S | SH_T)

typedef struct sh2_state {
    uint32_t r[16];
    uint32_t pc, pr, sr, gbr, vbr, mach, macl;
    uint32_t ea;
    uint32_t delay;            /* unused by translated code (kept for the converted ops) */
    uint64_t cycles;           /* CPU clock cycles executed */
    uint64_t instructions;
    uint32_t pending_irq;      /* bit n: external interrupt level n asserted (level-held lines) */
    int32_t internal_irq_level;    /* on-chip module request (-1 = none) */
    uint32_t internal_irq_vector;
    uint8_t pending_nmi;
    uint8_t sleep_mode;        /* 0 run, 1 sleeping, 2 woken by an exception (MAME semantics) */
    uint8_t test_irq;          /* re-evaluate interrupts before the next instruction */
    uint8_t spin;              /* idle until an interrupt line is asserted (MAME spin_until_interrupt) */
    uint32_t held_lines;       /* bit n: line n is level-held (ASSERT_LINE, cleared by the board),
                                  otherwise HOLD_LINE (cleared when the interrupt is taken) */
} sh2_state;

/* Board bus (implemented by the board; addresses are full 32-bit CPU addresses). */
uint8_t sh2_rd8(uint32_t a);
uint16_t sh2_rd16(uint32_t a);
uint32_t sh2_rd32(uint32_t a);
void sh2_wr8(uint32_t a, uint8_t d);
void sh2_wr16(uint32_t a, uint16_t d);
void sh2_wr32(uint32_t a, uint32_t d);

/* Instruction semantics (runtime/src/sh2_ops.c). */
void sh2_ADD(sh2_state *s, uint32_t m, uint32_t n);
void sh2_ADDI(sh2_state *s, uint32_t i, uint32_t n);
void sh2_ADDC(sh2_state *s, uint32_t m, uint32_t n);
void sh2_ADDV(sh2_state *s, uint32_t m, uint32_t n);
void sh2_AND(sh2_state *s, uint32_t m, uint32_t n);
void sh2_ANDI(sh2_state *s, uint32_t i);
void sh2_ANDM(sh2_state *s, uint32_t i);
void sh2_CLRMAC(sh2_state *s);
void sh2_CLRT(sh2_state *s);
void sh2_CMPEQ(sh2_state *s, uint32_t m, uint32_t n);
void sh2_CMPGE(sh2_state *s, uint32_t m, uint32_t n);
void sh2_CMPGT(sh2_state *s, uint32_t m, uint32_t n);
void sh2_CMPHI(sh2_state *s, uint32_t m, uint32_t n);
void sh2_CMPHS(sh2_state *s, uint32_t m, uint32_t n);
void sh2_CMPPL(sh2_state *s, uint32_t n);
void sh2_CMPPZ(sh2_state *s, uint32_t n);
void sh2_CMPSTR(sh2_state *s, uint32_t m, uint32_t n);
void sh2_CMPIM(sh2_state *s, uint32_t i);
void sh2_DIV0S(sh2_state *s, uint32_t m, uint32_t n);
void sh2_DIV0U(sh2_state *s);
void sh2_DIV1(sh2_state *s, uint32_t m, uint32_t n);
void sh2_DMULS(sh2_state *s, uint32_t m, uint32_t n);
void sh2_DMULU(sh2_state *s, uint32_t m, uint32_t n);
void sh2_DT(sh2_state *s, uint32_t n);
void sh2_EXTSB(sh2_state *s, uint32_t m, uint32_t n);
void sh2_EXTSW(sh2_state *s, uint32_t m, uint32_t n);
void sh2_EXTUB(sh2_state *s, uint32_t m, uint32_t n);
void sh2_EXTUW(sh2_state *s, uint32_t m, uint32_t n);
void sh2_LDCGBR(sh2_state *s, uint32_t m);
void sh2_LDCVBR(sh2_state *s, uint32_t m);
void sh2_LDCMGBR(sh2_state *s, uint32_t m);
void sh2_LDCMVBR(sh2_state *s, uint32_t m);
void sh2_LDSMACH(sh2_state *s, uint32_t m);
void sh2_LDSMACL(sh2_state *s, uint32_t m);
void sh2_LDSPR(sh2_state *s, uint32_t m);
void sh2_LDSMMACH(sh2_state *s, uint32_t m);
void sh2_LDSMMACL(sh2_state *s, uint32_t m);
void sh2_LDSMPR(sh2_state *s, uint32_t m);
void sh2_MAC_L(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MAC_W(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOV(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVBS(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVWS(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVLS(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVBL(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVWL(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVLL(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVBM(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVWM(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVLM(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVBP(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVWP(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVLP(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVBS0(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVWS0(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVLS0(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVBL0(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVWL0(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVLL0(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MOVI(sh2_state *s, uint32_t i, uint32_t n);
void sh2_MOVWI(sh2_state *s, uint32_t d, uint32_t n);
void sh2_MOVLI(sh2_state *s, uint32_t d, uint32_t n);
void sh2_MOVBLG(sh2_state *s, uint32_t d);
void sh2_MOVWLG(sh2_state *s, uint32_t d);
void sh2_MOVLLG(sh2_state *s, uint32_t d);
void sh2_MOVBSG(sh2_state *s, uint32_t d);
void sh2_MOVWSG(sh2_state *s, uint32_t d);
void sh2_MOVLSG(sh2_state *s, uint32_t d);
void sh2_MOVBS4(sh2_state *s, uint32_t d, uint32_t n);
void sh2_MOVWS4(sh2_state *s, uint32_t d, uint32_t n);
void sh2_MOVLS4(sh2_state *s, uint32_t m, uint32_t d, uint32_t n);
void sh2_MOVBL4(sh2_state *s, uint32_t m, uint32_t d);
void sh2_MOVWL4(sh2_state *s, uint32_t m, uint32_t d);
void sh2_MOVLL4(sh2_state *s, uint32_t m, uint32_t d, uint32_t n);
void sh2_MOVA(sh2_state *s, uint32_t d);
void sh2_MOVT(sh2_state *s, uint32_t n);
void sh2_MULL(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MULS(sh2_state *s, uint32_t m, uint32_t n);
void sh2_MULU(sh2_state *s, uint32_t m, uint32_t n);
void sh2_NEG(sh2_state *s, uint32_t m, uint32_t n);
void sh2_NEGC(sh2_state *s, uint32_t m, uint32_t n);
void sh2_NOP(sh2_state *s);
void sh2_NOT(sh2_state *s, uint32_t m, uint32_t n);
void sh2_OR(sh2_state *s, uint32_t m, uint32_t n);
void sh2_ORI(sh2_state *s, uint32_t i);
void sh2_ORM(sh2_state *s, uint32_t i);
void sh2_ROTCL(sh2_state *s, uint32_t n);
void sh2_ROTCR(sh2_state *s, uint32_t n);
void sh2_ROTL(sh2_state *s, uint32_t n);
void sh2_ROTR(sh2_state *s, uint32_t n);
void sh2_SETT(sh2_state *s);
void sh2_SHAL(sh2_state *s, uint32_t n);
void sh2_SHAR(sh2_state *s, uint32_t n);
void sh2_SHLL(sh2_state *s, uint32_t n);
void sh2_SHLL2(sh2_state *s, uint32_t n);
void sh2_SHLL8(sh2_state *s, uint32_t n);
void sh2_SHLL16(sh2_state *s, uint32_t n);
void sh2_SHLR(sh2_state *s, uint32_t n);
void sh2_SHLR2(sh2_state *s, uint32_t n);
void sh2_SHLR8(sh2_state *s, uint32_t n);
void sh2_SHLR16(sh2_state *s, uint32_t n);
void sh2_STCSR(sh2_state *s, uint32_t n);
void sh2_STCGBR(sh2_state *s, uint32_t n);
void sh2_STCVBR(sh2_state *s, uint32_t n);
void sh2_STCMSR(sh2_state *s, uint32_t n);
void sh2_STCMGBR(sh2_state *s, uint32_t n);
void sh2_STCMVBR(sh2_state *s, uint32_t n);
void sh2_STSMACH(sh2_state *s, uint32_t n);
void sh2_STSMACL(sh2_state *s, uint32_t n);
void sh2_STSPR(sh2_state *s, uint32_t n);
void sh2_STSMMACH(sh2_state *s, uint32_t n);
void sh2_STSMMACL(sh2_state *s, uint32_t n);
void sh2_STSMPR(sh2_state *s, uint32_t n);
void sh2_SUB(sh2_state *s, uint32_t m, uint32_t n);
void sh2_SUBC(sh2_state *s, uint32_t m, uint32_t n);
void sh2_SUBV(sh2_state *s, uint32_t m, uint32_t n);
void sh2_SWAPB(sh2_state *s, uint32_t m, uint32_t n);
void sh2_SWAPW(sh2_state *s, uint32_t m, uint32_t n);
void sh2_TAS(sh2_state *s, uint32_t n);
void sh2_TST(sh2_state *s, uint32_t m, uint32_t n);
void sh2_TSTI(sh2_state *s, uint32_t i);
void sh2_TSTM(sh2_state *s, uint32_t i);
void sh2_XOR(sh2_state *s, uint32_t m, uint32_t n);
void sh2_XORI(sh2_state *s, uint32_t i);
void sh2_XORM(sh2_state *s, uint32_t i);
void sh2_XTRCT(sh2_state *s, uint32_t m, uint32_t n);

/* SH7604-specific instructions and exceptions (runtime/src/sh2_runtime.c). */
void sh2_LDCSR(sh2_state *s, uint32_t n);
void sh2_LDCMSR(sh2_state *s, uint32_t n);
uint32_t sh2_rte_pop(sh2_state *s);                       /* pops PC and SR, returns the PC */
uint32_t sh2_trapa(sh2_state *s, uint32_t imm, uint32_t next_pc);
uint32_t sh2_illegal(sh2_state *s, uint32_t pc);
uint32_t sh2_sleep(sh2_state *s, uint32_t pc);
uint32_t sh2_unsupported(sh2_state *s, uint32_t pc);

/* Runtime. */
void sh2_reset(sh2_state *s);
void sh2_set_irq_line(sh2_state *s, int level, int asserted);
void sh2_set_internal_irq(sh2_state *s, int level, int vector);   /* level -1 clears */
#define SH2_RUN_OK 0
#define SH2_RUN_MISSING (-1)
int sh2_run_until(sh2_state *s, uint64_t target_cycles);
uint32_t sh2_last_missing_pc(void);
extern FILE *sh2_trace_file;   /* development trace */
/* Fallback interpreter (sh2_interp.c) for code the static analysis missed.
 * Strict mode (development) stops at such code instead. */
uint32_t sh2_interp_step(sh2_state *s, uint32_t pc);
void sh2_set_strict(int strict);
uint64_t sh2_fallback_instructions(void);
unsigned sh2_fallback_entries(const uint32_t **pcs);

/* Generated program (build/generated). */
void sh2_program_init(void);
uint32_t sh2_program_step(sh2_state *s, uint32_t pc);
uint32_t sh2_program_entry_count(void);
int sh2_program_has(uint32_t pc);
void sh2_program_ram_code(uint32_t *base, uint32_t *size, uint32_t *rom_source);

/* Translated code located in RAM (copied there from ROM by the program) is
 * only run while its page still holds exactly the bytes it was translated
 * from; the board keeps a count of differing 32-bit words per 4 KiB page.
 * Anything else in such a page runs on the fallback interpreter. */
extern uint32_t sh2_ram_code_base, sh2_ram_code_size;
extern const uint32_t *sh2_ram_code_dirty;

#ifdef __cplusplus
}
#endif
#endif
