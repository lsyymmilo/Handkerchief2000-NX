// DirectDraw for the recompiled Handkerchief: COM objects that live in guest memory, software surfaces.
//
// HDKWIN.EXE draws in 8-bit indexed mode (docs/engine-map.md: BltFast through surface slot +0x1C, 8-bit
// row copies of BMP banks). Surfaces keep their pixels in guest memory so Lock() can hand the game a
// pointer; the primary surface is converted through its palette and shown with plat_present_xrgb().
//
// COM layout: a guest object is {vtable guest address, host index}. Each vtable slot holds the synthetic
// guest address of a host function (rt_host_addr). Methods are stdcall with `this` as ARG(1).
// Slots without an implementation stop the run with the interface and method name (HK_TRACE_DD=1 logs calls).
#include <time.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif
#include <SDL2/SDL_mutex.h>
#include <SDL2/SDL_thread.h>
#include <SDL2/SDL_atomic.h>
#include "win32.h"
#include "plat.h"
#include "hk_ddraw.h"
#include <stdlib.h>

#define DD_OK 0u
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600FFu
#define E_NOINTERFACE 0x80004002u

static int g_trace = -1;
static void trace(const char *what) {
    if (g_trace < 0) g_trace = getenv("HK_TRACE_DD") != NULL;
    if (g_trace) rt_log("[dd] %s\n", what);
}

// ---------- host objects ----------
enum { OBJ_DD = 1, OBJ_SURF, OBJ_PAL, OBJ_CLIP };
#define MAX_OBJ 1024
typedef struct {
    int kind, refs;
    uint32_t guest;   // guest address of the COM object
    // surface
    int w, h, pitch, primary, backbuffer;
    uint32_t pixels, caps;
    int has_ck;
    uint32_t ck_lo, ck_hi;
    int attached, palette;   // object indices (0 = none)
    // palette
    uint8_t rgb[256][4];
} Obj;
static Obj g_obj[MAX_OBJ];
static uint32_t g_vt_dd, g_vt_surf, g_vt_pal, g_vt_clip;
static int g_primary;   // index of the primary surface
static uint32_t g_last_flip_ms;   // SDL ticks of the last Flip (palette changes wait for the next one)

static int obj_new(int kind, uint32_t vt) {
    HOSTFN;
    for (int i = 1; i < MAX_OBJ; i++)
        if (!g_obj[i].kind) {
            memset(&g_obj[i], 0, sizeof g_obj[i]);
            g_obj[i].kind = kind;
            g_obj[i].refs = 1;
            g_obj[i].guest = gheap_calloc(16);
            W32(g_obj[i].guest, vt);
            W32(g_obj[i].guest + 4, 0x48440000u | (uint32_t)i);   // 'HD' tag + index
            return i;
        }
    rt_fatal("DirectDraw: too many objects");
}
static Obj *obj_of(uint32_t guest, int kind) {
    HOSTFN;
    if (!guest) return NULL;
    uint32_t tag = R32(guest + 4);
    int i = (int)(tag & 0xffffu);
    if ((tag >> 16) != 0x4844u || i <= 0 || i >= MAX_OBJ || g_obj[i].kind != kind || g_obj[i].guest != guest) {
        rt_log("[dd] bad object 0x%08x (kind %d)\n", guest, kind);
        return NULL;
    }
    return &g_obj[i];
}
static int idx_of(Obj *o) { return o ? (int)(o - g_obj) : 0; }

// ---------- vtable construction ----------
typedef struct { int slot; gfn f; const char *name; } Method;
static uint32_t make_vtable(const char *iface, int nslots, const Method *m, gfn *missing) {
    HOSTFN;
    uint32_t vt = gheap_calloc((uint32_t)nslots * 4u);
    for (int s = 0; s < nslots; s++) W32(vt + 4u * s, rt_host_addr(missing[s], iface));
    for (; m->f; m++) W32(vt + 4u * m->slot, rt_host_addr(m->f, m->name));
    return vt;
}
// One distinct host function per unimplemented slot, so the fatal message names the method.
static const char *const DD_NAMES[] = {"QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette",
    "CreateSurface", "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps",
    "GetDisplayMode", "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine",
    "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode",
    "WaitForVerticalBlank", "GetAvailableVidMem"};
static const char *const SURF_NAMES[] = {"QueryInterface", "AddRef", "Release", "AddAttachedSurface",
    "AddOverlayDirtyRect", "Blt", "BltBatch", "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces",
    "EnumOverlayZOrders", "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey",
    "GetDC", "GetFlipStatus", "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize",
    "IsLost", "Lock", "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette",
    "Unlock", "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder", "GetDDInterface", "PageLock",
    "PageUnlock"};
static void miss_generic(CPU *c, int kind, int slot) {
    (void)c;
    const char *iface = kind == OBJ_DD ? "IDirectDraw" : kind == OBJ_SURF ? "IDirectDrawSurface" : kind == OBJ_PAL ? "IDirectDrawPalette" : "IDirectDrawClipper";
    const char *name = kind == OBJ_DD && slot < 24 ? DD_NAMES[slot] : kind == OBJ_SURF && slot < 39 ? SURF_NAMES[slot] : "?";
    rt_fatal("DirectDraw: %s::%s (slot %d) not implemented", iface, name, slot);
}
#define MS(k, n) static void miss_##k##_##n(CPU *c) { miss_generic(c, k, n); }
#define MS10(k, a) MS(k, a##0) MS(k, a##1) MS(k, a##2) MS(k, a##3) MS(k, a##4) MS(k, a##5) MS(k, a##6) MS(k, a##7) MS(k, a##8) MS(k, a##9)
MS(1, 0) MS(1, 1) MS(1, 2) MS(1, 3) MS(1, 4) MS(1, 5) MS(1, 6) MS(1, 7) MS(1, 8) MS(1, 9) MS10(1, 1) MS10(1, 2)
MS(2, 0) MS(2, 1) MS(2, 2) MS(2, 3) MS(2, 4) MS(2, 5) MS(2, 6) MS(2, 7) MS(2, 8) MS(2, 9) MS10(2, 1) MS10(2, 2) MS10(2, 3)
MS(3, 0) MS(3, 1) MS(3, 2) MS(3, 3) MS(3, 4) MS(3, 5) MS(3, 6) MS(3, 7)
MS(4, 0) MS(4, 1) MS(4, 2) MS(4, 3) MS(4, 4) MS(4, 5) MS(4, 6) MS(4, 7) MS(4, 8) MS(4, 9)
#define L10(k, a) miss_##k##_##a##0, miss_##k##_##a##1, miss_##k##_##a##2, miss_##k##_##a##3, miss_##k##_##a##4, \
    miss_##k##_##a##5, miss_##k##_##a##6, miss_##k##_##a##7, miss_##k##_##a##8, miss_##k##_##a##9
static gfn miss_dd[30] = {miss_1_0, miss_1_1, miss_1_2, miss_1_3, miss_1_4, miss_1_5, miss_1_6, miss_1_7, miss_1_8, miss_1_9, L10(1, 1), L10(1, 2)};
static gfn miss_surf[40] = {miss_2_0, miss_2_1, miss_2_2, miss_2_3, miss_2_4, miss_2_5, miss_2_6, miss_2_7, miss_2_8, miss_2_9, L10(2, 1), L10(2, 2), L10(2, 3)};
static gfn miss_pal[8] = {miss_3_0, miss_3_1, miss_3_2, miss_3_3, miss_3_4, miss_3_5, miss_3_6, miss_3_7};
static gfn miss_clip[10] = {miss_4_0, miss_4_1, miss_4_2, miss_4_3, miss_4_4, miss_4_5, miss_4_6, miss_4_7, miss_4_8, miss_4_9};

// ---------- IUnknown ----------
static void com_addref(CPU *c) {
    HOSTFN;
    uint32_t tag = R32(ARG(1) + 4);
    Obj *o = &g_obj[tag & 0xffffu];
    STDRET(1, (uint32_t)++o->refs);
}
static void com_release(CPU *c) {
    HOSTFN;
    uint32_t tag = R32(ARG(1) + 4);
    Obj *o = &g_obj[tag & 0xffffu];
    int r = --o->refs;
    if (r <= 0) {
        if (o->kind == OBJ_SURF && o->pixels && !o->backbuffer) gheap_free(o->pixels);
        if (idx_of(o) == g_primary) g_primary = 0;
        gheap_free(o->guest);
        o->kind = 0;
        r = 0;
    }
    STDRET(1, (uint32_t)r);
}
// QueryInterface: the same object answers every DirectDraw/surface interface the game asks for (logged).
static void com_qi(CPU *c) {
    HOSTFN;
    uint32_t iid = ARG(2), out = ARG(3);
    rt_log("[dd] QueryInterface %08x-%04x-%04x\n", R32(iid), R16(iid + 4), R16(iid + 6));
    uint32_t tag = R32(ARG(1) + 4);
    g_obj[tag & 0xffffu].refs++;
    W32(out, ARG(1));
    STDRET(3, DD_OK);
}

// ---------- surfaces ----------
static int surf_new(int w, int h, uint32_t caps) {
    int i = obj_new(OBJ_SURF, g_vt_surf);
    Obj *s = &g_obj[i];
    s->w = w; s->h = h; s->pitch = (w + 3) & ~3; s->caps = caps;
    s->pixels = gheap_calloc((uint32_t)(s->pitch * h));
    if (!s->pixels) rt_fatal("DirectDraw: surface %dx%d: out of guest memory", w, h);
    return i;
}

#define DDSD_CAPS 1u
#define DDSD_HEIGHT 2u
#define DDSD_WIDTH 4u
#define DDSD_PITCH 8u
#define DDSD_BACKBUFFERCOUNT 0x20u
#define DDSD_CKSRCBLT 0x10000u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSD_LPSURFACE 0x800u
#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDSCAPS_BACKBUFFER 0x4u

static void fill_desc(uint8_t *mb, uint32_t d, Obj *s) {
    uint32_t size = R32(d) ? R32(d) : 108;
    memset(mb + d, 0, size);
    W32(d, size);
    W32(d + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT);
    W32(d + 8, s->h); W32(d + 12, s->w); W32(d + 16, s->pitch);
    W32(d + 36, s->pixels);
    W32(d + 72, 32); W32(d + 76, 0x60u);   // DDPIXELFORMAT: size, DDPF_PALETTEINDEXED8 | DDPF_RGB
    W32(d + 84, 8);                        // dwRGBBitCount
    W32(d + 104, s->caps);
}

// IDirectDraw::CreateSurface(this, desc, out, outer)
static void dd_create_surface(CPU *c) {
    HOSTFN;
    uint32_t d = ARG(2), flags = R32(d + 4), caps = R32(d + 104);
    int w = (flags & DDSD_WIDTH) ? (int)R32(d + 12) : 640, h = (flags & DDSD_HEIGHT) ? (int)R32(d + 8) : 480;
    if (caps & DDSCAPS_PRIMARYSURFACE) { w = g_hk_ws_w; h = 480; }   // native widescreen: port/hk/wide.c
    int i = surf_new(w, h, caps);
    Obj *s = &g_obj[i];
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s->primary = 1;
        g_primary = i;
        if ((flags & DDSD_BACKBUFFERCOUNT) && R32(d + 20)) {
            int b = surf_new(w, h, DDSCAPS_BACKBUFFER | 0x4000u);
            g_obj[b].backbuffer = 1;
            s->attached = b;
        }
    }
    if (flags & DDSD_CKSRCBLT) { s->has_ck = 1; s->ck_lo = R32(d + 64); s->ck_hi = R32(d + 68); }
    rt_log("[dd] CreateSurface %dx%d caps=%08x flags=%08x backbuffers=%u -> #%d\n", w, h, caps, flags,
           (flags & DDSD_BACKBUFFERCOUNT) ? R32(d + 20) : 0, i);
    W32(ARG(3), s->guest);
    STDRET(4, DD_OK);
}
// IDirectDraw::CreatePalette(this, flags, entries[256] PALETTEENTRY {r,g,b,flags}, out, outer)
static void dd_create_palette(CPU *c) {
    HOSTFN;
    int i = obj_new(OBJ_PAL, g_vt_pal);
    if (ARG(3)) memcpy(g_obj[i].rgb, mb + ARG(3), 1024);
    rt_log("[dd] CreatePalette flags=%08x -> #%d\n", ARG(2), i);
    W32(ARG(4), g_obj[i].guest);
    STDRET(5, DD_OK);
}
static void dd_create_clipper(CPU *c) {
    HOSTFN;
    int i = obj_new(OBJ_CLIP, g_vt_clip);
    W32(ARG(3), g_obj[i].guest);
    STDRET(4, DD_OK);
}
static void dd_set_coop(CPU *c) { HOSTFN; rt_log("[dd] SetCooperativeLevel flags=%08x\n", ARG(3)); STDRET(3, DD_OK); }
static void dd_set_mode(CPU *c) {
    HOSTFN;
    rt_log("[dd] SetDisplayMode %ux%ux%u\n", ARG(2), ARG(3), ARG(4));
    if (ARG(2) != 640 || ARG(3) != 480 || ARG(4) != 8) rt_log("[dd] unexpected display mode\n");
    plat_show_window();
    STDRET(4, DD_OK);
}
static void dd_restore_mode(CPU *c) { HOSTFN; STDRET(1, DD_OK); }
static void dd_wait_vblank(CPU *c) { HOSTFN; STDRET(3, DD_OK); }
// IDirectDraw::GetDisplayMode(this, desc)
static void dd_get_display_mode(CPU *c) {
    HOSTFN;
    Obj tmp = {0};
    tmp.w = g_hk_ws_w; tmp.h = 480; tmp.pitch = g_hk_ws_w;
    fill_desc(mb, ARG(2), &tmp);
    STDRET(2, DD_OK);
}
// IDirectDraw::GetCaps(this, driverCaps, helCaps): zeroed caps with their sizes kept (software only).
static void dd_get_caps(CPU *c) {
    HOSTFN;
    for (int k = 2; k <= 3; k++)
        if (ARG(k)) { uint32_t n = R32(ARG(k)); memset(mb + ARG(k) + 4, 0, n > 4 ? n - 4 : 0); }
    STDRET(3, DD_OK);
}

// ---------- surface methods ----------
static void get_rect(uint8_t *mb, uint32_t r, Obj *s, int *x0, int *y0, int *x1, int *y1) {
    if (r) { *x0 = (int32_t)R32(r); *y0 = (int32_t)R32(r + 4); *x1 = (int32_t)R32(r + 8); *y1 = (int32_t)R32(r + 12); }
    else { *x0 = 0; *y0 = 0; *x1 = s->w; *y1 = s->h; }
}

// HK_PROF=1: every 300 presented frames logs wall ms per frame and where the port spent it.
static double prof_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double g_pf_blit, g_pf_conv, g_pf_plat;
static int g_pf_on = -1;


// ---------- present thread (Vita: core 2; PC: HK_PRESENT_THREAD=1 or 2 to compare) ----------
extern int hk_dlg_active(void);
static inline void convert_row(uint32_t *out, const uint8_t *in, int n, const uint32_t *lut) {
    int x = 0;
    for (; x + 4 <= n; x += 4) { out[x] = lut[in[x]]; out[x + 1] = lut[in[x + 1]]; out[x + 2] = lut[in[x + 2]]; out[x + 3] = lut[in[x + 3]]; }
    for (; x < n; x++) out[x] = lut[in[x]];
}
static int g_pt_mode = -1;
static SDL_sem *g_pt_sem;
static SDL_atomic_t g_pt_busy;
static uint8_t g_pt_pix[480 * HK_WS_MAX];
static uint32_t g_pt_lut[256], g_pt_out[2][480 * HK_WS_MAX];   // double buffer: worker fills one, the main thread shows the other
static int g_pt_w, g_pt_h, g_pt_ox, g_pt_wi, g_pt_pending;
void hk_ws_flip(void);                                    // port/hk/wide.c: 640-only scenes are pillarboxed
int hk_ws_frame_ox(void);
void hk_ws_pillarbox(uint32_t *frame, int w, int h, int ox);
static int pt_enabled(void) {
    if (g_pt_mode < 0) g_pt_mode = plat_present_threaded();
    return g_pt_mode > 0;
}
static int pt_thread(void *arg) {
    (void)arg;
#ifdef __vita__
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_2);
#endif
    for (;;) {
        SDL_SemWait(g_pt_sem);
        double t0 = g_pf_on ? prof_now() : 0;
        uint32_t *o = g_pt_out[g_pt_wi];
        for (int y = 0; y < g_pt_h; y++) convert_row(o + y * g_pt_w, g_pt_pix + y * g_pt_w, g_pt_w, g_pt_lut);
        hk_ws_pillarbox(o, g_pt_w, g_pt_h, g_pt_ox);
        if (g_pf_on) {   // worker time per presented frame, logged every 300
            static unsigned n; static double acc;
            acc += prof_now() - t0;
            if (++n == 300) { rt_log("[prof] present thread %.2f ms/frame\n", acc / n); n = 0; acc = 0; }
        }
        SDL_AtomicSet(&g_pt_busy, 0);
    }
    return 0;
}
static void pt_start(void) {
    if (g_pt_sem) return;
    g_pt_sem = SDL_CreateSemaphore(0);
    SDL_CreateThread(pt_thread, "hkpresent", NULL);
}
// Waits until the worker is idle and drops the frame it finished (before the main thread presents on its own or pauses).
static void pt_drain(void) {
    if (g_pt_sem) while (SDL_AtomicGet(&g_pt_busy)) SDL_Delay(0);
    g_pt_pending = 0;
}

static void present_primary(void) {
    HOSTFN;
    if (g_pf_on < 0) g_pf_on = getenv("HK_PROF") != NULL;
    double pf0 = g_pf_on ? prof_now() : 0;
    {   // HK_TRACE_DD: presents per second (Flip, Blt/BltFast/Unlock to the primary, palette changes)
        static unsigned n; static uint32_t t0;
        uint32_t now = SDL_GetTicks();
        n++;
        if (now - t0 >= 5000) { if (getenv("HK_TRACE_DD")) rt_log("[dd] %u presents in 5 s\n", n); n = 0; t0 = now; }
    }
    if (!g_primary) return;
    Obj *p = &g_obj[g_primary];
    Obj *pal = p->palette ? &g_obj[p->palette] : NULL;
    static uint32_t frame[480 * HK_WS_MAX];
    const int fw = p->w < HK_WS_MAX ? p->w : HK_WS_MAX, fh = p->h < 480 ? p->h : 480;
    uint32_t lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = pal ? ((uint32_t)pal->rgb[i][0] << 16) | ((uint32_t)pal->rgb[i][1] << 8) | pal->rgb[i][2]
                     : (uint32_t)i * 0x010101u;
    double pf2 = 0;
    int dlg = hk_dlg_active();
    if (pt_enabled() && !dlg && !plat_poll_main()) {
        // Pipelined one frame behind. Every SDL renderer call stays on this (the game) thread; the worker only does
        // the CPU work (8-bit to XRGB, pillarbox) into a buffer. When the worker is idle: take the buffer it
        // finished for the previous frame, hand it the new 8-bit frame to convert into the other buffer, then
        // upload the finished one here while the worker runs. Busy worker = frame dropped (HK_PRESENT_THREAD=2
        // waits instead, so scripted shots stay deterministic).
        pt_start();
        if (g_pt_mode == 2) while (SDL_AtomicGet(&g_pt_busy)) SDL_Delay(0);
        if (!SDL_AtomicGet(&g_pt_busy)) {
            uint32_t *done = g_pt_pending ? g_pt_out[g_pt_wi] : NULL;
            int dw = g_pt_w;
            for (int y = 0; y < fh; y++) memcpy(g_pt_pix + y * fw, mb + p->pixels + (uint32_t)(y * p->pitch), (size_t)fw);
            memcpy(g_pt_lut, lut, sizeof lut);
            g_pt_w = fw; g_pt_h = fh; g_pt_ox = hk_ws_frame_ox();
            g_pt_wi ^= 1; g_pt_pending = 1;
            SDL_AtomicSet(&g_pt_busy, 1);
            SDL_SemPost(g_pt_sem);
            if (done) plat_show_xrgb(done, dw, 480, dw * 4);
        }
        pf2 = g_pf_on ? prof_now() : 0;
    } else {
        pt_drain();
        for (int y = 0; y < fh; y++) convert_row(frame + y * fw, mb + p->pixels + (uint32_t)(y * p->pitch), fw, lut);
        hk_ws_pillarbox(frame, fw, fh, hk_ws_frame_ox());
        if (dlg) {
            void hk_dlg_overlay(uint32_t *fb);   // port/hk/dialog.c: open dialogs on top of the game (640x480 buffer)
            if (fw == 640) hk_dlg_overlay(frame);
            else {   // wider screen: the dialogs are drawn on the centred 640 columns
                static uint32_t mid[480][640];
                const int ox = (fw - 640) / 2;
                for (int y = 0; y < 480; y++) memcpy(mid[y], frame + y * fw + ox, 640 * 4);
                hk_dlg_overlay(&mid[0][0]);
                for (int y = 0; y < 480; y++) memcpy(frame + y * fw + ox, mid[y], 640 * 4);
            }
        }
        pf2 = g_pf_on ? prof_now() : 0;
        plat_present_xrgb(frame, fw, 480, fw * 4);
    }
    if (g_pf_on) {
        static unsigned n; static double last;
        double pf3 = prof_now();
        g_pf_conv += pf2 - pf0; g_pf_plat += pf3 - pf2;
        if (++n == 300) {
            rt_log("[prof] %.2f ms/frame wall, convert+overlay %.2f, plat_present %.2f, blits %.2f\n", (pf3 - last) / n, g_pf_conv / n, g_pf_plat / n, g_pf_blit / n);
            n = 0; g_pf_conv = g_pf_plat = g_pf_blit = 0; last = pf3;
        } else if (n == 1 && last == 0) last = pf3;
    }
    extern unsigned g_frame;
    g_frame++;
    extern void hk_saving_tick(void);
    hk_saving_tick();   // clears "saving..."/"loading..." once the save I/O has been quiet for a moment

    void hk_load_trace_frame(void);
    hk_load_trace_frame();
}
void hk_dd_present(void) { present_primary(); }

// Copies a rectangle, clipped to both surfaces; key >= 0 skips that source index (8-bit colour key).
// HK_TRACE_CLIP=1 logs every blit whose source or destination had to be clipped ("[clip]" lines).
static void blit(uint8_t *mb, Obj *d, int dx, int dy, Obj *s, int sx0, int sy0, int sx1, int sy1, int key) {
    int w = sx1 - sx0, h = sy1 - sy0;
    if (sx0 < 0) { dx -= sx0; w += sx0; sx0 = 0; }
    if (sy0 < 0) { dy -= sy0; h += sy0; sy0 = 0; }
    if (dx < 0) { sx0 -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy0 -= dy; h += dy; dy = 0; }
    int w0 = w, h0 = h;
    if (sx0 + w > s->w) w = s->w - sx0;
    if (sy0 + h > s->h) h = s->h - sy0;
    if (dx + w > d->w) w = d->w - dx;
    if (dy + h > d->h) h = d->h - dy;
    if (getenv("HK_TRACE_CLIP") && (w != w0 || h != h0)) rt_log("[clip] dst #%d %dx%d at (%d,%d) src #%d %dx%d at (%d,%d) want %dx%d got %dx%d\n", idx_of(d), d->w, d->h, dx, dy, idx_of(s), s->w, s->h, sx0, sy0, w0, h0, w, h);
    if (w <= 0 || h <= 0) return;
    double pb0 = g_pf_on > 0 ? prof_now() : 0;
    for (int y = 0; y < h; y++) {
        uint8_t *dr = mb + d->pixels + (uint32_t)((dy + y) * d->pitch + dx);
        const uint8_t *sr = mb + s->pixels + (uint32_t)((sy0 + y) * s->pitch + sx0);
        if (key < 0) memmove(dr, sr, (size_t)w);
        else {   // colour key: 4 pixels per step, a byte mask built from "source != key" (SWAR), no branches
            const uint32_t kk = 0x01010101u * (uint8_t)key;
            int x = 0;
            for (; x + 4 <= w; x += 4) {
                uint32_t sv, dv;
                memcpy(&sv, sr + x, 4);
                uint32_t t = sv ^ kk;   // zero byte = key
                uint32_t nz = (((t & 0x7f7f7f7fu) + 0x7f7f7f7fu) | t) & 0x80808080u;   // 0x80 where the byte is not zero
                if (!nz) continue;
                uint32_t m = (nz >> 7) * 0xffu;
                if (m == 0xffffffffu) { memcpy(dr + x, &sv, 4); continue; }
                memcpy(&dv, dr + x, 4);
                dv = (dv & ~m) | (sv & m);
                memcpy(dr + x, &dv, 4);
            }
            for (; x < w; x++) if (sr[x] != (uint8_t)key) dr[x] = sr[x];
        }
    }
    if (g_pf_on > 0) g_pf_blit += prof_now() - pb0;
}
// Stretched copy (nearest), used when Blt's source and destination rectangles differ in size.
static void stretch(uint8_t *mb, Obj *d, int dx0, int dy0, int dx1, int dy1, Obj *s, int sx0, int sy0, int sx1, int sy1, int key) {
    int dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int ty = dy0 + y, fy = sy0 + y * sh / dh;
        if (ty < 0 || ty >= d->h || fy < 0 || fy >= s->h) continue;
        for (int x = 0; x < dw; x++) {
            int tx = dx0 + x, fx = sx0 + x * sw / dw;
            if (tx < 0 || tx >= d->w || fx < 0 || fx >= s->w) continue;
            uint8_t v = R8(s->pixels + (uint32_t)(fy * s->pitch + fx));
            if (key < 0 || v != (uint8_t)key) W8(d->pixels + (uint32_t)(ty * d->pitch + tx), v);
        }
    }
}

#define DDBLT_COLORFILL 0x400u
#define DDBLT_KEYSRC 0x8000u
#define DDBLT_KEYSRCOVERRIDE 0x10000u
// HK_TRACE_PIX=x,y,N: logs every Blt / BltFast / colour fill to the back buffer that covers pixel (x, y) while
// frame N is drawn: source surface and point, colour key (-1 = opaque), source and resulting index, return address.
// Used to show that the black strip under the house in A0 is an opaque BltFast of black from HdkMAPb0 asked by
// the game itself (0x408D59, element bit 15 clear), which the original also shows (gameplay recording).
static void trace_pix(uint8_t *mb, const char *op, Obj *d, int dx0, int dy0, int dx1, int dy1, Obj *s, int sx0, int sy0, int key, uint32_t ret) {
    static int px = -1, py, pf;
    if (px == -1) { const char *e = getenv("HK_TRACE_PIX"); px = -2; if (e) sscanf(e, "%d,%d,%d", &px, &py, &pf); }
    extern unsigned g_frame;
    if (px < 0 || !d->backbuffer || (int)g_frame + 1 != pf) return;
    if (px < dx0 || px >= dx1 || py < dy0 || py >= dy1) return;
    int sx = s ? sx0 + px - dx0 : -1, sy = s ? sy0 + py - dy0 : -1;
    rt_log("[pix] %s dst(%d,%d)-(%d,%d) src #%d %dx%d at (%d,%d) key %d srcval %d -> %d (ret %08x)\n", op, dx0, dy0, dx1, dy1,
           s ? idx_of(s) : 0, s ? s->w : 0, s ? s->h : 0, sx, sy, key,
           s && sx >= 0 && sy >= 0 && sx < s->w && sy < s->h ? R8(s->pixels + (uint32_t)(sy * s->pitch + sx)) : -1,
           R8(d->pixels + (uint32_t)(py * d->pitch + px)), ret);
}
// IDirectDrawSurface::Blt(this, destRect, src, srcRect, flags, fx)
static void surf_blt(CPU *c) {
    HOSTFN;
    Obj *d = obj_of(ARG(1), OBJ_SURF);
    uint32_t flags = ARG(5), fx = ARG(6);
    if (!d) STDRET(6, DDERR_INVALIDPARAMS);
    int dx0, dy0, dx1, dy1;
    get_rect(mb, ARG(2), d, &dx0, &dy0, &dx1, &dy1);
    if (flags & DDBLT_COLORFILL) {
        uint8_t col = fx ? (uint8_t)R32(fx + 80) : 0;
        if (dx0 < 0) dx0 = 0;
        if (dy0 < 0) dy0 = 0;
        if (dx1 > d->w) dx1 = d->w;
        if (dy1 > d->h) dy1 = d->h;
        for (int y = dy0; y < dy1; y++) memset(mb + d->pixels + (uint32_t)(y * d->pitch + dx0), col, (size_t)(dx1 > dx0 ? dx1 - dx0 : 0));
        trace_pix(mb, "fill", d, dx0, dy0, dx1, dy1, NULL, 0, 0, col, R32(c->esp));
    } else {
        Obj *s = obj_of(ARG(3), OBJ_SURF);
        if (!s) STDRET(6, DDERR_INVALIDPARAMS);
        int sx0, sy0, sx1, sy1;
        get_rect(mb, ARG(4), s, &sx0, &sy0, &sx1, &sy1);
        int key = -1;
        if ((flags & DDBLT_KEYSRC) && s->has_ck) key = (int)(s->ck_lo & 0xff);
        if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) key = (int)(R32(fx + 92) & 0xff);
        if (dx1 - dx0 == sx1 - sx0 && dy1 - dy0 == sy1 - sy0) blit(mb, d, dx0, dy0, s, sx0, sy0, sx1, sy1, key);
        else stretch(mb, d, dx0, dy0, dx1, dy1, s, sx0, sy0, sx1, sy1, key);
        trace_pix(mb, "blt", d, dx0, dy0, dx1, dy1, s, sx0, sy0, key, R32(c->esp));
    }
    if (d->primary) present_primary();
    STDRET(6, DD_OK);
}
// IDirectDrawSurface::BltFast(this, x, y, src, srcRect, trans)
static void surf_bltfast(CPU *c) {
    HOSTFN;
    Obj *d = obj_of(ARG(1), OBJ_SURF), *s = obj_of(ARG(4), OBJ_SURF);
    if (!d || !s) STDRET(6, DDERR_INVALIDPARAMS);
    int sx0, sy0, sx1, sy1;
    get_rect(mb, ARG(5), s, &sx0, &sy0, &sx1, &sy1);
    int key = ((ARG(6) & 1u) && s->has_ck) ? (int)(s->ck_lo & 0xff) : -1;   // DDBLTFAST_SRCCOLORKEY
    blit(mb, d, (int32_t)ARG(2), (int32_t)ARG(3), s, sx0, sy0, sx1, sy1, key);
    trace_pix(mb, "fast", d, (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(2) + sx1 - sx0, (int32_t)ARG(3) + sy1 - sy0, s, sx0, sy0, key, R32(c->esp));
    if (d->primary) present_primary();
    STDRET(6, DD_OK);
}
// IDirectDrawSurface::Flip(this, target, flags): swap the primary's pixels with its back buffer's.
static void surf_flip(CPU *c) {
    HOSTFN;
    g_last_flip_ms = SDL_GetTicks();
    hk_ws_flip();
    Obj *p = obj_of(ARG(1), OBJ_SURF);
    if (p && p->attached) {
        Obj *b = &g_obj[p->attached];
        uint32_t t = p->pixels; p->pixels = b->pixels; b->pixels = t;
    }
    present_primary();
    STDRET(3, DD_OK);
}
// IDirectDrawSurface::GetAttachedSurface(this, caps, out)
static void surf_get_attached(CPU *c) {
    HOSTFN;
    Obj *p = obj_of(ARG(1), OBJ_SURF);
    if (!p || !p->attached) STDRET(3, DDERR_NOTFOUND);
    g_obj[p->attached].refs++;
    W32(ARG(3), g_obj[p->attached].guest);
    STDRET(3, DD_OK);
}
// IDirectDrawSurface::Lock(this, rect, desc, flags, event)
static void surf_lock(CPU *c) {
    HOSTFN;
    Obj *s = obj_of(ARG(1), OBJ_SURF);
    if (!s) STDRET(5, DDERR_INVALIDPARAMS);
    fill_desc(mb, ARG(3), s);
    W32(ARG(3) + 4, R32(ARG(3) + 4) | DDSD_LPSURFACE);
    if (ARG(2)) W32(ARG(3) + 36, s->pixels + (uint32_t)((int32_t)R32(ARG(2) + 4) * s->pitch + (int32_t)R32(ARG(2))));
    STDRET(5, DD_OK);
}
static void surf_unlock(CPU *c) {
    HOSTFN;
    Obj *s = obj_of(ARG(1), OBJ_SURF);
    if (s && s->primary) present_primary();
    STDRET(2, DD_OK);
}
static void surf_get_desc(CPU *c) {
    HOSTFN;
    Obj *s = obj_of(ARG(1), OBJ_SURF);
    if (!s) STDRET(2, DDERR_INVALIDPARAMS);
    fill_desc(mb, ARG(2), s);
    STDRET(2, DD_OK);
}
// IDirectDrawSurface::SetColorKey(this, flags, DDCOLORKEY {low, high})
static void surf_set_ck(CPU *c) {
    HOSTFN;
    Obj *s = obj_of(ARG(1), OBJ_SURF);
    if (!s) STDRET(3, DDERR_INVALIDPARAMS);
    if (ARG(3)) { s->has_ck = 1; s->ck_lo = R32(ARG(3)); s->ck_hi = R32(ARG(3) + 4); }
    else s->has_ck = 0;
    STDRET(3, DD_OK);
}
static void surf_set_palette(CPU *c) {
    HOSTFN;
    Obj *s = obj_of(ARG(1), OBJ_SURF);
    Obj *p = ARG(2) ? obj_of(ARG(2), OBJ_PAL) : NULL;
    if (s) s->palette = idx_of(p);
    if (s && s->attached) g_obj[s->attached].palette = idx_of(p);
    STDRET(2, DD_OK);
}
static void surf_get_palette(CPU *c) {
    HOSTFN;
    Obj *s = obj_of(ARG(1), OBJ_SURF);
    if (!s || !s->palette) STDRET(2, 0x887600F0u);   // DDERR_NOPALETTEATTACHED
    g_obj[s->palette].refs++;
    W32(ARG(2), g_obj[s->palette].guest);
    STDRET(2, DD_OK);
}
static void surf_is_lost(CPU *c) { HOSTFN; STDRET(1, DD_OK); }
static void surf_restore(CPU *c) { HOSTFN; STDRET(1, DD_OK); }
static void surf_set_clipper(CPU *c) { HOSTFN; STDRET(2, DD_OK); }
static void surf_get_caps(CPU *c) {
    HOSTFN;
    Obj *s = obj_of(ARG(1), OBJ_SURF);
    W32(ARG(2), s ? s->caps : 0);
    STDRET(2, DD_OK);
}
static void surf_get_pixel_format(CPU *c) {
    HOSTFN;
    uint32_t p = ARG(2);
    memset(mb + p, 0, 32);
    W32(p, 32); W32(p + 4, 0x60u); W32(p + 12, 8);
    STDRET(2, DD_OK);
}
// GetDC/ReleaseDC: the surface becomes a GDI device context (port/hk/gdi.c).
static void surf_get_dc(CPU *c) {
    HOSTFN;
    Obj *s = obj_of(ARG(1), OBJ_SURF);
    if (!s) STDRET(2, DDERR_INVALIDPARAMS);
    W32(ARG(2), hk_gdi_surface_dc(ARG(1)));
    STDRET(2, DD_OK);
}
static void surf_release_dc(CPU *c) {
    HOSTFN;
    Obj *s = obj_of(ARG(1), OBJ_SURF);
    if (s && s->primary) present_primary();
    STDRET(2, DD_OK);
}

// ---------- palette ----------
// IDirectDrawPalette::SetEntries(this, flags, start, count, entries)
static void pal_set_entries(CPU *c) {
    HOSTFN;
    Obj *p = obj_of(ARG(1), OBJ_PAL);
    uint32_t start = ARG(3), n = ARG(4);
    if (!p || start + n > 256) STDRET(5, DDERR_INVALIDPARAMS);
    memcpy(p->rgb[start], mb + ARG(5), n * 4u);
    {   static unsigned cnt; static uint32_t t0; uint32_t now = SDL_GetTicks(); cnt++;
        if (now - t0 >= 5000) { if (getenv("HK_TRACE_DD")) rt_log("[dd] %u palette SetEntries in 5 s\n", cnt); cnt = 0; t0 = now; } }
    // HDKWIN.EXE animates the palette every frame (~25 SetEntries/s) and then flips: presenting here too doubled
    // the presents (each waits for vblank) and slowed the game. The change is shown by the next Flip; only when
    // no Flip came for 60 ms (a fade over a still image) is it presented right away.
    if (g_primary && g_obj[g_primary].palette == idx_of(p) && SDL_GetTicks() - g_last_flip_ms > 60) present_primary();
    STDRET(5, DD_OK);
}
static void pal_get_entries(CPU *c) {
    HOSTFN;
    Obj *p = obj_of(ARG(1), OBJ_PAL);
    uint32_t start = ARG(3), n = ARG(4);
    if (!p || start + n > 256) STDRET(5, DDERR_INVALIDPARAMS);
    memcpy(mb + ARG(5), p->rgb[start], n * 4u);
    STDRET(5, DD_OK);
}

// Host-side access for GDI (port/hk/gdi.c).
int hk_dd_surface_info(uint32_t guest, int *w, int *h, int *pitch, uint32_t *pixels) {
    Obj *s = obj_of(guest, OBJ_SURF);
    if (!s) return 0;
    *w = s->w; *h = s->h; *pitch = s->pitch; *pixels = s->pixels;
    return 1;
}
const uint8_t *hk_dd_primary_palette(void) {
    if (!g_primary || !g_obj[g_primary].palette) return NULL;
    return &g_obj[g_obj[g_primary].palette].rgb[0][0];
}

// ---------- clipper: one window covering the whole screen, nothing to clip ----------
static void clip_set_hwnd(CPU *c) { HOSTFN; STDRET(3, DD_OK); }        // (this, flags, hwnd)
static void clip_set_list(CPU *c) { HOSTFN; STDRET(3, DD_OK); }        // (this, rgndata, flags)
// GetGDISurface(this, out): the primary is the surface GDI draws on (the game pauses with it before dialogs).
static void dd_get_gdi_surface(CPU *c) {
    HOSTFN;
    if (!g_primary) { W32(ARG(2), 0); STDRET(2, 0x887600FFu); }   // DDERR_NOTFOUND
    g_obj[g_primary].refs++;
    W32(ARG(2), g_obj[g_primary].guest);
    STDRET(2, DD_OK);
}
static void dd_flip_to_gdi(CPU *c) { HOSTFN; STDRET(1, DD_OK); }

static void clip_get_hwnd(CPU *c) { HOSTFN; W32(ARG(2), 0x00010010u); STDRET(2, DD_OK); }
static void clip_changed(CPU *c) { HOSTFN; if (ARG(2)) W32(ARG(2), 0); STDRET(2, DD_OK); }

static void build_vtables(void) {
    static const Method dd[] = {
        {0, com_qi, "IDirectDraw::QueryInterface"}, {1, com_addref, "IDirectDraw::AddRef"},
        {2, com_release, "IDirectDraw::Release"}, {4, dd_create_clipper, "IDirectDraw::CreateClipper"},
        {5, dd_create_palette, "IDirectDraw::CreatePalette"}, {6, dd_create_surface, "IDirectDraw::CreateSurface"},
        {10, dd_flip_to_gdi, "IDirectDraw::FlipToGDISurface"}, {11, dd_get_caps, "IDirectDraw::GetCaps"},
        {14, dd_get_gdi_surface, "IDirectDraw::GetGDISurface"}, {12, dd_get_display_mode, "IDirectDraw::GetDisplayMode"},
        {19, dd_restore_mode, "IDirectDraw::RestoreDisplayMode"}, {20, dd_set_coop, "IDirectDraw::SetCooperativeLevel"},
        {21, dd_set_mode, "IDirectDraw::SetDisplayMode"}, {22, dd_wait_vblank, "IDirectDraw::WaitForVerticalBlank"},
        {0, NULL, NULL}};
    static const Method surf[] = {
        {0, com_qi, "Surface::QueryInterface"}, {1, com_addref, "Surface::AddRef"}, {2, com_release, "Surface::Release"},
        {5, surf_blt, "Surface::Blt"}, {7, surf_bltfast, "Surface::BltFast"}, {11, surf_flip, "Surface::Flip"},
        {12, surf_get_attached, "Surface::GetAttachedSurface"}, {14, surf_get_caps, "Surface::GetCaps"},
        {17, surf_get_dc, "Surface::GetDC"}, {20, surf_get_palette, "Surface::GetPalette"},
        {21, surf_get_pixel_format, "Surface::GetPixelFormat"}, {22, surf_get_desc, "Surface::GetSurfaceDesc"},
        {24, surf_is_lost, "Surface::IsLost"}, {25, surf_lock, "Surface::Lock"},
        {26, surf_release_dc, "Surface::ReleaseDC"}, {27, surf_restore, "Surface::Restore"},
        {28, surf_set_clipper, "Surface::SetClipper"}, {29, surf_set_ck, "Surface::SetColorKey"},
        {31, surf_set_palette, "Surface::SetPalette"}, {32, surf_unlock, "Surface::Unlock"}, {0, NULL, NULL}};
    static const Method pal[] = {
        {0, com_qi, "Palette::QueryInterface"}, {1, com_addref, "Palette::AddRef"}, {2, com_release, "Palette::Release"},
        {4, pal_get_entries, "Palette::GetEntries"}, {6, pal_set_entries, "Palette::SetEntries"}, {0, NULL, NULL}};
    static const Method clip[] = {
        {0, com_qi, "Clipper::QueryInterface"}, {1, com_addref, "Clipper::AddRef"}, {2, com_release, "Clipper::Release"},
        {4, clip_get_hwnd, "Clipper::GetHWnd"}, {6, clip_changed, "Clipper::IsClipListChanged"},
        {7, clip_set_list, "Clipper::SetClipList"}, {8, clip_set_hwnd, "Clipper::SetHWnd"}, {0, NULL, NULL}};
    g_vt_dd = make_vtable("IDirectDraw", 24, dd, miss_dd);
    g_vt_surf = make_vtable("IDirectDrawSurface", 39, surf, miss_surf);
    g_vt_pal = make_vtable("IDirectDrawPalette", 7, pal, miss_pal);
    g_vt_clip = make_vtable("IDirectDrawClipper", 9, clip, miss_clip);
}

// DDRAW!DirectDrawCreate(guid, out, outer)
void imp_DDRAW_DirectDrawCreate(CPU *c) {
    HOSTFN;
    trace("DirectDrawCreate");
    if (!g_vt_dd) build_vtables();
    int i = obj_new(OBJ_DD, g_vt_dd);
    W32(ARG(2), g_obj[i].guest);
    rt_log("[dd] DirectDrawCreate -> #%d\n", i);
    STDRET(3, DD_OK);
}
