// Emulated USER32: a single window (SDL), minimal message queue.
#include "win32.h"
#include "plat.h"
#include <SDL.h>

#define U(name) void imp_USER32_##name(CPU *c)
#define HWND_MAIN    0x00010010u
#define HWND_DESKTOP 0x00010014u

static uint32_t g_wndproc;
static int g_quit_posted;

// Messages posted by the host for the window procedure (DirectShow SetNotifyWindow): PeekMessageA returns them and
// DispatchMessageA calls the procedure, as on Windows. Any thread may post (movie decoding threads).
static struct { uint32_t hwnd, msg, wp, lp; } g_posted[32];
static int g_nposted;
static SDL_mutex *g_post_lock;
// The GAME DATA loop posts WM_COMMAND 0x9CA4 ("confirm") whenever A/Y goes down - including the press that
// opens the delete confirmation on the Delete button. Left in the queue it is dispatched during that
// confirmation's own modal loop and loads the slot that is being deleted (0x40400C -> CopyFileA from the
// removed .HDK -> crash). The delete path in port/hk/dialog.c purges them before and after the game's chain.
void w32_purge_confirm_commands(void) {
    if (!g_post_lock) return;
    SDL_LockMutex(g_post_lock);
    int n = 0;
    for (int i = 0; i < g_nposted; i++)
        if (!(g_posted[i].msg == 0x111u && (g_posted[i].wp == 0x9CA4u || g_posted[i].wp == 0x9CA5u)))
            g_posted[n++] = g_posted[i];
    g_nposted = n;
    SDL_UnlockMutex(g_post_lock);
}
void w32_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!g_post_lock) g_post_lock = SDL_CreateMutex();
    SDL_LockMutex(g_post_lock);
    if (g_nposted < 32) { g_posted[g_nposted].hwnd = hwnd; g_posted[g_nposted].msg = msg; g_posted[g_nposted].wp = wp; g_posted[g_nposted].lp = lp; g_nposted++; }
    SDL_UnlockMutex(g_post_lock);
}

U(RegisterClassExA) { HOSTFN; g_wndproc = R32(ARG(1) + 8); STDRET(1, 0xc001); }
U(RegisterClassA) { HOSTFN; g_wndproc = R32(ARG(1) + 4); STDRET(1, 0xc001); }   // WNDCLASSA.lpfnWndProc
U(UnregisterClassA) { STDRET(2, 1); }
U(CreateWindowExA) {
    HOSTFN;
    rt_log("CreateWindowExA(\"%s\", %ux%u)\n", GSTR(ARG(3)), ARG(7), ARG(8));
    rt_apply_res();
    STDRET(12, HWND_MAIN);
}
// Dialogs (port/hk/dialog.c) have their own window handles.
int hk_dlg_destroy(CPU *c, uint32_t hwnd);
int hk_dlg_show(uint32_t hwnd, int cmd);
int hk_dlg_set_focus(uint32_t hwnd);
int hk_dlg_active(void);
void hk_dlg_keys(CPU *c, const uint8_t *pressed_vks, int n);
void hk_dlg_refresh(void);
void hk_menu_key(void);
U(DestroyWindow) { HOSTFN; hk_dlg_destroy(c, ARG(1)); STDRET(1, 1); }
U(ShowWindow) {
    HOSTFN;
    int was = hk_dlg_show(ARG(1), (int)ARG(2));
    if (was >= 0) STDRET(2, (uint32_t)was);
    plat_show_window();
    STDRET(2, 1);
}
U(UpdateWindow) { STDRET(1, 1); }
U(GetWindowPlacement) {
    HOSTFN;
    uint32_t p = ARG(2);
    memset(mb + p, 0, 44);
    W32(p, 44);
    W32(p + 8, 1); // SW_SHOWNORMAL
    STDRET(2, 1);
}
U(GetForegroundWindow) { STDRET(0, HWND_MAIN); }
U(GetDesktopWindow) { STDRET(0, HWND_DESKTOP); }
U(LoadIconA) { STDRET(2, 1); }
U(RegisterWindowMessageA) { static uint32_t n = 0xc100; STDRET(1, n++); }
U(SystemParametersInfoA) {
    HOSTFN;
    if (ARG(3) && ARG(1) == 0x10) W32(ARG(3), 0); // SPI_GETSCREENSAVEACTIVE
    STDRET(4, 1);
}
U(DefWindowProcA) { STDRET(4, 0); }
U(MessageBoxA) {
    HOSTFN;
    rt_log("MessageBoxA(\"%s\", \"%s\")\n", GSTR(ARG(3)) ? GSTR(ARG(3)) : "", GSTR(ARG(2)) ? GSTR(ARG(2)) : "");
    int button = plat_message_box(GSTR(ARG(3)), GSTR(ARG(2)), ARG(4));
    if (!button) w32_set_last_error(c, 31); // ERROR_GEN_FAILURE; no implicit answer
    STDRET(4, button);
}
// Keyboard messages: each key the game uses becomes WM_KEYDOWN / WM_KEYUP on a state change (plat_vk_down),
// queued for the window procedure like Windows does; GetKeyState keeps answering from the same state.
static void post_due_timers(void);
static void timer_delivered(uint32_t id);
// While a dialog is up, key presses go to it (hk_dlg_keys) instead of the game's window.
static void post_key_changes(CPU *c) {
    post_due_timers();
    // All letters: the input object keeps the last WM_KEYDOWN virtual key (0x411690 -> [0x4AA148+0x14]) and the game
    // scene reads it for typed codes ('H','D','K' opens the Secret Option dialog 1490 at 0x451F5E; 'u'/'m' in that
    // dialog's text). F1 and F4 are polled with GetKeyState (0x411878, 0x451F2B: "Stop this game ?").
    // F10 opens the port's menu bar.
    static const uint8_t keys[] = {0x25, 0x26, 0x27, 0x28, 0x0D, 0x1B, 0x20, 0x10, 0x11, 0x12, 0x70, 0x71, 0x73, 0x79,
                                   'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q',
                                   'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z'};
    static uint8_t down[256];
    uint8_t pressed[sizeof keys];
    int npressed = 0, dlg = hk_dlg_active();
    for (unsigned i = 0; i < sizeof keys; i++) {
        int vk = keys[i], d = plat_vk_down(vk) != 0;
        if (d == down[vk]) continue;
        down[vk] = (uint8_t)d;
        if (vk == 0x79) { if (d) hk_menu_key(); continue; }   // F10: the game's menu (port/hk/dialog.c)
        if (dlg) { if (d) pressed[npressed++] = (uint8_t)vk; continue; }
        w32_post_message(HWND_MAIN, d ? 0x100u : 0x101u, (uint32_t)vk, d ? 1u : 0xC0000001u);
    }
    if (dlg) hk_dlg_keys(c, pressed, npressed);
}

U(PeekMessageA) {
    HOSTFN;
    uint32_t msg = ARG(1);
    post_key_changes(c);
    hk_dlg_refresh();
    if (plat_pump_events() || g_quit_posted) {
        memset(mb + msg, 0, 28);
        W32(msg + 4, 0x12); // WM_QUIT
        STDRET(5, 1);
    }
    int got = 0;
    if (g_post_lock && g_nposted) {
        SDL_LockMutex(g_post_lock);
        if (g_nposted) {
            memset(mb + msg, 0, 28);
            W32(msg, g_posted[0].hwnd); W32(msg + 4, g_posted[0].msg); W32(msg + 8, g_posted[0].wp); W32(msg + 12, g_posted[0].lp);
            if ((ARG(5) & 1) && g_posted[0].msg == 0x113) timer_delivered(g_posted[0].wp);
            if (ARG(5) & 1) memmove(&g_posted[0], &g_posted[1], (size_t)(--g_nposted) * sizeof g_posted[0]);   // PM_REMOVE
            got = 1;
        }
        SDL_UnlockMutex(g_post_lock);
    }
    STDRET(5, got);
}
int hk_dlg_is(uint32_t hwnd);
uint32_t hk_dlg_dispatch(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
U(TranslateMessage) { STDRET(1, 0); }
U(DispatchMessageA) {
    HOSTFN;
    uint32_t m = ARG(1), r = 0;
    if (R32(m + 4) == 0x113 && R32(m + 12)) {   // WM_TIMER with a TIMERPROC: Windows calls it instead of the window
        rt_guest_call(c, R32(m + 12), 4, R32(m), 0x113u, R32(m + 8), w32_tick_ms());
        STDRET(1, 0);
    }
    if (hk_dlg_is(R32(m))) r = hk_dlg_dispatch(c, R32(m), R32(m + 4), R32(m + 8), R32(m + 12));
    else if (g_wndproc && R32(m + 4) != 0x12) r = rt_guest_call(c, g_wndproc, 4, R32(m), R32(m + 4), R32(m + 8), R32(m + 12));
    STDRET(1, r);
}
U(PostQuitMessage) { g_quit_posted = 1; STDRET(1, 0); }
// Messages for a dialog are queued and reach its procedure through DispatchMessageA: the GAME DATA dialog (1500)
// loop posts WM_COMMAND 0x9CA4 (confirm) / 0x9CA5 (cancel) to it (0x4329F8, 0x432A15); dropping them made Enter
// do nothing. The only other PostMessageA in HDKWIN.EXE (0x40ECE8) is left as before.
U(PostMessageA) {
    HOSTFN;
    if (ARG(2) == 0x10 || ARG(2) == 0x12) g_quit_posted = 1; // WM_CLOSE / WM_QUIT
    else if (hk_dlg_is(ARG(1))) w32_post_message(ARG(1), ARG(2), ARG(3), ARG(4));
    STDRET(4, 1);
}
U(PostThreadMessageA) { STDRET(4, 1); }
U(GetQueueStatus) { STDRET(1, 0); }
U(MsgWaitForMultipleObjects) {
    HOSTFN;
    uint32_t n = ARG(1), hs[64];
    for (uint32_t i = 0; i < n && i < 64; i++) hs[i] = R32(ARG(2) + 4 * i);
    extern uint64_t g_t_wait;
    uint64_t t0 = SDL_GetPerformanceCounter();
    uint32_t r = w32_wait(n, hs, ARG(3) != 0, ARG(4));
    if (c == g_main_cpu) g_t_wait += SDL_GetPerformanceCounter() - t0;
    STDRET(5, r);
}
// The puzzle cursor (rt/game_fixes.c) is the only pointer: the pad moves it while a puzzle is up, else (0, 0).
U(GetCursorPos) {
    HOSTFN;
    extern int sh3_vcursor_on(void);
    extern void sh3_vcursor_pixels(int *px, int *py);
    int x = 0, y = 0;
    if (sh3_vcursor_on()) sh3_vcursor_pixels(&x, &y);
    W32(ARG(1), (uint32_t)x); W32(ARG(1) + 4, (uint32_t)y);
    STDRET(1, 1);
}
U(SetCursorPos) { STDRET(2, 1); }
U(ClientToScreen) { STDRET(2, 1); }
U(GetClientRect) {
    HOSTFN;
    uint32_t r = ARG(2);
    W32(r, 0); W32(r + 4, 0); W32(r + 8, plat_width()); W32(r + 12, plat_height());
    STDRET(2, 1);
}

// ---------- imported by SH3 only ----------
#include "guest_fmt.h"
#define ONCE(name) do { static int said; if (!said++) rt_log("[user32] %s (stub)\n", name); } while (0)
U(GetWindowRect) {
    HOSTFN;
    uint32_t r = ARG(2);
    W32(r, 0); W32(r + 4, 0); W32(r + 8, plat_width()); W32(r + 12, plat_height());
    STDRET(2, 1);
}
U(GetMessageA) {   // blocking PeekMessage: WM_QUIT -> 0, otherwise a WM_NULL every ~10 ms
    HOSTFN;
    uint32_t msg = ARG(1);
    memset(mb + msg, 0, 28);
    if (plat_pump_events() || g_quit_posted) { W32(msg + 4, 0x12); STDRET(4, 0); }
    post_key_changes(c);
    hk_dlg_refresh();
    // HDKWIN.EXE peeks with PM_NOREMOVE, then takes the message with GetMessageA (0x40EA33): it must come off the queue.
    if (g_post_lock && g_nposted) {
        SDL_LockMutex(g_post_lock);
        if (g_nposted) {
            W32(msg, g_posted[0].hwnd); W32(msg + 4, g_posted[0].msg); W32(msg + 8, g_posted[0].wp); W32(msg + 12, g_posted[0].lp);
            if (g_posted[0].msg == 0x113) timer_delivered(g_posted[0].wp);
            memmove(&g_posted[0], &g_posted[1], (size_t)(--g_nposted) * sizeof g_posted[0]);
            SDL_UnlockMutex(g_post_lock);
            STDRET(4, 1);
        }
        SDL_UnlockMutex(g_post_lock);
    }
    SDL_Delay(10);
    STDRET(4, 1);
}
U(GetSystemMetrics) {   // SM_CXSCREEN / SM_CYSCREEN; everything else 0
    HOSTFN;
    STDRET(1, ARG(1) == 0 ? (uint32_t)plat_width() : ARG(1) == 1 ? (uint32_t)plat_height() : 0);
}
U(EnumDisplaySettingsA) {   // (device, mode index or -1 current, DEVMODEA *)
    HOSTFN;
    static const uint16_t modes[][2] = {{640, 480}, {800, 600}, {960, 544}, {1024, 768}, {1280, 720}};
    uint32_t i = ARG(2), dm = ARG(3), w, h;
    if (i == 0xffffffffu || i == 0xfffffffeu) { w = (uint32_t)plat_width(); h = (uint32_t)plat_height(); }
    else if (i < sizeof modes / sizeof modes[0]) { w = modes[i][0]; h = modes[i][1]; }
    else STDRET(3, 0);
    W32(dm + 40, 0x001c0000u);              // dmFields: BITSPERPEL | PELSWIDTH | PELSHEIGHT
    W32(dm + 104, 32); W32(dm + 108, w); W32(dm + 112, h); W32(dm + 120, 60);
    STDRET(3, 1);
}
U(ChangeDisplaySettingsA) { HOSTFN; rt_log("ChangeDisplaySettingsA(%08x)\n", ARG(1)); STDRET(2, 0); }   // DISP_CHANGE_SUCCESSFUL
U(GetWindowLongA) { HOSTFN; STDRET(2, (int32_t)ARG(2) == -4 ? g_wndproc : 0); }   // GWL_WNDPROC
U(SetWindowLongA) {
    HOSTFN;
    uint32_t prev = 0;
    if ((int32_t)ARG(2) == -4) { prev = g_wndproc; g_wndproc = ARG(3); }
    STDRET(3, prev);
}
U(CallWindowProcA) {   // (prev proc, hwnd, msg, wparam, lparam): forward to the guest procedure
    HOSTFN;
    uint32_t r = ARG(1) ? rt_guest_call(c, ARG(1), 4, ARG(2), ARG(3), ARG(4), ARG(5)) : 0;
    STDRET(5, r);
}
U(wsprintfA) {   // cdecl (buf, fmt, ...): the caller pops the arguments
    HOSTFN;
    char tmp[1024];
    int n = guest_vformat(mb, tmp, sizeof tmp, ARG(2), c->esp + 12);
    memcpy(mb + ARG(1), tmp, (size_t)n + 1);
    STDRET(0, n);
}
U(wvsprintfA) {
    HOSTFN;
    char tmp[1024];
    int n = guest_vformat(mb, tmp, sizeof tmp, ARG(2), ARG(3));
    memcpy(mb + ARG(1), tmp, (size_t)n + 1);
    STDRET(3, n);
}
U(GetWindowThreadProcessId) { HOSTFN; if (ARG(2)) W32(ARG(2), 0x10); STDRET(2, 0x100); }
// VK_LBUTTON is cross while the puzzle cursor is up (the game tests the low byte: 0xff80 when down, as Windows gives).
// HDKWIN.EXE polls keys with GetKeyState (arrows at 0x4117DA-0x411850; Z/X/C, space, Ctrl at 0x4117D4;
// Esc, Enter, Alt at 0x40EA7A): the platform answers from the real keyboard (PC) or pad (Vita).
// After a save-slot delete the confirm keys are blocked for a moment: the 1500 loop polls GetKeyState for
// Enter/Space/X/C (0x4028fb/0x40467e) and would read the still-held key that just confirmed the deletion as a
// fresh "confirm" (another 0x9CA4), loading the just-deleted save whose files are already gone (crash).
static uint32_t g_confirmkey_block_until;
void w32_block_confirm_keys(uint32_t ms) { g_confirmkey_block_until = w32_tick_ms() + ms; }
U(GetKeyState) {
    HOSTFN;
    uint32_t vk = ARG(1) & 0xff;
    int down = plat_vk_down((int)vk) != 0;
    if (down && (int32_t)(w32_tick_ms() - g_confirmkey_block_until) < 0 &&
        (vk == 0x0D || vk == 0x20 || vk == 'X' || vk == 'C')) down = 0;
    STDRET(1, down ? 0xff80u : 0u);
}
U(FindWindowA) { STDRET(2, 0); }   // no other running instance
U(IsIconic) { STDRET(1, 0); }
U(SetFocus) { HOSTFN; hk_dlg_set_focus(ARG(1)); STDRET(1, ARG(1)); }
U(SetForegroundWindow) { STDRET(1, 1); }
U(SetWindowPos) { STDRET(7, 1); }
U(AdjustWindowRectEx) { STDRET(4, 1); }
U(AttachThreadInput) { STDRET(3, 1); }
U(ClipCursor) { STDRET(1, 1); }
U(SetCursor) { STDRET(1, 0); }
U(ScreenToClient) { STDRET(2, 1); }
U(InvalidateRect) { STDRET(3, 1); }
U(RedrawWindow) { STDRET(4, 1); }
// Window timers (HDKWIN.EXE starts one once DirectSound works): SetTimer(hwnd, id, ms, proc). A due timer posts
// one WM_TIMER (wParam id, lParam proc) unless one is already waiting, like Windows' low-priority timer message.
static struct { uint32_t hwnd, id, ms, proc, due; int used, pending; } g_timers[16];
static void post_due_timers(void) {
    uint32_t now = w32_tick_ms();
    for (int i = 0; i < 16; i++)
        if (g_timers[i].used && !g_timers[i].pending && (int32_t)(now - g_timers[i].due) >= 0) {
            g_timers[i].pending = 1;
            g_timers[i].due = now + g_timers[i].ms;
            w32_post_message(g_timers[i].hwnd, 0x113u, g_timers[i].id, g_timers[i].proc);
        }
}
static void timer_delivered(uint32_t id) {
    for (int i = 0; i < 16; i++) if (g_timers[i].used && g_timers[i].id == id) g_timers[i].pending = 0;
}
U(SetTimer) {
    HOSTFN;
    uint32_t hwnd = ARG(1), id = ARG(2), ms = ARG(3) < 10 ? 10 : ARG(3), proc = ARG(4);
    int slot = -1;
    for (int i = 0; i < 16; i++) if (g_timers[i].used && g_timers[i].hwnd == hwnd && g_timers[i].id == id) slot = i;
    for (int i = 0; slot < 0 && i < 16; i++) if (!g_timers[i].used) slot = i;
    if (slot < 0) STDRET(4, 0);
    if (!hwnd && !id) id = 0x100 + (uint32_t)slot;
    g_timers[slot].used = 1; g_timers[slot].hwnd = hwnd; g_timers[slot].id = id; g_timers[slot].ms = ms;
    g_timers[slot].proc = proc; g_timers[slot].due = w32_tick_ms() + ms; g_timers[slot].pending = 0;
    rt_log("[user32] SetTimer(id %u, %u ms, proc %08x)\n", id, ms, proc);
    STDRET(4, id ? id : 1);
}
U(KillTimer) {
    HOSTFN;
    for (int i = 0; i < 16; i++) if (g_timers[i].used && g_timers[i].hwnd == ARG(1) && g_timers[i].id == ARG(2)) g_timers[i].used = 0;
    STDRET(2, 1);
}
U(GetClassLongA) { STDRET(2, 0); }
U(BeginPaint) { HOSTFN; memset(mb + ARG(2), 0, 64); W32(ARG(2), 0x0001d000u); STDRET(2, 0x0001d000u); }   // fake HDC
U(EndPaint) { STDRET(2, 1); }
U(RegisterDeviceNotificationA) { STDRET(3, 0x0001e000u); }
U(UnregisterDeviceNotification) { STDRET(1, 1); }
U(LoadStringA) {   // (hInstance, id, buffer, buffer size): from the image's string table
    HOSTFN;
    int w32_load_string(uint32_t id, char *out, size_t cap);
    char s[1024];
    int n = w32_load_string(ARG(2), s, sizeof s);
    if (RT_TRACE("HK_TRACE_GDI") || rt_first_time(0x4c530000ull | ARG(2))) rt_log("LoadStringA(%u) -> \"%s\"\n", ARG(2), s);
    if (!ARG(3) || !ARG(4)) STDRET(4, 0);
    if ((uint32_t)n >= ARG(4)) n = (int)ARG(4) - 1;
    memcpy(mb + ARG(3), s, (size_t)n);
    W8(ARG(3) + (uint32_t)n, 0);
    STDRET(4, (uint32_t)n);
}
U(DrawTextA) { ONCE("DrawTextA"); STDRET(5, 16); }
U(DrawTextW) { ONCE("DrawTextW"); STDRET(5, 16); }

// One pass of a modal dialog's message loop (DialogBoxParamA, port/hk/dialog.c): keys go to the dialog, timers
// and posted messages are dispatched as Windows' modal loop does.
void w32_modal_step(CPU *c) {
    if (plat_pump_events()) g_quit_posted = 1;
    post_key_changes(c);
    for (int guard = 0; guard < 32 && g_post_lock && g_nposted; guard++) {
        SDL_LockMutex(g_post_lock);
        if (!g_nposted) { SDL_UnlockMutex(g_post_lock); break; }
        uint32_t m[4] = {g_posted[0].hwnd, g_posted[0].msg, g_posted[0].wp, g_posted[0].lp};
        memmove(&g_posted[0], &g_posted[1], (size_t)(--g_nposted) * sizeof g_posted[0]);
        SDL_UnlockMutex(g_post_lock);
        if (m[1] == 0x113) {
            timer_delivered(m[2]);
            for (int i = 0; i < 16; i++)
                if (g_timers[i].used && g_timers[i].id == m[2] && g_timers[i].proc) rt_guest_call(c, g_timers[i].proc, 4, m[0], 0x113u, m[2], w32_tick_ms());
        } else if (m[1] != 0x100 && m[1] != 0x101 && g_wndproc) rt_guest_call(c, g_wndproc, 4, m[0], m[1], m[2], m[3]);
    }
}
