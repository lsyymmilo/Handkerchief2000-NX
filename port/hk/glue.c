// Neutral definitions for symbols the sh3vita-derived Win32 layer expects from Silent Hill 3 features
// (virtual puzzle cursor, DirectShow movies, CPU profiler). Handkerchief does not use them.
#include "rt.h"
#include "x86_funcs.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

unsigned g_frame;   // presented frame counter (read by file and timer diagnostics)
uint64_t g_pause_qpc;   // performance-counter ticks spent in the port's pause screen (Vita Start)

int rt_cpu_profile_thread(const char *label) { (void)label; return 0; }
int movie_cocreate(CPU *c, uint32_t clsid, uint32_t riid, uint32_t out, uint32_t *hr) {
    (void)c; (void)clsid; (void)riid; (void)out; (void)hr;
    return 0;
}
int sh3_vcursor_on(void) { return 0; }
void sh3_vcursor_pixels(int *px, int *py) { *px = 0; *py = 0; }
int sh3_vcursor_click(void) { return 0; }

// ---- ASPack import proxy ----
// The Chinese release is ASPack-packed and one CreateFontIndirectA call is redirected through the
// shell: 0x410977 does `call [0x400390]`, the shim at 0x400340 swaps the caller's LOGFONT for the
// built-in one at 0x400300 (lfHeight -12, GBK face "楷体") and jumps to IAT 0x4af794. The static
// recompile never runs the shim, so the slot still holds the shim pointer (header garbage) and the
// game dies on indirect call to 0x00400340 right after entering a stage. Reproduce the shim.
static void hk_proxy_create_font(CPU *c) {
    HOSTFN;
    W32(c->esp + 4, 0x400300u);   // [esp] = return address, [esp+4] = caller's LOGFONT (one arg)
    imp_GDI32_CreateFontIndirectA(c);
}

// Load-time data patches of HDKWIN.EXE (guest memory, after rt_load_exe).
// [0x4A88EC] is the CRT small-block-heap threshold (0x1E0; malloc at 0x45E2E0 sub-allocates blocks below it).
// With sound on, that heap handed out a live sound object a second time (0x45CB29, then 0x459D13).
// A zero threshold sends every allocation to HeapAlloc, i.e. the port's heap, which tracks live blocks.
// HK_STAGE=label (test aid, port only, never set by default): NEW GAME starts in another stage file.
// The game scene (0x454341) passes the table at 0x4899A0 (pointers to "a0", "a0_a", ... "j0_a", NULL) to 0x4255BD
// with flags 1, which builds one stage from entry 0. Entry 0 is pointed at the table's own string for the label,
// so the game's own loader opens System/<label>.HDK. Not a way to play: progress flags of earlier stages are unset.
static void stage_override(uint8_t *const mb) {
    const char *want = getenv("HK_STAGE");
    if (!want || !*want) return;
    const uint32_t table = 0x4899A0u, end = 0x489AD4u;
    if (R32(table) != 0x48A01Cu || strcmp((const char *)mb + 0x48A01Cu, "a0")) {
        rt_log("[patch] HK_STAGE: unexpected stage table, left alone\n");
        return;
    }
    for (uint32_t a = table; a < end && R32(a); a += 4)
        if (!strcasecmp((const char *)mb + R32(a), want)) {
            W32(table, R32(a));
            rt_log("[patch] HK_STAGE: NEW GAME starts in %s (test aid)\n", (const char *)mb + R32(a));
            return;
        }
    rt_log("[patch] HK_STAGE: no stage named %s\n", want);
}

// Hook before 0x4542C0 (recomp/image_patches.py): esi is the game scene being constructed.
void hk_stage_scene_hook(CPU *c) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("HK_STAGE"); on = e && *e; }
    if (!on) return;
    uint8_t *const mb = g_mem;
    for (uint32_t t = 1; t < 10; t++) W8(c->esi + 0x70u + t, 1);
}

void hk_patch_data(void) {
    uint8_t *const mb = g_mem;
    if (R32(0x4A88ECu) == 0x1E0u) W32(0x4A88ECu, 0);
    else rt_log("[patch] unexpected small-block threshold %08x, left alone\n", R32(0x4A88ECu));
    if (R32(0x400390u) == 0x00400340u)
        W32(0x400390u, rt_host_addr(hk_proxy_create_font, "apack_CreateFontIndirectA"));
    else rt_log("[patch] unexpected ASPack proxy slot %08x, left alone\n", R32(0x400390u));
    stage_override(mb);
}
