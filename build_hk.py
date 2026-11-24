#!/usr/bin/env python3
"""Handkerchief 2000 - Nintendo Switch static-recompilation build.

Stages:
  python build_hk.py build link nro     (default: all)

Layout expected:
  recomp/                     x86-to-C recompiler (GPL-3.0, from handkerchief-vita)
  build/hk-recomp/gen/        recompiler output: the game's code as C.
                              Generated from YOUR OWN copy of HDKWIN.EXE; never distributed.
  port/                       the Windows/SDL environment the game runs in (GPL-3.0)
  switch/main.c               the Switch entry point
  tools/                      nro_pack.py and helper scripts
  out/                        Handkerchief2000.elf / .nro

Toolchain (all overridable through the environment):
  HK_TC       aarch64-none-elf-gcc prefix directory (xpack or devkitA64)
  DEVKITPRO   devkitPro root (libnx)
  HK_NEWLIB   newlib sysroot        (lib, include)
  HK_LIBS     directory with libSDL2.a
  HK_SDL_INC  SDL2 include directory
  HK_EXE      HDKWIN.EXE (only for the icon fallback; build/hk-recomp/gen/ already has the code)
  HK_ICON     a 256x256 PNG to embed as the NRO icon
  HK_GAME_DIR the original game folder (optional; used by tools/gen_icon_title.py)
"""
import glob
import os
import struct
import subprocess
import sys
import zlib
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(ROOT, 'build', 'hk-recomp', 'gen')
PORT = os.path.join(ROOT, 'port')
SWITCH = os.path.join(ROOT, 'switch')
BUILD = os.path.join(ROOT, 'build', 'hk-switch')
OUT = os.path.join(ROOT, 'out')

TC = os.environ.get('HK_TC', 'C:/pharaoh-build/tc/xpack-aarch64-none-elf-gcc-15.2.1-1.1').replace('\\', '/')
CC = os.path.join(TC, 'bin', 'aarch64-none-elf-gcc.exe')
DKP = os.environ.get('DEVKITPRO', 'C:/pharaoh-build/dkp').replace('\\', '/')
NEWLIB = os.environ.get('HK_NEWLIB', 'C:/pharaoh-build/dkp-newlib').replace('\\', '/')
NEWLIB_LIB = os.path.join(NEWLIB, 'aarch64-none-elf', 'lib')
NEWLIB_INC = os.path.join(NEWLIB, 'aarch64-none-elf', 'include')
LIBS = os.environ.get('HK_LIBS', 'C:/pharaoh-build/libs').replace('\\', '/')
SDL_INC = os.environ.get('HK_SDL_INC', 'C:/pharaoh-build/SDL/include').replace('\\', '/')
SPECS = os.path.join(SWITCH, 'akhenaten_switch.specs')
EXE = os.environ.get('HK_EXE', '')

ARCH = '-march=armv8-a+crc -mtune=cortex-a57 -fPIC'.split()
CFLAGS = ['-std=gnu11', '-ffunction-sections', '-fdata-sections', '-D__SWITCH__',
          '-include', os.path.join(SWITCH, 'log_path.h'),
          '-fno-strict-aliasing', '-Wno-attributes', '-Wno-unused-function']
INCS = ['-I' + os.path.join(PORT, 'rt'), '-I' + os.path.join(PORT, 'win32'),
        '-I' + PORT, '-I' + os.path.join(PORT, 'hk'),
        '-I' + os.path.join(PORT, 'third_party', 'tlsf'),
        '-I' + os.path.join(PORT, 'third_party', 'sdl2_alias'),
        '-I' + GEN, '-I' + SDL_INC, '-I' + os.path.join(DKP, 'libnx', 'include'),
        '-I' + os.path.join(DKP, 'libnx', 'bsd_include'),
        '-isystem', NEWLIB_INC]
JOBS = max(2, (os.cpu_count() or 4) - 2)


def tasks_list():
    T = []
    for f in sorted(glob.glob(os.path.join(GEN, '*.c'))):
        T.append((f, os.path.join(BUILD, 'gen_' + os.path.basename(f) + '.o'), 0))
    for sub in ('rt', 'win32', 'hk'):
        for f in sorted(glob.glob(os.path.join(PORT, sub, '*.c'))):
            T.append((f, os.path.join(BUILD, 'port_' + sub + '_' + os.path.basename(f) + '.o'), 1))
    tlsf = os.path.join(PORT, 'third_party', 'tlsf', 'tlsf.c')
    T.append((tlsf, os.path.join(BUILD, 'port_tlsf.c.o'), 1))
    T.append((os.path.join(SWITCH, 'main.c'), os.path.join(BUILD, 'switch_main.c.o'), 1))
    return T


def stage_build():
    os.makedirs(BUILD, exist_ok=True)
    T = tasks_list()

    def one(t):
        src, obj, is_port = t
        if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src):
            return None
        opt = '-O2' if is_port else '-O1'
        cmd = [CC] + ARCH + CFLAGS + [opt] + INCS + ['-c', src, '-o', obj]
        r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode != 0:
            return (src, (r.stderr or r.stdout or '')[-5000:])
        return None

    fails = []
    with ThreadPoolExecutor(JOBS) as pool:
        for res in pool.map(one, T):
            if res:
                fails.append(res)
                print('FAIL', os.path.basename(res[0]))
    if fails:
        for s, e in fails:
            print('=' * 50)
            print('FAIL:', s)
            print(e)
        raise SystemExit(1)
    print(f'build ok: {len(T)} files')


def stage_link():
    objs = sorted(glob.glob(os.path.join(BUILD, '**', '*.o'), recursive=True))
    if not objs:
        raise SystemExit('no objects')
    os.makedirs(OUT, exist_ok=True)
    elf = os.path.join(OUT, 'Handkerchief2000.elf')
    libs = [os.path.join(LIBS, 'libSDL2.a')]
    env = dict(os.environ, DKP_LIBNX=os.path.join(DKP, 'libnx').replace('\\', '/'))
    cmd = [CC] + ARCH + ['-nodefaultlibs', '-specs=' + SPECS,
            '-L' + os.path.join(DKP, 'libnx', 'lib'), '-L' + LIBS, '-L' + NEWLIB_LIB,
            '-o', elf] + [o.replace('\\', '/') for o in objs] + [l.replace('\\', '/') for l in libs] + [
            '-Wl,--start-group',
            os.path.join(NEWLIB, 'lib', 'libstdc++.a'),
            '-lnx', '-lpthread', '-lsysbase', '-lc', '-lm',
            os.path.join(TC, 'lib', 'gcc', 'aarch64-none-elf', '15.2.1', 'libgcc.a'),
            '-Wl,--end-group']
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, encoding='utf-8', errors='replace')
    print((r.stdout or '')[-2000:])
    print((r.stderr or '')[-8000:])
    if r.returncode != 0:
        raise SystemExit('link failed')
    print('link ok ->', elf)


# ---- icon: the original exe's embedded 32x32 icon, nearest-neighbour x8 ----
def extract_icon_png(exe_path):
    data = open(exe_path, 'rb').read()
    pe_off = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe_off + 6)[0]
    opt_size = struct.unpack_from('<H', data, pe_off + 20)[0]
    sec = pe_off + 24 + opt_size

    def rva2off(rva):
        for i in range(nsec):
            o = sec + 40 * i
            vs, va, rs, rp = struct.unpack_from('<IIII', data, o + 8)
            if va <= rva < va + max(vs, rs):
                return rp + (rva - va)
        return None

    rsrva, rsz = struct.unpack_from('<II', data, pe_off + 24 + 112)
    roff = rva2off(rsrva)

    def walk2(off, path):
        n_named, n_id = struct.unpack_from('<HH', data, off + 12)
        out = []
        for i in range(n_named + n_id):
            eo = off + 16 + i * 8
            nid, noff = struct.unpack_from('<II', data, eo)
            if noff & 0x80000000:
                out += walk2(roff + (noff & 0x7FFFFFFF), path + [nid])
            else:
                doff, dsize = struct.unpack_from('<II', data, roff + noff)
                doff_va = rva2off(doff)
                if doff_va is not None:
                    out.append((path, doff_va, dsize))
        return out

    icos = walk2(roff, [])
    best = None
    for path, off, sz in icos:
        if not path or path[0] != 3:
            continue
        if off + 40 + 1024 > len(data) or sz < 40:
            continue
        w = struct.unpack_from('<i', data, off + 4)[0]
        h2 = struct.unpack_from('<i', data, off + 8)[0] // 2
        if w == 32 and h2 == 32:
            best = (off, sz)
    if not best:
        raise SystemExit('no 32x32 icon found')
    off, sz = best
    bpp = struct.unpack_from('<H', data, off + 14)[0]
    pal_n = struct.unpack_from('<I', data, off + 32)[0] or (1 << bpp)
    pal = []
    for i in range(pal_n):
        b, g, r, _ = data[off + 40 + 4 * i: off + 44 + 4 * i]
        pal.append((r, g, b))
    px_off = off + 40 + 4 * pal_n
    row_bytes = 32 * bpp // 8
    img = []
    for y in range(31, -1, -1):
        row = []
        base = px_off + y * row_bytes
        if bpp == 8:
            for x in range(32):
                row.append(pal[data[base + x]])
        else:
            for x in range(32):
                byte = data[base + (x >> 1)]
                idx = (byte & 0xF) if (x & 1) else (byte >> 4)
                row.append(pal[idx & 0xF])
        img.append(row)
    mask_off = px_off + row_bytes * 32
    mask = [data[mask_off + y * 4: mask_off + y * 4 + 4] for y in range(32)]
    S = 8
    out = bytearray()
    for y in range(256):
        sy = y // S
        srow = img[sy]
        mrow = mask[31 - sy] if (31 - sy) < 32 else mask[0]
        for x in range(256):
            sx = x // S
            r, g, b = srow[sx]
            mb = mrow[sx >> 3]
            bit = (mb >> (7 - (sx & 7))) & 1
            a = 0 if bit else 255
            out += bytes((r, g, b, a))

    def png_chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)

    ihdr = struct.pack('>IIBBBBB', 256, 256, 8, 6, 0, 0, 0)
    raw = b''.join(b'\x00' + bytes(out[y * 256 * 4:(y + 1) * 256 * 4]) for y in range(256))
    return b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', ihdr) + png_chunk(b'IDAT', zlib.compress(raw, 7)) + png_chunk(b'IEND', b'')


def plain_icon_png(rgb=(40, 44, 60)):
    """A flat 256x256 icon, so the NRO can always be packed without the game files."""
    def png_chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)
    row = bytes(rgb) * 256
    raw = b''.join(b'\x00' + row for _ in range(256))
    ihdr = struct.pack('>IIBBBBB', 256, 256, 8, 2, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', ihdr) + png_chunk(b'IDAT', zlib.compress(raw, 9)) + png_chunk(b'IEND', b'')


def stage_nro():
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import nro_pack
    os.makedirs(OUT, exist_ok=True)
    icon_png = os.environ.get('HK_ICON', '')
    if icon_png and os.path.exists(icon_png):
        icon = None
        icon_path = icon_png
    elif EXE and os.path.exists(EXE):
        icon = extract_icon_png(EXE)
        icon_path = os.path.join(OUT, 'icon_exe.png')
        open(icon_path, 'wb').write(icon)
    else:
        icon = plain_icon_png()
        icon_path = os.path.join(OUT, 'icon_plain.png')
        open(icon_path, 'wb').write(icon)
    import hashlib
    if icon is not None:
        print('icon md5', hashlib.md5(icon).hexdigest(), len(icon), 'bytes')
    nro_pack.pack_nro(os.path.join(OUT, 'Handkerchief2000.elf'),
                      os.path.join(OUT, 'Handkerchief2000.nro'),
                      icon_png=icon_path,
                      title='Handkerchief2000', author='千叶黑猫/lsyymmilo',
                      version='NX-2.0')
    nro = open(os.path.join(OUT, 'Handkerchief2000.nro'), 'rb').read()
    print('nro size:', len(nro))


if __name__ == '__main__':
    stages = sys.argv[1:] or ['build', 'link', 'nro']
    for s in stages:
        print(f'=== {s} ===')
        if s == 'build':
            stage_build()
        elif s == 'link':
            stage_link()
        elif s == 'nro':
            stage_nro()