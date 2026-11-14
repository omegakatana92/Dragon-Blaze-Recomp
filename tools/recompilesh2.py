"""Static translation of the Dragon Blaze SH-2 program (Psikyo PS5 board,
program ROM at 0x00000000) into C.

Every instruction reachable from the vector tables, from seeds
(tools/sh2_seeds.txt) and from code addresses found in literal pools becomes
one C function `b_XXXXXXXX(sh2_state *s)` that performs it (through the
semantic functions of runtime/src/sh2_ops.c, converted from MAME's SH core),
adds its cycle count and returns the address of the next instruction.  A
delayed branch and its delay-slot instruction are emitted together, so an
interrupt can never be taken between them.  Nothing is interpreted at run time:
an address without a function stops the game with a message.

usage: recompilesh2.py --game dragnblz.zip [--out DIR]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import rom_set  # noqa: E402
import sh2dec as D  # noqa: E402

AM = 0xC7FFFFFF


def canonical(a: int) -> int:
    a &= 0xFFFFFFFF
    if (a & 0xF0000000) in (0x00000000, 0x20000000):
        a &= AM
    return a


class Image:
    def __init__(self, regions: list[tuple[int, bytes]]):
        self.regions = regions

    def word(self, a: int) -> int | None:
        a = canonical(a)
        for base, data in self.regions:
            if base <= a < base + len(data) - 1:
                o = a - base
                return (data[o] << 8) | data[o + 1]
        return None

    def long(self, a: int) -> int | None:
        hi, lo = self.word(a), self.word(a + 2)
        if hi is None or lo is None:
            return None
        return (hi << 16) | lo

    def contains(self, a: int) -> bool:
        return self.word(a) is not None


def walk(img: Image, seeds: list[int]):
    seen: dict[int, D.Ins] = {}
    literal_targets: set[int] = set()
    work = [canonical(x) for x in seeds if x is not None]
    while work:
        a = work.pop()
        while True:
            if a in seen or a & 1:
                break
            op = img.word(a)
            if op is None:
                break
            ins = D.decode(op, a)
            seen[a] = ins
            k = ins.kind
            if k == D.PCREL and ins.name == "MOVLI":
                ea = ((a + 4) & ~3) + ins.args[0] * 4
                v = img.long(ea)
                if v is not None and not v & 1 and img.contains(v) and canonical(v) not in seen:
                    literal_targets.add(canonical(v))
            if k in D.DELAYED:
                slot = img.word(a + 2)
                if slot is not None and a + 2 not in seen:
                    seen[a + 2] = D.decode(slot, a + 2)
            if k in (D.BT, D.BF):
                work.append(ins.target)
            elif k in (D.BTS, D.BFS):
                work.append(ins.target)
                work.append(a + 4)
                break
            elif k == D.BRA:
                work.append(ins.target)
                break
            elif k == D.BSR:
                work.append(ins.target)
                work.append(a + 4)
                break
            elif k in (D.BSRF, D.JSR):
                work.append(a + 4)
                break
            elif k in (D.BRAF, D.JMP, D.RTS, D.RTE, D.ILLEGAL):
                break
            a += 2
    return seen, literal_targets


def args_c(ins: D.Ins) -> str:
    return "".join(f", {v}u" for v in ins.args)


def body(ins: D.Ins, pc_value: str) -> list[str]:
    """Statements of a non-branch instruction; pc_value is what MAME's PC holds
    while it executes (the instruction's address + 2)."""
    k = ins.kind
    if k in (D.OP,):
        return [f"sh2_{ins.name}(s{args_c(ins)});"]
    if k == D.PCREL:
        return [f"s->pc = {pc_value};", f"sh2_{ins.name}(s{args_c(ins)});"]
    if k == D.LDCSR:
        return [f"sh2_{ins.name}(s{args_c(ins)});"]
    return None


def emit(ins: D.Ins, slot: D.Ins | None) -> list[str]:
    """C statements for one instruction (with its delay slot).  Cycles are
    added in MAME's order: an instruction's own extra cycles inside its
    semantic function, its base cycle after it, so a memory access sees the
    same time MAME's CPU reports (the start of the instruction)."""
    a = ins.address
    k = ins.kind
    L = []
    nxt = f"0x{a + 2:08X}u"
    if k in (D.OP, D.PCREL, D.LDCSR):
        L += body(ins, nxt)
        L.append("s->cycles += 1;")
        L.append(f"return {nxt};")
        return L
    if k == D.SLEEP:
        return ["s->cycles += 1;", f"return sh2_sleep(s, 0x{a:08X}u);"]
    if k == D.ILLEGAL:
        return [f"uint32_t r = sh2_illegal(s, 0x{a:08X}u);", "s->cycles += 1;", "return r;"]
    if k == D.TRAPA:
        return [f"uint32_t r = sh2_trapa(s, {ins.args[0]}u, {nxt});", "s->cycles += 1;", "return r;"]
    tgt = f"0x{ins.target:08X}u" if ins.target is not None else "tgt"
    if k in (D.BT, D.BF):
        cond = "(s->sr & SH_T)" if k == D.BT else "!(s->sr & SH_T)"
        return ["s->cycles += 1;", f"if ({cond}) {{ s->cycles += 2; return {tgt}; }}", f"return {nxt};"]

    # delayed branches: the slot instruction runs inline.  A PC-relative load in
    # the slot uses the slot's own address (checked on this ROM: the literal-pool
    # values only make sense that way).
    if slot is None:
        return [f"return sh2_unsupported(s, 0x{a:08X}u);"]
    sb = body(slot, f"0x{slot.address + 2:08X}u")
    if sb is None:     # branch / trap in a delay slot: slot illegal instruction
        return L + [f"return sh2_illegal(s, 0x{slot.address:08X}u);"]
    slot_lines = sb + ["s->cycles += 1;   /* delay slot */"]
    if k in (D.BTS, D.BFS):
        cond = "(s->sr & SH_T)" if k == D.BTS else "!(s->sr & SH_T)"
        L.append("s->cycles += 1;")
        L.append(f"if (!{cond}) return {nxt};   /* not taken: the slot runs as the next instruction */")
        L.append("s->cycles += 1;")
        L += slot_lines
        L.append(f"return {tgt};")
        return L
    if k == D.BRA:
        L += ["s->cycles += 2;"] + slot_lines + [f"return {tgt};"]
    elif k == D.BSR:
        L += ["s->cycles += 2;", f"s->pr = 0x{a + 4:08X}u;"] + slot_lines + [f"return {tgt};"]
    elif k == D.BRAF:
        L += [f"uint32_t tgt = 0x{a + 4:08X}u + s->r[{ins.args[0]}];", "s->cycles += 2;"] + slot_lines + ["return tgt;"]
    elif k == D.BSRF:
        L += [f"uint32_t tgt = 0x{a + 4:08X}u + s->r[{ins.args[0]}];", f"s->pr = 0x{a + 4:08X}u;", "s->cycles += 2;"] + slot_lines + ["return tgt;"]
    elif k == D.JMP:
        L += [f"uint32_t tgt = s->r[{ins.args[0]}];", "s->cycles += 1;"] + slot_lines + ["return tgt;"]
    elif k == D.JSR:
        L += [f"uint32_t tgt = s->r[{ins.args[0]}];", f"s->pr = 0x{a + 4:08X}u;", "s->cycles += 2;"] + slot_lines + ["return tgt;"]
    elif k == D.RTS:
        L += ["uint32_t tgt = s->pr;", "s->cycles += 2;"] + slot_lines + ["return tgt;"]
    elif k == D.RTE:
        L += ["uint32_t tgt = sh2_rte_pop(s);", "s->cycles += 1;"] + slot_lines + ["return tgt;"]
    else:
        raise ValueError(k)
    return L


def load_seeds(path: Path) -> list[int]:
    out = []
    if path.exists():
        for line in path.read_text(encoding="ascii").splitlines():
            line = line.split("#")[0].strip()
            if line:
                out.append(int(line.split()[0], 16))
    return out


def vector_seeds(img: Image, base: int, count: int = 128) -> list[int]:
    out = []
    for v in range(count):
        if v in (1, 3):     # initial SP values
            continue
        x = img.long(base + v * 4)
        if x is not None and not x & 1 and img.contains(x):
            out.append(x)
    return out


def generate(img: Image, seeds: list[int], out: Path, ram_code: tuple[int, int, int] = (0, 0, 0)) -> dict:
    def plausible(t: int) -> bool:
        """Straight-line decode reaches a return/jump within 400 instructions
        without an illegal opcode."""
        for k in range(400):
            w = img.word(t + 2 * k)
            if w is None:
                return False
            kind = D.decode(w, t + 2 * k).kind
            if kind == D.ILLEGAL:
                return False
            if kind in (D.RTS, D.RTE, D.BRA, D.JMP, D.BRAF):
                return True
        return False

    # pointer tables: runs of 3+ aligned longwords that all point into a ROM
    # region at even addresses whose decode looks like code
    def table_seeds() -> list[int]:
        found = []
        for base, data in img.regions:
            run = []
            for o in range(0, len(data) - 3, 4):
                v = int.from_bytes(data[o:o + 4], "big")
                if not v & 1 and img.contains(v) and canonical(v) != base + o:
                    run.append(canonical(v))
                    continue
                if len(run) >= 3:
                    found += run
                run = []
        return [t for t in sorted(set(found)) if plausible(t)]
    seeds = list(seeds) + table_seeds()

    # walk, then add code addresses found in literal pools (function pointers
    # loaded with MOV.L @(disp,PC)) until nothing new appears
    roots = list(seeds)
    extra: list[int] = []
    while True:
        seen, lits = walk(img, roots)
        new = [t for t in sorted(lits) if t not in seen and t not in extra and plausible(t)]
        if not new:
            break
        extra += new
        roots = list(seeds) + extra
    out.mkdir(parents=True, exist_ok=True)
    written = set()

    def put(path: Path, text: str):
        written.add(path)
        if not path.exists() or path.read_text(encoding="ascii") != text:
            path.write_text(text, encoding="ascii")

    addrs = sorted(seen)
    blocks: dict[int, list[int]] = {}
    for a in addrs:
        blocks.setdefault(a >> 14, []).append(a)
    for blk, items in blocks.items():
        lines = ['/* Generated by tools/recompilesh2.py from the user-supplied ROMs.',
                 ' * ROM-derived code: never commit or redistribute this file. */',
                 '#include "sh2/sh2.h"', '']
        for a in items:
            ins = seen[a]
            slot = seen.get(a + 2) if ins.kind in D.DELAYED else None
            stmt = emit(ins, slot)
            desc = f"{ins.opcode:04X} {D.text(ins)}"
            if slot is not None:
                desc += f"  [slot {slot.opcode:04X} {D.text(slot)}]"
            lines.append(f"/* {a:08X}: {desc} */")
            lines.append(f"uint32_t b_{a:08X}(sh2_state *s)")
            lines.append("{")
            lines += ["    " + x for x in stmt]
            lines.append("}")
            lines.append("")
        put(out / f"sh2prog_{blk << 14:08X}.c", "\n".join(lines) + "\n")

    d = ['/* Generated dispatch table for the translated SH-2 program. */', '#include "sh2/sh2.h"', '#include <stddef.h>', '']
    d += [f"uint32_t b_{a:08X}(sh2_state *s);" for a in addrs]
    d += ['', 'typedef struct { uint32_t key; uint32_t (*fn)(sh2_state *); } entry;', 'static const entry entries[] = {']
    d += [f"    {{ 0x{a:08X}u, b_{a:08X} }}," for a in addrs]
    bits = max(17, (2 * len(addrs)).bit_length())
    d += ['};', f'#define COUNT {len(addrs)}u', f'#define HBITS {bits}u', '#define HSIZE (1u << HBITS)',
          'static uint32_t keys[HSIZE];', 'static uint32_t slots[HSIZE];', 'static int ready;', '',
          'static uint32_t hash(uint32_t pc) { pc ^= pc >> 15; pc *= 0x9E3779B1u; pc ^= pc >> 13; return pc & (HSIZE - 1u); }',
          'static uint32_t canon(uint32_t pc) { uint32_t top = pc & 0xF0000000u; return (top == 0 || top == 0x20000000u) ? (pc & 0xC7FFFFFFu) : pc; }',
          'void sh2_program_init(void) {', '    if (ready) return;',
          '    for (uint32_t i = 0; i < COUNT; i++) { uint32_t h = hash(entries[i].key + 1u); while (keys[h]) h = (h + 1u) & (HSIZE - 1u); keys[h] = entries[i].key + 1u; slots[h] = i; }',
          '    ready = 1;', '}',
          'static int find(uint32_t pc, uint32_t *idx) {', '    uint32_t k = canon(pc) + 1u, h = hash(k);',
          '    while (keys[h]) { if (keys[h] == k) { *idx = slots[h]; return 1; } h = (h + 1u) & (HSIZE - 1u); }', '    return 0;', '}',
          'uint32_t sh2_program_step(sh2_state *s, uint32_t pc) { uint32_t i; if (!find(pc, &i)) return 0xFFFFFFFFu; return entries[i].fn(s); }',
          'int sh2_program_has(uint32_t pc) { uint32_t i; sh2_program_init(); return find(pc, &i); }',
          f'uint32_t sh2_program_entry_count(void) {{ return COUNT; }}',
          '/* translated code that runs from RAM: where it lives and the ROM bytes it was copied from */',
          f'void sh2_program_ram_code(uint32_t *base, uint32_t *size, uint32_t *rom_source) {{ *base = 0x{ram_code[0]:08X}u; *size = 0x{ram_code[1]:X}u; *rom_source = 0x{ram_code[2]:X}u; }}',
          '']
    put(out / "sh2prog_dispatch.c", "\n".join(d) + "\n")
    for old in out.glob("sh2prog_*.c"):
        if old not in written:
            old.unlink()
    slot_pcrel = sum(1 for a, i in seen.items() if i.kind in D.DELAYED and seen.get(a + 2) is not None
                     and seen[a + 2].kind == D.PCREL)
    return {"instructions": len(addrs), "files": len(blocks), "literal_seeds": len(extra),
            "pcrel_in_delay_slot": slot_pcrel}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", type=Path, required=True)
    ap.add_argument("--seeds", type=Path, default=ROOT / "tools" / "sh2_seeds.txt")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "generated")
    a = ap.parse_args()
    program = rom_set.build_program(a.game)
    src, dst, size = rom_set.code_copy(program)
    # the main program runs from work RAM, where the boot code copies it
    regions = [(rom_set.PROGRAM_BASE, program), (dst, program[src:src + size])]
    img = Image(regions)
    seeds = vector_seeds(img, rom_set.PROGRAM_BASE) + [dst]
    seeds += load_seeds(a.seeds)
    st = generate(img, seeds, a.out, ram_code=(dst, size, src))
    print(f"SH-2 program: {st['instructions']} instructions in {st['files']} files "
          f"({st['literal_seeds']} literal-pool entry points; {st['pcrel_in_delay_slot']} PC-relative delay slots)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
