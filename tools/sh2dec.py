"""SH-2 (SH7604) instruction decoder for the static translator.

Decodes one 16-bit opcode into the name of the runtime semantic function
(runtime/src/sh2_ops.c, converted from MAME's sh.cpp), its arguments, and its
control-flow kind.  The opcode map follows MAME 0.264 sh_common_execution::
execute_one() for the SH-2 (SH-4-only opcodes decode as illegal).
"""
from __future__ import annotations

from dataclasses import dataclass, field

# control-flow kinds
OP = "op"            # falls through
PCREL = "pcrel"      # falls through, reads PC (MOV.W/L @(disp,PC), MOVA)
BT, BF, BTS, BFS = "bt", "bf", "bts", "bfs"          # conditional (S = delayed)
BRA, BSR, BRAF, BSRF, JMP, JSR, RTS, RTE = "bra", "bsr", "braf", "bsrf", "jmp", "jsr", "rts", "rte"
TRAPA, SLEEP, ILLEGAL, LDCSR = "trapa", "sleep", "illegal", "ldcsr"

DELAYED = {BTS, BFS, BRA, BSR, BRAF, BSRF, JMP, JSR, RTS, RTE}
ENDS_BLOCK = {BRA, BRAF, JMP, RTS, RTE, TRAPA, ILLEGAL}


@dataclass
class Ins:
    address: int
    opcode: int
    name: str                 # semantic function (sh2_<name>) or a flow mnemonic
    args: tuple = ()
    kind: str = OP
    target: int | None = None # static branch target
    text: str = ""


def s8(v: int) -> int:
    return v - 0x100 if v & 0x80 else v


def s12(v: int) -> int:
    return v - 0x1000 if v & 0x800 else v


def decode(op: int, address: int) -> Ins:
    n = (op >> 8) & 15
    m = (op >> 4) & 15
    i8 = op & 0xFF
    top = op >> 12
    I = lambda name, *a, kind=OP, target=None: Ins(address, op, name, tuple(a), kind, target)

    if top == 0:
        low = op & 0x3F
        lo4 = op & 0x0F
        if lo4 == 0x4: return I("MOVBS0", m, n)
        if lo4 == 0x5: return I("MOVWS0", m, n)
        if lo4 == 0x6: return I("MOVLS0", m, n)
        if lo4 == 0x7: return I("MULL", m, n)
        if lo4 == 0xC: return I("MOVBL0", m, n)
        if lo4 == 0xD: return I("MOVWL0", m, n)
        if lo4 == 0xE: return I("MOVLL0", m, n)
        if lo4 == 0xF: return I("MAC_L", m, n)
        if low == 0x02: return I("STCSR", n)
        if low == 0x03: return I("BSRF", n, kind=BSRF)
        if low == 0x08 and op == 0x0008: return I("CLRT")
        if low == 0x09 and op == 0x0009: return I("NOP")
        if low == 0x0A: return I("STSMACH", n)
        if low == 0x0B and op == 0x000B: return I("RTS", kind=RTS)
        if low == 0x12: return I("STCGBR", n)
        if low == 0x18 and op == 0x0018: return I("SETT")
        if low == 0x19 and op == 0x0019: return I("DIV0U")
        if low == 0x1A: return I("STSMACL", n)
        if low == 0x1B and op == 0x001B: return I("SLEEP", kind=SLEEP)
        if low == 0x22: return I("STCVBR", n)
        if low == 0x23: return I("BRAF", n, kind=BRAF)
        if low == 0x28 and op == 0x0028: return I("CLRMAC")
        if low == 0x29: return I("MOVT", n)
        if low == 0x2A: return I("STSPR", n)
        if low == 0x2B and op == 0x002B: return I("RTE", kind=RTE)
        # MAME accepts the register fields as don't-care for some of these;
        # be equally lenient so behaviour matches
        if low == 0x08: return I("CLRT")
        if low == 0x09: return I("NOP")
        if low == 0x0B: return I("RTS", kind=RTS)
        if low == 0x18: return I("SETT")
        if low == 0x19: return I("DIV0U")
        if low == 0x1B: return I("SLEEP", kind=SLEEP)
        if low == 0x28: return I("CLRMAC")
        if low == 0x2B: return I("RTE", kind=RTE)
        return I("ILLEGAL", kind=ILLEGAL)
    if top == 1:
        return I("MOVLS4", m, op & 15, n)
    if top == 2:
        t = ["MOVBS", "MOVWS", "MOVLS", None, "MOVBM", "MOVWM", "MOVLM", "DIV0S",
             "TST", "AND", "XOR", "OR", "CMPSTR", "XTRCT", "MULU", "MULS"][op & 15]
        return I(t, m, n) if t else I("ILLEGAL", kind=ILLEGAL)
    if top == 3:
        t = ["CMPEQ", None, "CMPHS", "CMPGE", "DIV1", "DMULU", "CMPHI", "CMPGT",
             "SUB", None, "SUBC", "SUBV", "ADD", "DMULS", "ADDC", "ADDV"][op & 15]
        return I(t, m, n) if t else I("ILLEGAL", kind=ILLEGAL)
    if top == 4:
        low = op & 0x3F
        if (op & 0x0F) == 0x0F: return I("MAC_W", m, n)
        t = {0x00: "SHLL", 0x01: "SHLR", 0x02: "STSMMACH", 0x03: "STCMSR", 0x04: "ROTL", 0x05: "ROTR",
             0x06: "LDSMMACH", 0x08: "SHLL2", 0x09: "SHLR2", 0x0A: "LDSMACH",
             0x10: "DT", 0x11: "CMPPZ", 0x12: "STSMMACL", 0x13: "STCMGBR", 0x15: "CMPPL", 0x16: "LDSMMACL",
             0x17: "LDCMGBR", 0x18: "SHLL8", 0x19: "SHLR8", 0x1A: "LDSMACL", 0x1B: "TAS", 0x1E: "LDCGBR",
             0x20: "SHAL", 0x21: "SHAR", 0x22: "STSMPR", 0x23: "STCMVBR", 0x24: "ROTCL", 0x25: "ROTCR",
             0x26: "LDSMPR", 0x27: "LDCMVBR", 0x28: "SHLL16", 0x29: "SHLR16", 0x2A: "LDSPR",
             0x2E: "LDCVBR"}.get(low)
        if t: return I(t, n)
        if low == 0x07: return I("LDCMSR", n, kind=LDCSR)
        if low == 0x0E: return I("LDCSR", n, kind=LDCSR)
        if low == 0x0B: return I("JSR", n, kind=JSR)
        if low == 0x2B: return I("JMP", n, kind=JMP)
        return I("ILLEGAL", kind=ILLEGAL)
    if top == 5:
        return I("MOVLL4", m, op & 15, n)
    if top == 6:
        t = ["MOVBL", "MOVWL", "MOVLL", "MOV", "MOVBP", "MOVWP", "MOVLP", "NOT",
             "SWAPB", "SWAPW", "NEGC", "NEG", "EXTUB", "EXTUW", "EXTSB", "EXTSW"][op & 15]
        return I(t, m, n)
    if top == 7:
        return I("ADDI", i8, n)
    if top == 8:
        sub = (op >> 8) & 15
        if sub == 0: return I("MOVBS4", op & 15, m)
        if sub == 1: return I("MOVWS4", op & 15, m)
        if sub == 4: return I("MOVBL4", m, op & 15)
        if sub == 5: return I("MOVWL4", m, op & 15)
        if sub == 8: return I("CMPIM", i8)
        tgt = (address + 4 + s8(i8) * 2) & 0xFFFFFFFF
        if sub == 9: return I("BT", kind=BT, target=tgt)
        if sub == 11: return I("BF", kind=BF, target=tgt)
        if sub == 13: return I("BTS", kind=BTS, target=tgt)
        if sub == 15: return I("BFS", kind=BFS, target=tgt)
        return I("ILLEGAL", kind=ILLEGAL)
    if top == 9:
        return I("MOVWI", i8, n, kind=PCREL)
    if top == 10:
        return I("BRA", kind=BRA, target=(address + 4 + s12(op & 0xFFF) * 2) & 0xFFFFFFFF)
    if top == 11:
        return I("BSR", kind=BSR, target=(address + 4 + s12(op & 0xFFF) * 2) & 0xFFFFFFFF)
    if top == 12:
        sub = (op >> 8) & 15
        if sub == 3: return I("TRAPA", i8, kind=TRAPA)
        if sub == 7: return I("MOVA", i8, kind=PCREL)
        t = ["MOVBSG", "MOVWSG", "MOVLSG", None, "MOVBLG", "MOVWLG", "MOVLLG", None,
             "TSTI", "ANDI", "XORI", "ORI", "TSTM", "ANDM", "XORM", "ORM"][sub]
        return I(t, i8)
    if top == 13:
        return I("MOVLI", i8, n, kind=PCREL)
    if top == 14:
        return I("MOVI", i8, n)
    return I("ILLEGAL", kind=ILLEGAL)


def text(ins: Ins) -> str:
    if ins.target is not None:
        return f"{ins.name} ${ins.target:08X}"
    return f"{ins.name} " + ",".join(str(a) for a in ins.args) if ins.args else ins.name
