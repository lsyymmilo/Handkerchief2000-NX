// Nintendo Switch entry point for the recompiled Handkerchief (port structure follows port/vita/main.c, GPL-3.0).
// Game files in sdmc:/switch/Hdk2000/Hdkwin/ (the player's own copy, including HDKWIN.EXE — unpacked image),
// port log in sdmc:/switch/Hdk2000/hklog.txt, settings in sdmc:/switch/Hdk2000/hkport.cfg.
#include "rt.h"
#include "plat.h"
#include <switch.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "hk/font_data.h"

#define HK_DATA_DIR "sdmc:/switch/Hdk2000"
#define HK_GAME_DIR "sdmc:/switch/Hdk2000/Hdkwin"

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;

SDL_Window *plat_sdl_window(void) { return g_win; }
int plat_width(void) { return 640; }
int plat_height(void) { return 480; }
void plat_show_window(void) {}

int plat_message_box(const char *title, const char *text, unsigned type) {
    rt_log("[msgbox] %s: %s (type %u)\n", title ? title : "", text ? text : "", type);
    return 0;
}

// ---- libnx pad ----
static PadState g_pad;
static int g_pad_init;
static HidAnalogStickState g_stick;
static uint32_t pad_poll(void) {
    if (!g_pad_init) {
        // padConfigureInput is REQUIRED before padInitialize: without it hid has no npad style
        // configured and every read is empty (padGetAttributes stayed 0 on real hardware).
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_pad);
        hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
        g_pad_init = 1;
    }
    padUpdate(&g_pad);
    g_stick = padGetStickPos(&g_pad, 0);
    uint32_t b = padGetButtons(&g_pad);
    static int logged;
    if (!logged) {
        logged = 1;
        rt_log("[pad] init ok, connected %d attributes %08x\n", padIsConnected(&g_pad), padGetAttributes(&g_pad));
    }
    static uint32_t last_b, last_sx, last_sy;
    int sx = g_stick.x, sy = g_stick.y;
    if (b != last_b || (sx && !last_sx) || (!sx && last_sx) || (sy && !last_sy) || (!sy && last_sy)) {
        rt_log("[pad] held %08x stick %d,%d\n", b, sx, sy);
        last_b = b; last_sx = sx; last_sy = sy;
    }
    return b;
}
#define BTN_A    HidNpadButton_A     // Vita Cross: shoot + confirm
#define BTN_B    HidNpadButton_B     // Vita Circle: jump in play (user request: X/B swapped)
#define BTN_X    HidNpadButton_X     // Vita Square: switch & rotation in play (user request: X/B swapped)
#define BTN_Y    HidNpadButton_Y     // Vita Triangle: Turncoat Bomb
#define BTN_L    HidNpadButton_L     // Shift
#define BTN_R    HidNpadButton_R     // Ctrl
#define BTN_MINUS HidNpadButton_Minus // Vita Select: menu / Esc
#define BTN_PLUS  HidNpadButton_Plus  // Vita Start: pause

// ---- text entry (save names): libnx software keyboard, blocking dialog ----
static char g_kbd_text[64];
static int g_kbd_state;   // 0 none, 1 accepted pending, 2 cancelled
int plat_has_keyboard(void) { return 0; }
void plat_text_enable(int on) { (void)on; }
int plat_text_typed(char *out, int cap) { (void)out; (void)cap; return 0; }
int plat_text_open(const char *title, const char *text, int maxlen) {
    if (g_kbd_state == 1) return 1;
    SwkbdConfig kbd;
    Result r = swkbdCreate(&kbd, 0);
    if (R_FAILED(r)) { rt_log("[swkbd] create fail %08x\n", r); return 0; }
    swkbdConfigMakePresetDefault(&kbd);
    if (maxlen <= 0 || maxlen > 32) maxlen = 32;
    swkbdConfigSetStringLenMax(&kbd, (int)maxlen);
    if (title) swkbdConfigSetGuideText(&kbd, title);
    if (text) swkbdConfigSetInitialText(&kbd, text);
    static char outbuf[64];
    memset(outbuf, 0, sizeof outbuf);
    r = swkbdShow(&kbd, outbuf, sizeof outbuf);
    swkbdClose(&kbd);
    rt_log("[swkbd] show -> %08x text='%s'\n", r, outbuf);
    if (R_SUCCEEDED(r) && outbuf[0]) {
        snprintf(g_kbd_text, sizeof g_kbd_text, "%s", outbuf);
        g_kbd_state = 1;
        return 1;
    }
    g_kbd_state = 2;
    return 0;
}
int plat_text_status(char *out, int cap) {
    if (g_kbd_state == 1) {
        snprintf(out, (size_t)cap, "%s", g_kbd_text);
        g_kbd_state = 0;
        return 1;
    }
    if (g_kbd_state == 2) { g_kbd_state = 0; return 2; }
    return 2;
}

int plat_pump_events(void) {
    SDL_Event e;
    int quit = 0;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) quit = 1;
    return quit;
}

// ---- settings (hkport.cfg), same keys as the Vita port ----
#define HK_CFG HK_DATA_DIR "/hkport.cfg"
static int g_wide, g_fps, g_wide_native, g_vol = 6, g_mus = 6;
extern float g_hk_volume;
extern float g_hk_music_volume;
static void apply_volume(void) {
    if (g_mus < 1) g_mus = 1; if (g_mus > 10) g_mus = 10;
    g_hk_music_volume = 0.18f * (float)g_mus;
    if (g_vol < 1) g_vol = 1; if (g_vol > 10) g_vol = 10;
    g_hk_volume = 0.06f * (float)g_vol;
}
static void apply_aspect(void) {
    if (!g_ren) return;
    if (g_wide) SDL_RenderSetLogicalSize(g_ren, 0, 0);
    else SDL_RenderSetLogicalSize(g_ren, g_hk_ws_w, 480);
}
static void load_cfg(void) {
    FILE *f = fopen(HK_CFG, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        sscanf(line, "wide=%d", &g_wide); sscanf(line, "fps=%d", &g_fps);
        sscanf(line, "volume=%d", &g_vol); sscanf(line, "music=%d", &g_mus);
        sscanf(line, "wide_native=%d", &g_wide_native);
    }
    fclose(f);
}
static void save_cfg(void) {
    FILE *f = fopen(HK_CFG, "w");
    if (f) { fprintf(f, "wide=%d\nfps=%d\nwide_native=%d\nvolume=%d\nmusic=%d\n", g_wide, g_fps, g_wide_native, g_vol, g_mus); fclose(f); }
}

// ---- bitmap font helpers (from the port's font_data.h) ----
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
    if (!g_tex) g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, HK_WS_MAX, 480);
    SDL_Rect src = {0, 0, width, height};
    SDL_UpdateTexture(g_tex, &src, pixels, pitch_bytes);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, &src, NULL);
    SDL_RenderPresent(g_ren);
}

// ---- pause screen (+) ----
extern uint64_t g_pause_qpc;
static unsigned g_menu_until;
#define HK_UNRIVALED 0x4AC84Cu
#define HK_INVINCIBLE 0x47BCD8u
static char g_keyq[32];
static int g_keyq_n;
static unsigned g_keyq_at;
static void keyq_push(char k) { if (g_keyq_n < (int)sizeof g_keyq) g_keyq[g_keyq_n++] = k; }
static int keyq_down(int vk) { extern unsigned g_frame; return g_keyq_n && g_keyq[0] == vk && g_frame >= g_keyq_at && g_frame < g_keyq_at + 3; }
static void keyq_step(void) {
    extern unsigned g_frame;
    if (g_keyq_n && g_frame >= g_keyq_at + 6) { memmove(g_keyq, g_keyq + 1, (size_t)--g_keyq_n); g_keyq_at = g_frame + 1; }
}
static void pause_loop(const uint32_t *pixels, int width, int height, int pitch_bytes) {
    static uint32_t dim[480 * HK_WS_MAX];
    const int bw = g_hk_ws_w, tx = (bw - text_width("A: widescreen OFF (restart game)")) / 2, sx = bw / 2 - 90;
    uint64_t t0 = SDL_GetPerformanceCounter();
    memset(dim, 0, sizeof dim);
    for (int y = 0; y < height && y < 480; y++)
        for (int x = 0; x < width && x < bw; x++)
            dim[y * bw + x] = (pixels[y * (pitch_bytes / 4) + x] >> 2) & 0x3f3f3fu;
    for (int y = 200; y < 280; y++)
        for (int x = 0; x < 24; x++) { dim[y * bw + sx + 66 + x] = 0xffffffu; dim[y * bw + sx + 90 + x] = 0xffffffu; }
    draw_text(dim, tx, 300, "+: continue", 0xffffffu);
    int left_was = 1, right_was = 1, up_was = 1, dn_was = 1, a_was = 1;
    int released = 0, y_was = 1, x_was = 1, m_was = 1, l_was = 1, r_was = 1, b_was = 1, money = 0, inv = 0;
    extern unsigned g_frame;
    for (;;) {
        static uint32_t shown[480 * HK_WS_MAX];
        memcpy(shown, dim, sizeof shown);
        draw_text(shown, tx, 324, g_wide ? "Y: screen 16:9" : "Y: screen 4:3", 0xffff80u);
        draw_text(shown, tx, 348, g_fps ? "X: FPS counter on" : "X: FPS counter off", 0xffff80u);
        draw_text(shown, tx, 372, "-: game menu", 0xffff80u);
        {
            static int started_wide = -1;
            if (started_wide < 0) started_wide = g_wide_native;
            const char *w = g_wide_native == started_wide ? (g_wide_native ? "A: widescreen ON" : "A: widescreen OFF")
                          : (g_wide_native ? "A: widescreen ON (restart game)" : "A: widescreen OFF (restart game)");
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
            snprintf(line, sizeof line, money ? "B: add money (x%d)" : "B: add money", money);
            draw_text(shown, tx, 444, line, 0x80ffffu);
        }
        show_frame(shown, bw, 480, bw * 4);
        uint32_t b = pad_poll();
        int tri = (b & BTN_Y) != 0, sq = (b & BTN_X) != 0;
        if (tri && !y_was) { g_wide = !g_wide; apply_aspect(); save_cfg(); }
        if (sq && !x_was) { g_fps = !g_fps; save_cfg(); }
        int a = (b & BTN_A) != 0;
        if (a && !a_was) { g_wide_native = !g_wide_native; save_cfg(); }
        a_was = a;
        int l = (b & BTN_L) != 0, r = (b & BTN_R) != 0, o = (b & BTN_B) != 0;
        if (l && !l_was) *(uint32_t *)(g_mem + HK_UNRIVALED) = !cheats;
        if (cheats && r && !r_was) inv = !inv;
        if (cheats && o && !b_was && money < 20) money++;
        b_was = o;
        int lf = (b & HidNpadButton_Left) != 0 || g_stick.x < -8000;
        int rt = (b & HidNpadButton_Right) != 0 || g_stick.x > 8000;
        if ((lf && !left_was) || (rt && !right_was)) { g_vol += rt ? 1 : -1; apply_volume(); save_cfg(); }
        int upb = (b & HidNpadButton_Up) != 0 || g_stick.y > 8000;
        int dnb = (b & HidNpadButton_Down) != 0 || g_stick.y < -8000;
        if ((upb && !up_was) || (dnb && !dn_was)) { g_mus += upb ? 1 : -1; apply_volume(); save_cfg(); }
        up_was = upb; dn_was = dnb;
        left_was = lf; right_was = rt;
        int sel = (b & BTN_MINUS) != 0;
        if (sel && !m_was) { g_menu_until = g_frame + 3; break; }
        m_was = sel;
        y_was = tri; x_was = sq; l_was = l; r_was = r;
        if (!(b & BTN_PLUS)) released = 1;
        else if (released) break;
        SDL_Delay(16);
    }
    if (*(uint32_t *)(g_mem + HK_UNRIVALED)) {
        if (inv) keyq_push('U');
        while (money-- > 0) keyq_push('M');
        g_keyq_at = g_frame + 2;
    }
    uint32_t b;
    do { b = pad_poll(); SDL_Delay(16); } while (b & (BTN_PLUS | BTN_MINUS));
    g_pause_qpc += SDL_GetPerformanceCounter() - t0;
}

static int g_start_was;
int plat_poll_main(void) {
    uint32_t b = pad_poll();
    if ((b & BTN_PLUS) && !g_start_was) return 1;
    g_start_was = (b & BTN_PLUS) != 0;
    keyq_step();
    return 0;
}
int plat_present_threaded(void) { return 0; }
void plat_present_xrgb(uint32_t *pixels, int width, int height, int pitch_bytes) {
    uint32_t b = pad_poll();
    int start = (b & BTN_PLUS) != 0;
    if (start && !g_start_was) { pause_loop(pixels, width, height, pitch_bytes); start = 0; }
    g_start_was = start;
    keyq_step();
    plat_show_xrgb(pixels, width, height, pitch_bytes);
}
void plat_show_xrgb(uint32_t *pixels, int width, int height, int pitch_bytes) {
    static uint64_t t_sec;
    static int count, fps;
    uint64_t now = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency();
    count++;
    if (!t_sec) t_sec = now;
    if (now - t_sec >= hz) { fps = (int)((uint64_t)count * hz / (now - t_sec)); count = 0; t_sec = now; }
    if (g_fps && width == g_hk_ws_w && height <= 480 && pitch_bytes == width * 4) {
        uint32_t *buf = pixels;
        char txt[16];
        snprintf(txt, sizeof txt, "%d FPS", fps);
        for (int y = 2; y < 22; y++)
            for (int x = 2; x < 74; x++) buf[y * width + x] = (buf[y * width + x] >> 2) & 0x3f3f3fu;
        draw_text(buf, 6, 3, txt, fps >= 24 ? 0x80ff80u : 0xffff60u);
    }
    show_frame(pixels, width, height, pitch_bytes);
}

// ---- pad -> Win32 virtual keys (mapping researched by the Vita port, key tests in stage A0) ----
int hk_dlg_active(void);
extern int g_hk_in_stage;
int plat_vk_down(int vk) {
    static uint32_t tap_at = 0xffffffffu, last_ms;
    extern unsigned g_frame;
    static uint32_t b;
    uint32_t now = SDL_GetTicks();
    // Sampled every 8 ms, not once per presented frame: the GAME DATA dialog (1500) runs its own loop that polls
    // GetKeyState and never presents a frame, so a frame-based sample left the pad frozen and no key ever arrived
    // (arrows could not move the slot, Esc could not cancel).
    if (now - last_ms >= 8) {
        last_ms = now;
        b = pad_poll();
        // Prone toggle: the game keeps prone after Down until Down is pressed again. When Down is released with
        // no other direction held, the port re-presses Down for 2 frames to stand him up (Vita-port measured logic).
        int d_now = (b & HidNpadButton_Down) || g_stick.y < -8000;
        int other = (b & (HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_Up))
                    || g_stick.x < -8000 || g_stick.x > 8000 || g_stick.y > 8000;
        static int d_prev, skip_release;
        if (d_prev && !d_now) {
            if (skip_release) skip_release = 0;
            else if (!other && !hk_dlg_active() && g_frame >= g_menu_until && g_hk_in_stage) tap_at = g_frame + 2;
        } else if (d_now && !d_prev && tap_at != 0xffffffffu && g_frame < tap_at) {
            tap_at = 0xffffffffu;
            skip_release = 1;
        } else if (other || hk_dlg_active()) tap_at = 0xffffffffu;
        d_prev = d_now;
    }
    int tap = tap_at != 0xffffffffu && g_frame >= tap_at && g_frame < tap_at + 2;
    int down = (b & HidNpadButton_Down) || g_stick.y < -8000 || tap;
    switch (vk) {
    case 0x25: return (b & HidNpadButton_Left) || g_stick.x < -8000;
    case 0x26: return (b & HidNpadButton_Up) || g_stick.y > 8000;
    case 0x27: return (b & HidNpadButton_Right) || g_stick.x > 8000;
    case 0x28: return down;
    case 'S': return down;
    case 0x0D: return (b & BTN_A) != 0;   // Enter: menus confirm with it, same as the Vita port's Cross
    case 'X': return (b & BTN_A) != 0;
    case 0x20: return (b & BTN_A) != 0;
    case 'U': case 'M': return keyq_down(vk);
    case 'A': return (b & (hk_dlg_active() ? BTN_X : BTN_B)) != 0;   // jump: B in play (swapped), X in menus
    case 'Z': return (b & (hk_dlg_active() ? BTN_B : BTN_X)) != 0;   // switch & rotation: X in play (swapped), B in menus
    case 'C': return (b & BTN_Y) != 0;
    case 0x1B: return ((b & BTN_MINUS) || (b & BTN_B)) && hk_dlg_active();   // Esc: cancel a dialog (B also cancels)
    case 0x79: return g_frame < g_menu_until || ((b & BTN_MINUS) && !hk_dlg_active());
    case 0x10: return (b & BTN_L) != 0;
    case 0x11: return (b & BTN_R) != 0;
    default: return 0;
    }
}

void rt_platform_fatal(const char *msg) { (void)msg; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // Sphaira forwarder chain-load: the new process does not inherit fs sessions, so
    // sdmc: is unmounted and every fopen fails silently (no log, instant exit on the
    // first rt_load_exe). Mount it ourselves; both calls are no-ops when already done.
    fsInitialize();
    fsdevMountSdmc();
    rt_log("[switch] main enter (%s %s)\n", __DATE__, __TIME__);
    SDL_strlcpy(g_game_dir, HK_GAME_DIR, sizeof g_game_dir);
    rt_log("[switch] SDL_Init(VIDEO|AUDIO|TIMER|EVENTS)\n");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS)) {
        rt_log("[switch] SDL_Init FAILED: %s\n", SDL_GetError());
        return 1;
    }
    rt_log("[switch] SDL ok (video=%s)\n", SDL_GetCurrentVideoDriver());
    g_win = SDL_CreateWindow("Handkerchief", 0, 0, 1280, 720, 0);
    rt_log("[switch] window=%p\n", (void *)g_win);
    g_ren = g_win ? SDL_CreateRenderer(g_win, 0, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    rt_log("[switch] renderer=%p err=%s\n", (void *)g_ren, SDL_GetError());
    load_cfg();
    apply_volume();
    hk_wide_set(g_wide_native ? 848 : 640);
    apply_aspect();
    rt_log("[switch] cfg wide=%d fps=%d wn=%d vol=%d mus=%d\n", g_wide, g_fps, g_wide_native, g_vol, g_mus);
    if (rt_init(128u << 20)) rt_fatal("guest memory: allocation failed");
    rt_log("[switch] guest heap 128MB ok\n");
    int r = rt_load_exe(HK_GAME_DIR "/HDKWIN.EXE");
    if (r) rt_fatal("cannot load " HK_GAME_DIR "/HDKWIN.EXE (%d)", r);
    rt_log("[switch] exe mapped, text %08x-%08x\n", g_text_lo, g_text_hi);
    rt_fill_iat();
    { void hk_patch_data(void); hk_patch_data(); }
    rt_log("[switch] iat+patch done, launching guest entry\n");
    rt_run_entry();
    SDL_Quit();
    return 0;
}
