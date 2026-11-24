// PS Vita entry point for the recompiled Handkerchief: SDL2 window 960x544 (game 640x480 letterboxed, or 848x480 with wide_native=1),
// game files in ux0:data/handkerchief/ (the player's own copy, including HDKWIN.EXE), log in the same folder.
// Structure follows port/pc/main.c; startup settings follow the vita-port skill (assets/vita_runtime.c).
#include "rt.h"
#include "plat.h"
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ime_dialog.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "hk/font_data.h"

#define HK_DATA_DIR "ux0:data/handkerchief"

// newlib heap: guest memory (HK_GUEST_MB) plus host allocations. Budget against 365 MB (ATTRIBUTE2=12).
#define HK_GUEST_MB 128
unsigned int _newlib_heap_size_user = (HK_GUEST_MB + 64) * 1024 * 1024;
unsigned int sceLibcHeapSize = 4 * 1024 * 1024;
const unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;

SDL_Window *plat_sdl_window(void) { return g_win; }
int plat_width(void) { return 640; }
int plat_height(void) { return 480; }
void plat_show_window(void) {}

int plat_message_box(const char *title, const char *text, unsigned type) {
    rt_log("[msgbox] %s: %s (type %u)\n", title ? title : "", text ? text : "", type);
    return 0;   // no implicit answer
}

// Text entry (plat.h): the system keyboard (IME dialog). SDL's GXM renderer calls sceCommonDialogUpdate on every
// SDL_RenderPresent, which draws the dialog; the game's dialog keeps presenting while it waits (port/hk/dialog.c).
// Not tested on the console yet.
static SceWChar16 g_ime_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH], g_ime_init[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1],
    g_ime_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static int g_ime_open;
static void latin1_to_utf16(SceWChar16 *out, int cap, const char *s) {
    int n = 0;
    for (; s && *s && n < cap - 1; s++) out[n++] = (uint8_t)*s;
    out[n] = 0;
}
int plat_has_keyboard(void) { return 0; }
void plat_text_enable(int on) { (void)on; }
int plat_text_typed(char *out, int cap) { (void)out; (void)cap; return 0; }
int plat_text_open(const char *title, const char *text, int maxlen) {
    if (g_ime_open) return 1;
    if (maxlen <= 0 || maxlen > SCE_IME_DIALOG_MAX_TEXT_LENGTH) maxlen = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    latin1_to_utf16(g_ime_title, SCE_IME_DIALOG_MAX_TITLE_LENGTH, title);
    latin1_to_utf16(g_ime_init, maxlen + 1, text);
    memset(g_ime_text, 0, sizeof g_ime_text);
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_TRUE;
    p.type = SCE_IME_TYPE_BASIC_LATIN;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = g_ime_title;
    p.maxTextLength = (SceUInt32)maxlen;
    p.initialText = g_ime_init;
    p.inputTextBuffer = g_ime_text;
    int r = sceImeDialogInit(&p);
    rt_log("[ime] sceImeDialogInit -> %08x\n", (unsigned)r);
    g_ime_open = r >= 0;
    return g_ime_open;
}
int plat_text_status(char *out, int cap) {
    if (!g_ime_open) return 2;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return 0;
    SceImeDialogResult r;
    memset(&r, 0, sizeof r);
    sceImeDialogGetResult(&r);
    sceImeDialogTerm();
    g_ime_open = 0;
    if (r.button != SCE_IME_DIALOG_BUTTON_ENTER) return 2;
    int n = 0;
    for (const SceWChar16 *w = g_ime_text; *w && n < cap - 1; w++) out[n++] = (char)(*w < 256 ? *w : '?');
    out[n] = 0;
    return 1;
}

int plat_pump_events(void) {
    SDL_Event e;
    int quit = 0;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) quit = 1;
    return quit;
}

// Aspect: 0 = 4:3 letterboxed (original proportions), 1 = stretched to the full 16:9 screen.
// Toggled with Triangle on the pause screen, saved in HK_DATA_DIR/hkport.cfg.
// FPS counter (frames shown per second, top left): toggled with Square on the pause screen, saved there too.
// wide_native=1 (hkport.cfg only, off by default; no pause toggle yet): native widescreen, the game draws an
// 848x480 screen (port/hk/wide.c, docs/widescreen-plan.md) that fills the 960x544 screen at the 4:3 setting.
#define HK_CFG HK_DATA_DIR "/hkport.cfg"
static int g_present_thread;   // hkport.cfg present_thread=1 (see plat_present_threaded)
static int g_wide, g_fps, g_wide_native, g_vol = 6, g_mus = 6;   // g_vol: master volume 1..10 (pause screen, Left/Right)
extern float g_hk_volume;   // port/hk/dsound.c
extern float g_hk_music_volume;   // port/hk/dsound.c
static void apply_volume(void) { if (g_mus < 1) g_mus = 1; if (g_mus > 10) g_mus = 10; g_hk_music_volume = 0.18f * (float)g_mus; if (g_vol < 1) g_vol = 1; if (g_vol > 10) g_vol = 10; g_hk_volume = 0.06f * (float)g_vol; }
static void apply_aspect(void) {
    if (!g_ren) return;
    if (g_wide) SDL_RenderSetLogicalSize(g_ren, 0, 0);
    else SDL_RenderSetLogicalSize(g_ren, g_hk_ws_w, 480);
}
static void load_cfg(void) {
    FILE *f = fopen(HK_CFG, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) { sscanf(line, "wide=%d", &g_wide); sscanf(line, "fps=%d", &g_fps); sscanf(line, "volume=%d", &g_vol); sscanf(line, "music=%d", &g_mus);
                                     sscanf(line, "wide_native=%d", &g_wide_native); sscanf(line, "present_thread=%d", &g_present_thread); }
    fclose(f);
}
static void save_cfg(void) {
    FILE *f = fopen(HK_CFG, "w");
    if (f) { fprintf(f, "wide=%d\nfps=%d\nwide_native=%d\nvolume=%d\nmusic=%d\npresent_thread=%d\n", g_wide, g_fps, g_wide_native, g_vol, g_mus, g_present_thread); fclose(f); }
}
// Text on a g_hk_ws_w x 480 XRGB buffer with the port's bitmap font (cell 18).
static void draw_text(uint32_t *buf, int x, int y, const char *s, uint32_t color) {
    const int bw = g_hk_ws_w;
    const HkFont *f = &hk_fonts[1];
    for (; *s; s++) {
        uint8_t ch = (uint8_t)*s;
        const HkGlyph *g = &f->glyphs[(ch < 0x20 ? '?' : ch) - 0x20];
        for (int gy = 0; gy < f->cell; gy++)
            for (int gx = 0; gx < g->width; gx++)
                if (((f->rows[g->offset + gy] >> gx) & 1u) && x + gx >= 0 && x + gx < bw && y + gy >= 0 && y + gy < 480)
                    buf[(y + gy) * bw + x + gx] = color;
        x += g->advance;
    }
}

static int text_width(const char *s) {
    int w = 0;
    for (; *s; s++) { uint8_t ch = (uint8_t)*s; w += hk_fonts[1].glyphs[(ch < 0x20 ? '?' : ch) - 0x20].advance; }
    return w;
}

static void show_frame(const uint32_t *pixels, int width, int height, int pitch_bytes) {
    if (!g_ren) return;
    if (!g_tex) g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, HK_WS_MAX, 480);   // fixed size, never recreated
    SDL_Rect src = {0, 0, width, height};
    SDL_UpdateTexture(g_tex, &src, pixels, pitch_bytes);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, &src, NULL);
    SDL_RenderPresent(g_ren);
}

// Start pauses: the last frame is shown darkened with a pause sign until Start is pressed again. The game's
// clocks skip the paused time (g_pause_qpc, read by w32_qpc/w32_tick_ms) so it does not try to catch up.
extern uint64_t g_pause_qpc;
// Select on the pause screen leaves it and opens the game's menu bar (File: save / stop / exit, Option: Key and Game
// Configuration, About): plat_vk_down reports F10 for a few frames, which USER32 turns into the game's right click
// (port/hk/dialog.c hk_menu_key). In play Select opens the menu directly; Exit is in
// its File menu.
static unsigned g_menu_until;
// Cheats (pause screen): the game's own Secret Option (dialog 1490, typed H D K on a
// keyboard) has "Unrivaled Mode": while it is on, 'u' toggles invincibility and 'm' adds money. Its checkbox 1417
// is stored at 0x4AC84C (0x4154F5 / 0x41555D). L sets that flag; R and Circle queue 'U' and 'M' presses that are
// delivered to the game after the pause, a few frames each (plat_vk_down).
#define HK_UNRIVALED 0x4AC84Cu
#define HK_INVINCIBLE 0x47BCD8u   // starts at 1 in .data; each 'u' XORs it (0x455138), effective with Unrivaled Mode
static char g_keyq[32];
static int g_keyq_n;
static unsigned g_keyq_at;
static void keyq_push(char k) { if (g_keyq_n < (int)sizeof g_keyq) g_keyq[g_keyq_n++] = k; }
static int keyq_down(int vk) { extern unsigned g_frame; return g_keyq_n && g_keyq[0] == vk && g_frame >= g_keyq_at && g_frame < g_keyq_at + 3; }
static void keyq_step(void) {   // once per presented frame: 3 frames down, 3 frames up, next key
    extern unsigned g_frame;
    if (g_keyq_n && g_frame >= g_keyq_at + 6) { memmove(g_keyq, g_keyq + 1, (size_t)--g_keyq_n); g_keyq_at = g_frame + 1; }
}
static void pause_loop(const uint32_t *pixels, int width, int height, int pitch_bytes) {
    static uint32_t dim[480 * HK_WS_MAX];
    // The text block (left-aligned lines) and the pause sign are centred on the g_hk_ws_w-wide frame.
    const int bw = g_hk_ws_w, tx = (bw - text_width("Cross: widescreen OFF (restart game)")) / 2, sx = bw / 2 - 90;
    uint64_t t0 = SDL_GetPerformanceCounter();
    memset(dim, 0, sizeof dim);
    for (int y = 0; y < height && y < 480; y++)
        for (int x = 0; x < width && x < bw; x++)
            dim[y * bw + x] = (pixels[y * (pitch_bytes / 4) + x] >> 2) & 0x3f3f3fu;
    for (int y = 200; y < 280; y++)
        for (int x = 0; x < 24; x++) { dim[y * bw + sx + 66 + x] = 0xffffffu; dim[y * bw + sx + 90 + x] = 0xffffffu; }
    draw_text(dim, tx, 300, "Start: continue", 0xffffffu);
    SceCtrlData pad;
    int left_was = 1, right_was = 1, up_was = 1, dn_was = 1, x_was = 1;
    int released = 0, tri_was = 1, sq_was = 1, sel_was = 1, l_was = 1, r_was = 1, o_was = 1, money = 0, inv = 0;
    extern unsigned g_frame;
    for (;;) {
        static uint32_t shown[480 * HK_WS_MAX];
        memcpy(shown, dim, sizeof shown);
        draw_text(shown, tx, 324, g_wide ? "Triangle: screen 16:9" : "Triangle: screen 4:3", 0xffff80u);
        draw_text(shown, tx, 348, g_fps ? "Square: FPS counter on" : "Square: FPS counter off", 0xffff80u);
        draw_text(shown, tx, 372, "Select: game menu", 0xffff80u);
        {
            static int started_wide = -1;
            if (started_wide < 0) started_wide = g_wide_native;
            const char *w = g_wide_native == started_wide ? (g_wide_native ? "Cross: widescreen ON" : "Cross: widescreen OFF")
                          : (g_wide_native ? "Cross: widescreen ON (restart game)" : "Cross: widescreen OFF (restart game)");
            draw_text(shown, tx, 124, w, 0xffff80u);
        }
        {
            char vl[48];
            snprintf(vl, sizeof vl, "Left/Right: volume %d/10", g_vol);
            draw_text(shown, tx, 148, vl, 0xffff80u);
            snprintf(vl, sizeof vl, "Up/Down: music %d/10", g_mus);
            draw_text(shown, tx, 172, vl, 0xffff80u);
        }
        int cheats = *(uint32_t *)(g_mem + HK_UNRIVALED) != 0;
        draw_text(shown, tx, 396, cheats ? "L: cheats on" : "L: cheats off", 0x80ffffu);
        if (cheats) {
            char line[64];
            int on = (*(uint32_t *)(g_mem + HK_INVINCIBLE) != 0) ^ inv;
            snprintf(line, sizeof line, "R: invincible %s", on ? "ON" : "OFF");
            draw_text(shown, tx, 420, line, 0x80ffffu);
            snprintf(line, sizeof line, money ? "Circle: add money (x%d)" : "Circle: add money", money);
            draw_text(shown, tx, 444, line, 0x80ffffu);
        }
        show_frame(shown, bw, 480, bw * 4);
        sceCtrlPeekBufferPositive(0, &pad, 1);
        int tri = (pad.buttons & SCE_CTRL_TRIANGLE) != 0, sq = (pad.buttons & SCE_CTRL_SQUARE) != 0;
        if (tri && !tri_was) { g_wide = !g_wide; apply_aspect(); save_cfg(); }
        if (sq && !sq_was) { g_fps = !g_fps; save_cfg(); }
        // Cross: native widescreen (848x480, more of the world) on/off; the surfaces are created at startup, so the
        // choice applies the next time the game starts.
        int x = (pad.buttons & SCE_CTRL_CROSS) != 0;
        if (x && !x_was) { g_wide_native = !g_wide_native; save_cfg(); }
        x_was = x;
        int l = (pad.buttons & SCE_CTRL_LTRIGGER) != 0, r = (pad.buttons & SCE_CTRL_RTRIGGER) != 0,
            o = (pad.buttons & SCE_CTRL_CIRCLE) != 0;
        if (l && !l_was) *(uint32_t *)(g_mem + HK_UNRIVALED) = !cheats;
        if (cheats && r && !r_was) { inv = !inv; }
        if (cheats && o && !o_was && money < 20) money++;
        l_was = l; r_was = r; o_was = o;
        int lf = (pad.buttons & SCE_CTRL_LEFT) != 0, rt = (pad.buttons & SCE_CTRL_RIGHT) != 0;
        if ((lf && !left_was) || (rt && !right_was)) { g_vol += rt ? 1 : -1; apply_volume(); save_cfg(); }
        int upb = (pad.buttons & SCE_CTRL_UP) != 0, dnb = (pad.buttons & SCE_CTRL_DOWN) != 0;
        if ((upb && !up_was) || (dnb && !dn_was)) { g_mus += upb ? 1 : -1; apply_volume(); save_cfg(); }
        up_was = upb; dn_was = dnb;
        left_was = lf; right_was = rt;
        int sel = (pad.buttons & SCE_CTRL_SELECT) != 0;
        if (sel && !sel_was) { extern unsigned g_frame; g_menu_until = g_frame + 3; break; }
        tri_was = tri;
        sq_was = sq;
        sel_was = sel;
        if (!(pad.buttons & SCE_CTRL_START)) released = 1;
        else if (released) break;
        SDL_Delay(16);
    }
    if (*(uint32_t *)(g_mem + HK_UNRIVALED)) {
        if (inv) keyq_push('U');
        while (money-- > 0) keyq_push('M');
        g_keyq_at = g_frame + 2;
    }
    while (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0 && (pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT))) SDL_Delay(16);
    g_pause_qpc += SDL_GetPerformanceCounter() - t0;
}

static int g_start_was;
// Game thread, once per frame when presenting through the present thread: 1 = Start was just pressed, so the
// caller drains the thread and presents synchronously (plat_present_xrgb opens the pause screen).
int plat_poll_main(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if ((pad.buttons & SCE_CTRL_START) && !g_start_was) return 1;
    g_start_was = (pad.buttons & SCE_CTRL_START) != 0;
    keyq_step();
    return 0;
}
// Off by default: the first console run with the present thread closed the game when stage A0 started
// (2026-11-21, cause not yet known). hkport.cfg present_thread=1 turns it on for testing.
int plat_present_threaded(void) { return g_present_thread; }
void plat_present_xrgb(uint32_t *pixels, int width, int height, int pitch_bytes) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    int start = (pad.buttons & SCE_CTRL_START) != 0;
    if (start && !g_start_was) { pause_loop(pixels, width, height, pitch_bytes); start = 0; }
    g_start_was = start;
    keyq_step();
    plat_show_xrgb(pixels, width, height, pitch_bytes);
}
void plat_show_xrgb(uint32_t *pixels, int width, int height, int pitch_bytes) {
    // FPS: frames shown in the last full second (the game targets 25).
    static uint64_t t_sec;
    static int count, fps;
    uint64_t now = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency();
    count++;
    if (!t_sec) t_sec = now;
    if (now - t_sec >= hz) { fps = (int)((uint64_t)count * hz / (now - t_sec)); count = 0; t_sec = now; }
    if (g_fps && width == g_hk_ws_w && height <= 480 && pitch_bytes == width * 4) {   // drawn into the frame in place
        uint32_t *buf = pixels;
        char txt[16];
        snprintf(txt, sizeof txt, "%d FPS", fps);
        for (int y = 2; y < 22; y++)
            for (int x = 2; x < 74; x++) buf[y * width + x] = (buf[y * width + x] >> 2) & 0x3f3f3fu;
        draw_text(buf, 6, 3, txt, fps >= 24 ? 0x80ff80u : 0xffff60u);
    }
    show_frame(pixels, width, height, pitch_bytes);
}

// Pad mapping. Key tests in stage A0 (2026-11-20) show X shoots, A or Up jumps, S crouches; the
// game's Key Configuration dialog (resource 1492) lists Shoot & Bomb, Turncoat Bomb, Switch & Rotation, Jump and
// Crouch. Menus confirm with Enter or space and go back with Z/X/C (0x4117D4), checking confirm first, so Cross
// sends X + space: it shoots in play and confirms in menus. Square = A (jump), down = arrow + S (crouch),
// Circle = Z (Switch & Rotation), Triangle = C (Turncoat Bomb), as the Key Configuration defaults show; L = Shift,
// R = Ctrl, Select = Esc. Start is the port's own pause (plat_present_xrgb), not a game key. In the game's dialogs
// (port/hk/dialog.c) Cross presses the focused control, Triangle presses OK and Circle or Select cancel.
int hk_dlg_active(void);   // port/hk/dialog.c
extern int g_hk_in_stage;  // port/hk/wide.c
int plat_vk_down(int vk) {
    static SceCtrlData pad;
    static uint32_t last_frame = 0xffffffffu, tap_at = 0xffffffffu;
    extern unsigned g_frame;
    if (last_frame != g_frame) {   // one read per presented frame is enough for GetKeyState polling
        static int init;
        if (!init) { sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG); init = 1; }
        sceCtrlPeekBufferPositive(0, &pad, 1);
        last_frame = g_frame;
        // The game keeps the character prone after Down until Down is pressed AGAIN (or another direction). To be
        // prone only while Down is held, when Down is released with no other direction held the port reports Down
        // released for 2 frames and then presses it for 2 frames: that press stands him up without a jump (checked
        // on PC with HK_KEYS: Down 30 frames, released 2, Down 2, then standing, no jump in between). Cancelled if
        // Down or another direction is pressed first, or a dialog or menu opens.
        int d_now = (pad.buttons & SCE_CTRL_DOWN) || pad.ly > 192;
        int other = (pad.buttons & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP)) || pad.lx < 64 || pad.lx > 192 || pad.ly < 64;
        static int d_prev, skip_release;
        if (d_prev && !d_now) {
            if (skip_release) skip_release = 0;   // his real re-press already stood him up
            else if (!other && !hk_dlg_active() && g_frame >= g_menu_until && g_hk_in_stage) tap_at = g_frame + 2;   // stages only: in menus the tap moved the cursor twice
        } else if (d_now && !d_prev && tap_at != 0xffffffffu && g_frame < tap_at) {
            tap_at = 0xffffffffu;   // pressed again before the synthetic press: the real press does the job
            skip_release = 1;
        } else if (other || hk_dlg_active()) tap_at = 0xffffffffu;
        d_prev = d_now;
    }
    int tap = tap_at != 0xffffffffu && g_frame >= tap_at && g_frame < tap_at + 2;
    uint32_t b = pad.buttons;
    int down = (b & SCE_CTRL_DOWN) || pad.ly > 192 || tap;
    switch (vk) {
    case 0x25: return (b & SCE_CTRL_LEFT) || pad.lx < 64;
    case 0x26: return (b & SCE_CTRL_UP) || pad.ly < 64;
    case 0x27: return (b & SCE_CTRL_RIGHT) || pad.lx > 192;
    case 0x28: return down;
    case 'S': return down;
    case 'X': return (b & SCE_CTRL_CROSS) != 0;
    case 0x20: return (b & SCE_CTRL_CROSS) != 0;
    case 'U': case 'M': return keyq_down(vk);   // cheats from the pause screen
    case 'A': return (b & SCE_CTRL_SQUARE) != 0;
    case 'Z': return (b & SCE_CTRL_CIRCLE) != 0;
    case 'C': return (b & SCE_CTRL_TRIANGLE) != 0;
    // Select opens the game's menu bar in play (F10) and cancels (Esc) while a dialog or the menu is up.
    case 0x1B: return (b & SCE_CTRL_SELECT) && hk_dlg_active();
    case 0x79: return g_frame < g_menu_until || ((b & SCE_CTRL_SELECT) && !hk_dlg_active());
    case 0x10: return (b & SCE_CTRL_LTRIGGER) != 0;
    case 0x11: return (b & SCE_CTRL_RTRIGGER) != 0;
    default: return 0;
    }
}

void rt_platform_fatal(const char *msg) { (void)msg; }   // already in the log; nothing to show it with

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(HK_DATA_DIR, 0777);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    // The game's main thread keeps core 0 to itself; audio (port/hk/dsound.c), log and the game's helper
    // threads run on cores 1-2. Without this the audio thread could share the main thread's core and starve in
    // heavy scenes (sound dropping out when the house is destroyed, in tunnels and on map changes).
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    SDL_strlcpy(g_game_dir, HK_DATA_DIR, sizeof g_game_dir);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) {
        rt_log("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g_win = SDL_CreateWindow("Handkerchief", 0, 0, 960, 544, 0);
    g_ren = g_win ? SDL_CreateRenderer(g_win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
    load_cfg();
    apply_volume();
    hk_wide_set(g_wide_native ? 848 : 640);
    apply_aspect();
    rt_log("[vita] window %p renderer %p\n", (void *)g_win, (void *)g_ren);
    if (rt_init(HK_GUEST_MB << 20)) rt_fatal("guest memory: allocation of %u MB failed", HK_GUEST_MB);
    int r = rt_load_exe(HK_DATA_DIR "/HDKWIN.EXE");
    if (r) rt_fatal("cannot load " HK_DATA_DIR "/HDKWIN.EXE (%d): copy your game files there", r);
    rt_fill_iat();
    { void hk_patch_data(void); hk_patch_data(); }
    rt_log("game loaded from %s\n", g_game_dir);
    rt_run_entry();
    SDL_Quit();
    return 0;
}
