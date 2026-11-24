// PC entry point for the recompiled Handkerchief: SDL2 window, game exe loading, recompiled code start.
// Structure derived from sh3vita port/src/pc/main.c (GPL-3.0).
#include "rt.h"
#include "plat.h"
#include "cp1252.h"
#include <stdlib.h>

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;

SDL_Window *plat_sdl_window(void) { return g_win; }
int plat_width(void) { return 640; }
int plat_height(void) { return 480; }
void plat_show_window(void) { SDL_ShowWindow(g_win); }

int plat_message_box(const char *title, const char *text, unsigned type) {
    if ((type & 15u) || getenv("HK_HEADLESS")) return 0;   // only MB_OK is answered; no fabricated confirmation
    unsigned icon = (type & 0xf0u) == 0x10u ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION;
    char title_utf8[768], text_utf8[6144];
    rt_cp1252_utf8(title_utf8, sizeof title_utf8, title ? title : "");
    rt_cp1252_utf8(text_utf8, sizeof text_utf8, text ? text : "");
    return SDL_ShowSimpleMessageBox(icon, title_utf8, text_utf8, g_win) == 0 ? 1 : 0;
}

// Typed text (plat.h): SDL_TEXTINPUT and Backspace while an EDIT control has the focus. HK_TEXT=frame:text[,...]
// types text at a presented frame for headless tests ('~' stands for Backspace).
static int g_text_on;
static char g_typed[256];
static int g_ntyped;
static void typed_add(const char *utf8) {
    for (const unsigned char *p = (const unsigned char *)utf8; *p && g_ntyped < (int)sizeof g_typed - 1; p++) {
        unsigned ch = *p;
        if (ch >= 0xC2 && ch <= 0xC3 && (p[1] & 0xC0) == 0x80) { ch = ((ch & 0x1F) << 6) | (p[1] & 0x3F); p++; }   // U+0080-U+00FF
        else if (ch >= 0x80) { while ((p[1] & 0xC0) == 0x80) p++; ch = '?'; }
        if (ch >= 0x20 || ch == '\b') g_typed[g_ntyped++] = (char)ch;
    }
}
int plat_has_keyboard(void) { return 1; }
void plat_text_enable(int on) {
    if (on == g_text_on) return;
    g_text_on = on;
    g_ntyped = 0;
    if (on) SDL_StartTextInput(); else SDL_StopTextInput();
}
int plat_text_typed(char *out, int cap) {
    const char *s = getenv("HK_TEXT");
    extern unsigned g_frame;
    static unsigned done_frame;
    while (s && *s && g_text_on) {   // scripted text, once per entry
        char *e;
        unsigned at = (unsigned)strtoul(s, &e, 10);
        if (*e != ':') break;
        const char *t = e + 1, *end = strchr(t, ',');
        size_t n = end ? (size_t)(end - t) : strlen(t);
        if (g_frame >= at && at > done_frame) {
            for (size_t i = 0; i < n && g_ntyped < (int)sizeof g_typed - 1; i++) g_typed[g_ntyped++] = t[i] == '~' ? '\b' : t[i];
            done_frame = at;
        }
        s = end ? end + 1 : t + n;
    }
    int n = g_ntyped < cap ? g_ntyped : cap;
    memcpy(out, g_typed, (size_t)n);
    memmove(g_typed, g_typed + n, (size_t)(g_ntyped - n));
    g_ntyped -= n;
    return n;
}
int plat_text_open(const char *title, const char *text, int maxlen) { (void)title; (void)text; (void)maxlen; return 0; }
int plat_text_status(char *out, int cap) { (void)out; (void)cap; return 2; }

int plat_pump_events(void) {
    SDL_Event e;
    int quit = 0;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit = 1;
        else if (e.type == SDL_TEXTINPUT && g_text_on) typed_add(e.text.text);
        else if (e.type == SDL_KEYDOWN && g_text_on && e.key.keysym.sym == SDLK_BACKSPACE) typed_add("\b");
    }
    return quit;
}

// HK_SHOT=N[,M...]: writes shot_N.ppm (in the working folder) when frame N is presented.
static void maybe_shot(const uint32_t *pixels, int width, int height, int pitch_bytes) {
    static unsigned n;
    const char *list = getenv("HK_SHOT");
    n++;
    for (const char *s = list; s && *s;) {
        char *end;
        unsigned want = (unsigned)strtoul(s, &end, 10);
        if (end == s) break;
        if (want == n) {
            char path[64];
            snprintf(path, sizeof path, "shot_%u.ppm", n);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6 %d %d 255\n", width, height);
                for (int y = 0; y < height; y++)
                    for (int x = 0; x < width; x++) {
                        uint32_t v = pixels[y * (pitch_bytes / 4) + x];
                        unsigned char rgb[3] = {(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
                        fwrite(rgb, 1, 3, f);
                    }
                fclose(f);
                rt_log("[shot] %s\n", path);
            }
        }
        s = *end == ',' ? end + 1 : end;
    }
}

void plat_show_xrgb(uint32_t *pixels, int width, int height, int pitch_bytes) {
    maybe_shot(pixels, width, height, pitch_bytes);
    if (!g_ren) return;
    if (!g_tex) g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, HK_WS_MAX, 480);   // fixed size, never recreated
    SDL_Rect src = {0, 0, width, height};
    SDL_UpdateTexture(g_tex, &src, pixels, pitch_bytes);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, &src, NULL);
    SDL_RenderPresent(g_ren);
}

void plat_present_xrgb(uint32_t *pixels, int width, int height, int pitch_bytes) { plat_show_xrgb(pixels, width, height, pitch_bytes); }
int plat_present_threaded(void) { const char *e = getenv("HK_PRESENT_THREAD"); return e ? atoi(e) : 0; }
int plat_poll_main(void) { return 0; }

// HK_KEYS=frame:vk:frames[,...] (hex vk): scripted key presses for headless tests, e.g. 1600:0d:10 holds Enter
// for 10 frames from presented frame 1600.
static int scripted_key(int vk) {
    extern unsigned g_frame;
    const char *s = getenv("HK_KEYS");
    while (s && *s) {
        char *e;
        unsigned at = (unsigned)strtoul(s, &e, 10);
        if (*e != ':') break;
        int key = (int)strtol(e + 1, &e, 16);
        if (*e != ':') break;
        unsigned len = (unsigned)strtoul(e + 1, &e, 10);
        if (key == vk && g_frame >= at && g_frame < at + len) return 1;
        s = *e == ',' ? e + 1 : e;
    }
    return 0;
}

int plat_vk_down(int vk) {
    if (scripted_key(vk)) return 1;
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    if (!k) return 0;
    switch (vk) {
    case 0x25: return k[SDL_SCANCODE_LEFT];
    case 0x26: return k[SDL_SCANCODE_UP];
    case 0x27: return k[SDL_SCANCODE_RIGHT];
    case 0x28: return k[SDL_SCANCODE_DOWN];
    case 0x0D: return k[SDL_SCANCODE_RETURN];
    case 0x1B: return k[SDL_SCANCODE_ESCAPE];
    case 0x20: return k[SDL_SCANCODE_SPACE];
    case 0x10: return k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
    case 0x11: return k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL];
    case 0x12: return k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT];
    case 0x70: return k[SDL_SCANCODE_F1];
    case 0x71: return k[SDL_SCANCODE_F2];   // Save This Game
    case 0x73: return k[SDL_SCANCODE_F4];   // Stop This Game
    case 0x79: return k[SDL_SCANCODE_F10];  // the game's menu bar (port: hk_menu_key)
    default:
        if (vk >= 'A' && vk <= 'Z') return k[SDL_SCANCODE_A + (vk - 'A')];
        return 0;
    }
}

void rt_platform_fatal(const char *msg) {
    if (getenv("HK_HEADLESS")) return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Handkerchief (recomp)", msg, g_win);
}

// HK_DUMP_AFTER=seconds: logs where the main thread is (rt_dump_hang: host function and guest return addresses).
static int dump_thread(void *arg) {
    int secs = (int)(intptr_t)arg;
    for (;;) { SDL_Delay((Uint32)secs * 1000u); rt_dump_hang(); }
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : getenv("HK_GAME_DIR") ? getenv("HK_GAME_DIR") : "original-data";
    SDL_strlcpy(g_game_dir, dir, sizeof g_game_dir);
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   // keep default SIGINT/SIGTERM: `timeout` must stop test runs
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // HK_WIDE=848: native widescreen (port/hk/wide.c, docs/widescreen-plan.md); unset = the original 640x480.
    hk_wide_set(getenv("HK_WIDE") ? atoi(getenv("HK_WIDE")) : 640);
    if (!getenv("HK_HEADLESS")) {
        g_win = SDL_CreateWindow("Handkerchief (recomp)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 g_hk_ws_w * 3 / 2, 720, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
        g_ren = g_win ? SDL_CreateRenderer(g_win, -1, SDL_RENDERER_PRESENTVSYNC) : NULL;
        if (g_ren) SDL_RenderSetLogicalSize(g_ren, g_hk_ws_w, 480);
    }
    if (rt_init(256u << 20)) rt_fatal("guest memory: allocation failed");
    char exe[600];
    SDL_snprintf(exe, sizeof exe, "%s/HDKWIN.EXE", g_game_dir);
    int r = rt_load_exe(exe);
    if (r) rt_fatal("cannot load %s (%d)", exe, r);
    rt_fill_iat();
    { void hk_patch_data(void); hk_patch_data(); }
    rt_log("game loaded from %s\n", g_game_dir);
    if (getenv("HK_DUMP_AFTER")) SDL_CreateThread(dump_thread, "hkdump", (void *)(intptr_t)atoi(getenv("HK_DUMP_AFTER")));
    rt_run_entry();
    SDL_Quit();
    return 0;
}
