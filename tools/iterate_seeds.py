"""Iterative entry discovery from native execution (development helper).

Runs the headless program with several input patterns; every address where
execution had to fall back to the interpreter (code the static analysis did
not reach) is appended to tools/sh2_seeds.txt, then the program is regenerated
and rebuilt.  Only addresses are stored, never ROM bytes.

usage: python tools/iterate_seeds.py dragnblz.zip [--frames N] [--inputs SPEC ...]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEEDS = ROOT / "tools" / "sh2_seeds.txt"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("game", type=Path)
    ap.add_argument("--frames", type=int, default=20000)
    ap.add_argument("--inputs", nargs="*", default=["none", "auto", "random:1", "random:2", "random:3"])
    ap.add_argument("--iterations", type=int, default=20)
    ap.add_argument("--build-dir", type=Path, default=ROOT / "build")
    args = ap.parse_args()
    exe = args.build_dir / "bin" / ("dragnblz_headless.exe" if sys.platform == "win32" else "dragnblz_headless")
    for iteration in range(args.iterations):
        found = set()
        runs = [(spec, subprocess.Popen([str(exe), "--rom", str(args.game),
                                         "--frames", str(args.frames), "--inputs", spec],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
                for spec in args.inputs]
        for spec, proc in runs:
            out = proc.communicate()[0]
            m = re.search(r"fallback: (\d+) interpreted instructions, (\d+) entry points(.*)", out)
            pcs = [int(x, 16) for x in m.group(3).split()] if m else []
            found.update(pcs)
            print(f"iteration {iteration}: {spec}: {m.group(1) if m else '?'} interpreted, {len(pcs)} entries", flush=True)
        existing = SEEDS.read_text(encoding="ascii") if SEEDS.exists() else ""
        known = {int(l.split()[0], 16) for l in existing.splitlines() if l.strip() and not l.startswith("#")}
        new = sorted(found - known)
        if not new:
            return 0
        with SEEDS.open("a", encoding="ascii") as f:
            for pc in new:
                f.write(f"{pc:08X} discovered by iterative native execution\n")
        subprocess.run(["cmake", "-S", str(ROOT), "-B", str(args.build_dir)], check=True, capture_output=True)
        subprocess.run(["cmake", "--build", str(args.build_dir), "--target", "dragnblz_headless", "-j16"],
                       check=True, capture_output=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
