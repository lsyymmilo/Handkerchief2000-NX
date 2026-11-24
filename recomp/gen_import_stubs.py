"""Writes weak fallbacks for every Win32 import referenced by the generated code.

Each fallback stops the program with the import's name, so a run always reports the next
missing API. A real implementation in port/ (a strong symbol) replaces the fallback at link time.

Usage: python recomp/gen_import_stubs.py [gen_dir]   (default: build/hk-recomp/gen)
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
gen = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "hk-recomp" / "gen"
names = sorted(set(re.findall(r"void (imp_\w+)\(CPU \*c\);", (gen / "x86_funcs.h").read_text())))
out = ['#include "rt.h"', ""]
out += [f'__attribute__((weak)) void {n}(CPU *c) {{ (void)c; rt_fatal("Win32 import not implemented: {n[4:]}"); }}'
        for n in names]
(gen / "x86_import_stubs.c").write_text("\n".join(out) + "\n")
print(f"{len(names)} import fallbacks -> {gen / 'x86_import_stubs.c'}")
