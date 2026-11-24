// Win32 imports used by HDKWIN.EXE and absent from the sh3vita-derived layer: simple, self-contained ones.
// Every function follows stdcall: STDRET(number of 4-byte arguments, return value).
#include "win32.h"
#include <stdio.h>
#include <stdlib.h>

#define K(name) void imp_KERNEL32_##name(CPU *c)
#define U(name) void imp_USER32_##name(CPU *c)
#define A(name) void imp_ADVAPI32_##name(CPU *c)
#define W(name) void imp_WINMM_##name(CPU *c)
#define S(name) void imp_SHELL32_##name(CPU *c)

// ---------- WINMM: no Windows joystick (the Vita pad is mapped to the keyboard state instead) ----------
#define JOYERR_UNPLUGGED 167u
W(joyGetPos) { HOSTFN; STDRET(2, JOYERR_UNPLUGGED); }
W(joyGetDevCapsA) { HOSTFN; STDRET(3, JOYERR_UNPLUGGED); }

// ---------- USER32 rectangles: RECT = {left, top, right, bottom} int32 ----------
#define RL(r) ((int32_t)R32((r)))
#define RT(r) ((int32_t)R32((r) + 4))
#define RR(r) ((int32_t)R32((r) + 8))
#define RB(r) ((int32_t)R32((r) + 12))
static void set_rect(uint8_t *mb, uint32_t r, int32_t l, int32_t t, int32_t rr, int32_t b) {
    W32(r, l); W32(r + 4, t); W32(r + 8, rr); W32(r + 12, b);
}
U(IsRectEmpty) { HOSTFN; uint32_t r = ARG(1); STDRET(1, RR(r) <= RL(r) || RB(r) <= RT(r)); }
U(CopyRect) {
    HOSTFN;
    if (!ARG(1) || !ARG(2)) STDRET(2, 0);
    memmove(mb + ARG(1), mb + ARG(2), 16);
    STDRET(2, 1);
}
U(OffsetRect) {
    HOSTFN;
    uint32_t r = ARG(1);
    int32_t dx = (int32_t)ARG(2), dy = (int32_t)ARG(3);
    set_rect(mb, r, RL(r) + dx, RT(r) + dy, RR(r) + dx, RB(r) + dy);
    STDRET(3, 1);
}
U(PtInRect) { HOSTFN;   // (const RECT *, POINT by value: x, y)
    uint32_t r = ARG(1);
    int32_t x = (int32_t)ARG(2), y = (int32_t)ARG(3);
    STDRET(3, x >= RL(r) && x < RR(r) && y >= RT(r) && y < RB(r));
}
// Matches Windows: empty sources are ignored; both empty gives an empty (zero) rectangle and FALSE.
U(UnionRect) {
    HOSTFN;
    uint32_t d = ARG(1), a = ARG(2), b = ARG(3);
    int ea = RR(a) <= RL(a) || RB(a) <= RT(a), eb = RR(b) <= RL(b) || RB(b) <= RT(b);
    if (ea && eb) { set_rect(mb, d, 0, 0, 0, 0); STDRET(3, 0); }
    if (ea) { int32_t l = RL(b), t = RT(b), r = RR(b), bo = RB(b); set_rect(mb, d, l, t, r, bo); STDRET(3, 1); }
    if (eb) { int32_t l = RL(a), t = RT(a), r = RR(a), bo = RB(a); set_rect(mb, d, l, t, r, bo); STDRET(3, 1); }
    int32_t l = RL(a) < RL(b) ? RL(a) : RL(b), t = RT(a) < RT(b) ? RT(a) : RT(b);
    int32_t r = RR(a) > RR(b) ? RR(a) : RR(b), bo = RB(a) > RB(b) ? RB(a) : RB(b);
    set_rect(mb, d, l, t, r, bo);
    STDRET(3, 1);
}

// ---------- USER32: single-window shell ----------
U(LoadCursorA) { HOSTFN; STDRET(2, 0x00020001u); }
int hk_dlg_enable(uint32_t hwnd, int on);   // port/hk/dialog.c
int hk_dlg_enabled(uint32_t hwnd);
U(EnableWindow) { HOSTFN; int was = hk_dlg_enable(ARG(1), (int)ARG(2)); STDRET(2, was > 0 ? 1u : 0u); }
U(IsWindowEnabled) { HOSTFN; int e = hk_dlg_enabled(ARG(1)); STDRET(1, e < 0 ? 1u : (uint32_t)e); }
U(WaitMessage) { HOSTFN; SDL_Delay(1); STDRET(0, 1); }
U(OemToCharA) { HOSTFN; if (ARG(1) != ARG(2)) memmove(mb + ARG(2), mb + ARG(1), strlen((char *)mb + ARG(1)) + 1); STDRET(2, 1); }
U(CharToOemA) { HOSTFN; if (ARG(1) != ARG(2)) memmove(mb + ARG(2), mb + ARG(1), strlen((char *)mb + ARG(1)) + 1); STDRET(2, 1); }

// ---------- KERNEL32 memory: GMEM_FIXED semantics (the handle is the pointer) ----------
K(GlobalAlloc) {
    HOSTFN;
    uint32_t size = ARG(2) ? ARG(2) : 1;
    uint32_t p = (ARG(1) & 0x40u) ? gheap_calloc(size) : gheap_alloc(size);   // GMEM_ZEROINIT
    if (!p) w32_set_last_error(c, 8);
    STDRET(2, p);
}
K(GlobalLock) { HOSTFN; STDRET(1, ARG(1)); }
K(GlobalUnlock) { HOSTFN; w32_set_last_error(c, 0); STDRET(1, 0); }
K(GlobalHandle) { HOSTFN; STDRET(1, ARG(1)); }
K(GlobalFree) { HOSTFN; if (ARG(1)) gheap_free(ARG(1)); STDRET(1, 0); }
K(LocalFree) { HOSTFN; if (ARG(1)) gheap_free(ARG(1)); STDRET(1, 0); }
K(FreeResource) { HOSTFN; STDRET(1, 0); }
K(lstrcpyA) { HOSTFN; memmove(mb + ARG(1), mb + ARG(2), strlen((char *)mb + ARG(2)) + 1); STDRET(2, ARG(1)); }

K(GetWindowsDirectoryA) {
    HOSTFN;
    static const char dir[] = "C:\\WINDOWS";
    if (ARG(2) > sizeof dir - 1) strcpy((char *)mb + ARG(1), dir);
    STDRET(2, sizeof dir - 1);
}
K(GetSystemDirectoryA) {
    HOSTFN;
    static const char dir[] = "C:\\WINDOWS\\SYSTEM";
    if (ARG(2) > sizeof dir - 1) strcpy((char *)mb + ARG(1), dir);
    STDRET(2, sizeof dir - 1);
}
static void put_name(CPU *c, uint32_t buf, uint32_t size_ptr, const char *name) {
    uint8_t *const mb = g_mem; (void)c;
    uint32_t need = (uint32_t)strlen(name) + 1;
    if (R32(size_ptr) >= need) strcpy((char *)mb + buf, name);
    W32(size_ptr, need);
}
K(GetComputerNameA) { HOSTFN; put_name(c, ARG(1), ARG(2), "VITA"); STDRET(2, 1); }
A(GetUserNameA) { HOSTFN; put_name(c, ARG(1), ARG(2), "PLAYER"); STDRET(2, 1); }

K(FormatMessageA) { HOSTFN;   // only used to describe an error: an empty message
    if (ARG(5) && ARG(6)) { if (ARG(1) & 0x100u) W32(ARG(5), 0); else W8(ARG(5), 0); }   // ALLOCATE_BUFFER
    STDRET(7, 0);
}

static int copy_host(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    char buf[8192];
    size_t n;
    int ok = a && b;
    while (ok && (n = fread(buf, 1, sizeof buf, a)) > 0) ok = fwrite(buf, 1, n, b) == n;
    if (a) fclose(a);
    if (b) fclose(b);
    return ok;
}
K(CopyFileA) {
    HOSTFN;
    char from[600], to[600];
    w32_host_path(GSTR(ARG(1)), from, sizeof from);
    w32_host_path(GSTR(ARG(2)), to, sizeof to);
    if (ARG(3)) { FILE *e = fopen(to, "rb"); if (e) { fclose(e); w32_set_last_error(c, 80); STDRET(3, 0); } }
    int ok = copy_host(from, to);
    rt_log("CopyFileA(\"%s\", \"%s\") -> %d (%s)\n", GSTR(ARG(1)), GSTR(ARG(2)), ok, to);
    if (!ok) w32_set_last_error(c, 2);
    STDRET(3, ok);
}
K(MoveFileA) {
    HOSTFN;
    char from[600], to[600];
    w32_host_path(GSTR(ARG(1)), from, sizeof from);
    w32_host_path(GSTR(ARG(2)), to, sizeof to);
    int ok = rename(from, to) == 0;
    rt_log("MoveFileA(\"%s\", \"%s\") -> %d (%s)\n", GSTR(ARG(1)), GSTR(ARG(2)), ok, to);
    if (!ok) w32_set_last_error(c, 2);
    STDRET(2, ok);
}

// ---------- KERNEL32 16-bit style file calls (_lopen/_lread/_lclose) ----------
#define LFILE_BASE 0x7000u
static FILE *g_lfiles[64];
K(_lopen) { HOSTFN;   // (path, mode): OF_READ 0, OF_WRITE 1, OF_READWRITE 2
    char hp[600];
    w32_host_path(GSTR(ARG(1)), hp, sizeof hp);
    static const char *modes[] = {"rb", "r+b", "r+b"};
    FILE *f = fopen(hp, modes[ARG(2) & 3u] ? modes[ARG(2) & 3u] : "rb");
    for (unsigned i = 0; f && i < 64; i++)
        if (!g_lfiles[i]) { g_lfiles[i] = f; STDRET(2, LFILE_BASE + i); }
    if (f) fclose(f);
    rt_log("[win32] _lopen(\"%s\") failed\n", hp);
    STDRET(2, 0xFFFFFFFFu);   // HFILE_ERROR
}
static FILE *lfile(uint32_t h) { return h >= LFILE_BASE && h < LFILE_BASE + 64 ? g_lfiles[h - LFILE_BASE] : NULL; }
K(_lread) {
    HOSTFN;
    FILE *f = lfile(ARG(1));
    STDRET(3, f ? (uint32_t)fread(mb + ARG(2), 1, ARG(3), f) : 0xFFFFFFFFu);
}
K(_lclose) {
    HOSTFN;
    FILE *f = lfile(ARG(1));
    if (!f) STDRET(1, 0xFFFFFFFFu);
    fclose(f);
    g_lfiles[ARG(1) - LFILE_BASE] = NULL;
    STDRET(1, 0);
}

// ---------- ADVAPI32 registry writes the sh3vita layer does not cover: accepted, not persisted yet ----------
A(RegCreateKeyExA) { HOSTFN;   // (hkey, subkey, reserved, class, options, sam, security, phkResult, disposition)
    rt_log("[win32] RegCreateKeyExA(\"%s\") (not persisted)\n", GSTR(ARG(2)) ? GSTR(ARG(2)) : "");
    if (ARG(8)) W32(ARG(8), 0x00080001u);
    if (ARG(9)) W32(ARG(9), 1);   // REG_CREATED_NEW_KEY
    STDRET(9, 0);
}
A(RegDeleteKeyA) { HOSTFN; STDRET(2, 0); }
A(RegFlushKey) { HOSTFN; STDRET(1, 0); }

// ---------- SHELL32 ----------
S(ShellExecuteA) { HOSTFN; STDRET(6, 2); }   // "file not found": no external program can be launched on the Vita

// ---------- DirectPlay, Winsock: unavailable (no multiplayer) ----------
// DPLAYX ordinal 4 = DirectPlayLobbyCreateA(guid, out, unknown, data, size)
void imp_DPLAYX_ord4(CPU *c) { HOSTFN; if (ARG(2)) W32(ARG(2), 0); STDRET(5, 0x8877047Eu); }   // DPERR_UNAVAILABLE
// WSOCK32 ordinals: 115 WSAStartup, 116 WSACleanup, 57 gethostname, 52 gethostbyname, 8 htonl
void imp_WSOCK32_ord115(CPU *c) { HOSTFN; STDRET(2, 10091u); }   // WSASYSNOTREADY
void imp_WSOCK32_ord116(CPU *c) { HOSTFN; STDRET(0, 0); }
void imp_WSOCK32_ord57(CPU *c) { HOSTFN; if (ARG(1) && ARG(2) > 4) strcpy((char *)mb + ARG(1), "vita"); STDRET(2, 0); }
void imp_WSOCK32_ord52(CPU *c) { HOSTFN; STDRET(1, 0); }
void imp_WSOCK32_ord8(CPU *c) { HOSTFN; STDRET(1, __builtin_bswap32(ARG(1))); }
