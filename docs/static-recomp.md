# Static recompilation of HDKWIN.EXE

Decision (2026-11-20): the main path to a playable Vita build is full static
recompilation of `HDKWIN.EXE` to C, following the architecture of
[sh3vita](https://github.com/matfsouza/sh3vita) (Silent Hill 3 PC, playable on Vita). Reverse
engineering continues as support (function boundaries, indirect calls, Win32 semantics).
Native reconstruction work and the earlier PCRecomp proofs (`plano-recompilacao.md`) remain valid.

## Pipeline

All outputs derived from the game stay under `build/` and are never committed.

```sh
python3 -m venv build/venv && build/venv/bin/pip install capstone unicorn pytest pyflakes pefile
# Ghidra 12.1.2 headless, JDK 21: function list (about 2 minutes)
JAVA_HOME=/usr/lib/jvm/java-21-openjdk /opt/ghidra/support/analyzeHeadless \
  build/hk-recomp/ghidra-proj hk -import original-data/HDKWIN.EXE \
  -scriptPath build/ext/sh3vita/re/ghidra -postScript ExportFunctions.java \
  "$PWD/build/hk-recomp/re/functions.json" -deleteProject
# x86 -> C (about 30 seconds): build/hk-recomp/gen/
build/venv/bin/python -I recomp/x86rec.py
```

`build/ext/sh3vita` is a local clone of sh3vita `2e43f62` (for `ExportFunctions.java`).
Paths can be overridden with `HK_EXE`, `HK_FUNCS` and `HK_GEN`.

## First measurement (2026-11-20)

| Item | Result |
|---|---|
| Ghidra functions exported | 3717 |
| Recompiled functions (incl. 2314 found through pointers) | 4512 |
| Thunks | 1519 |
| Win32 imports referenced | 197 |
| Untranslatable instructions / jumps to non-entries | 0 / 0 |
| Functions using x87 | 9 (8 static, 1 dynamic) |
| Host compile (`cc -std=gnu11 -O1`, 41 files) | 0 errors |
| Vita compile (`arm-vita-eabi-gcc -O2 -mthumb`) | 0 errors, 1,218,392 bytes of text |

Compiling is not running: no runtime, Win32 layer or DirectDraw layer exists yet, and the
translator has not yet been checked with `difftest.py` on this executable.

## Next steps

1. Runtime (`port/rt`): guest memory, PE image loading, dispatch table, heap, threads.
2. Win32 layer for the 197 imports (KERNEL32, USER32, GDI32, WINMM, ADVAPI32...), reusing
   sh3vita's implementations where they fit.
3. DirectDraw and DirectSound as COM objects in guest memory; DirectPlay as a stub.
4. Linux SDL2 build to run the game on PC , then the Vita build.
5. Adapt `recomp/difftest.py` to validate every instruction form used by HDKWIN.EXE.
