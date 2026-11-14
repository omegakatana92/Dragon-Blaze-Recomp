"""ROM set definition for Dragon Blaze (MAME 0.264 'dragnblz', Psikyo PS5
hardware: SH-2 + PS6406B video + YMF278B).

Only names, sizes and checksums are stored here; the user supplies the files.
"""
from __future__ import annotations

import hashlib
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class RomFile:
    name: str
    size: int
    crc: int
    sha1: str
    role: str


GAME_FILES = [
    RomFile("2prog_h.u21", 0x80000, 0xFC5EADE8, "e5d05543641e4a3900b0d42e0d5f75734683d635", "SH-2 program (high words)"),
    RomFile("1prog_l.u22", 0x80000, 0x95D6FD02, "2b2830e7fa66cbd13666191762bfddc40571caec", "SH-2 program (low words)"),
    RomFile("1l.u4", 0x200000, 0xC2EB565C, "07e41b36cc03a87f28d091754fdb0d1a7316a532", "graphics"),
    RomFile("1h.u12", 0x200000, 0x23CB46B7, "005b7cc40eea103688a64a72c219c7535970dbfb", "graphics"),
    RomFile("2l.u5", 0x200000, 0xBC256AEA, "1f1d678e8a63513a95f296b8a07d2ea485d1e53f", "graphics"),
    RomFile("2h.u13", 0x200000, 0xB75F59EC, "a6cde94bc972e46e54c962fde49fc2174b312882", "graphics"),
    RomFile("3l.u6", 0x200000, 0x4284F008, "610b13304043411b3088fd4299b3cb0a4d8b0cc2", "graphics"),
    RomFile("3h.u14", 0x200000, 0xABE5CBBF, "c2fb1d8ea8772572c08b36496cf9fc5b91cf848b", "graphics"),
    RomFile("4l.u7", 0x200000, 0xC9FCF2E5, "7cecdf3406da11289b54aaf58d12883ddfdc5e6b", "graphics"),
    RomFile("4h.u15", 0x200000, 0x0AB0A12A, "1b29b6dc79e69edb56634517365d0ee8e6ea78ae", "graphics"),
    RomFile("5l.u8", 0x200000, 0x68D03CCF, "d2bf6da5fa6e346b05872ed9616ffe51c3768f50", "graphics"),
    RomFile("5h.u16", 0x200000, 0x5450FBCA, "7a804263549cea951782a67855e69cb8cb417e98", "graphics"),
    RomFile("6l.u1", 0x200000, 0x8B52C90B, "e1067ef252870787e46c62015e5778b4e641e68d", "graphics"),
    RomFile("6h.u2", 0x200000, 0x7362F929, "9ced06202e3f104d30377aeef489021d26e87f73", "graphics"),
    RomFile("7l.u19", 0x200000, 0xB4F4D86E, "2ad786c5626c98e6943ae05688a1b66307ceac84", "graphics"),
    RomFile("7h.u20", 0x200000, 0x44B7B9CC, "3f8122b62ea1183d9fb3aad32d0e47bd32244f87", "graphics"),
    RomFile("8l.u28", 0x200000, 0xCD079F89, "49c46eb36bc0458428a7fad3fe622f5ed974073b", "graphics"),
    RomFile("8h.u29", 0x200000, 0x3EDB508A, "72b07fb34a94cc127de02070604b1ff31f3d46c7", "graphics"),
    RomFile("9l.u41", 0x200000, 0x0B53CD78, "e2071d9fe6c7be4e289b491587ab431c164e59da", "graphics"),
    RomFile("9h.u42", 0x200000, 0xBC61998A, "75dbefe712104c64576196c27c25dbed59ae3923", "graphics"),
    RomFile("10l.u58", 0x200000, 0xA3F5C7F8, "d17478ca3e7ef46270f350ffa35d43acb05b1185", "graphics"),
    RomFile("10h.u59", 0x200000, 0x30E304C4, "1d866276bfe7f7524306a880d225aaf11ac2e5dd", "graphics"),
    RomFile("snd0.u52", 0x200000, 0x7FD1B225, "6aa61021ada51393bbb34fd1aea00b8feccc8197", "YMF278B samples"),
    RomFile("eeprom-dragnblz.u44", 0x100, 0x46E85DA9, "673cf974fd23a20e6bfa7b2b234206d550011f54", "default EEPROM"),
]

PROGRAM_BASE = 0x00000000
PROGRAM_SIZE = 0x100000

# The boot code (reset vector at 0x400) copies the main program into work RAM
# and jumps to it; its copy descriptor sits at 0x2E148 (source, destination,
# length).  The translator translates that code at its RAM address.
CODE_COPY_DESCRIPTOR = 0x2E148


def _read(zip_path: Path, name: str) -> bytes | None:
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            if info.filename.split("/")[-1].lower() == name.lower():
                return z.read(info)
    return None


def check(files: list[RomFile], zip_path: Path) -> list[tuple[RomFile, str]]:
    out = []
    for f in files:
        data = _read(zip_path, f.name)
        if data is None:
            out.append((f, "MISSING"))
        elif len(data) != f.size or zlib.crc32(data) != f.crc or hashlib.sha1(data).hexdigest() != f.sha1:
            out.append((f, "BAD"))
        else:
            out.append((f, "OK"))
    return out


def build_program(zip_path: Path) -> bytes:
    """maincpu region: ROM_LOAD32_WORD_SWAP of the two program ROMs, as the
    big-endian SH-2 sees it (1 MiB at 0x00000000)."""
    hi = _read(zip_path, "2prog_h.u21")
    lo = _read(zip_path, "1prog_l.u22")
    if hi is None or lo is None:
        raise SystemExit(f"{zip_path}: 2prog_h.u21 / 1prog_l.u22 not found (is this the dragnblz set?)")
    image = bytearray(PROGRAM_SIZE)
    image[0::4] = hi[1::2]
    image[1::4] = hi[0::2]
    image[2::4] = lo[1::2]
    image[3::4] = lo[0::2]
    return bytes(image)


def code_copy(program: bytes) -> tuple[int, int, int]:
    """(source, destination, length) of the boot-time program copy, read from
    the descriptor the boot code uses; the loop copies length rounded up to a
    multiple of 4."""
    def long(a: int) -> int:
        return int.from_bytes(program[a:a + 4], "big")
    src = long(CODE_COPY_DESCRIPTOR)
    dst = long(CODE_COPY_DESCRIPTOR + 4)
    size = (long(CODE_COPY_DESCRIPTOR + 8) & ~3) + 4
    if dst != 0x06000000 or src & 3 or src + size > PROGRAM_SIZE:
        raise SystemExit("unexpected boot copy descriptor (is this the dragnblz set?)")
    return src, dst, size


def main(argv: list[str]) -> int:
    """Check a dragnblz.zip against the expected SHA-1s."""
    if len(argv) < 2:
        print("usage: python tools/rom_set.py dragnblz.zip")
        return 2
    game = Path(argv[1])
    ok = True
    for f, status in check(GAME_FILES, game):
        print(f"{game.name}: {f.name}: {status}")
        ok = ok and status == "OK"
    print("ROM set OK (dragnblz)" if ok else "ROM set check FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    import sys
    raise SystemExit(main(sys.argv))
