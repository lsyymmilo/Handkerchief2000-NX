// Native widescreen, milestone 1 (docs/widescreen-plan.md): the game's screen width becomes g_hk_ws_w
// (640 = original behaviour, 848 = fills the Vita's 960x544 at the game's height of 480). The platform sets it
// before rt_run_entry (PC: HK_WIDE, Vita: wide_native in hkport.cfg). With 640 every hook below writes the value
// the original instruction just wrote, so frames stay byte-identical.
// The hooks run just before the instruction at their address (recomp/image_patches.py HOOKS).
#include "rt.h"
#include "plat.h"
#include <stdlib.h>
#include <string.h>

int g_hk_ws_w = 640;

// Layout of the frame being drawn (decided at each Flip, hk_ws_flip below): 1 = the frame is drawn at the game's
// original 640 width and the present centres it between black pillars; 0 = the game draws the full wide screen.
// The hooks below that shift content right by (g_hk_ws_w - 640) / 2 must not run on a 640-only frame: such a
// frame is then shifted twice (once here, once by hk_ws_pillarbox) and its right side falls off the screen -
// that is what the stage-clear screen showed (content at 2x104 px, cut at the right edge).
static int g_ws_stage_drawn;
static int g_ws_pillar = 1;

void hk_wide_set(int w) {
    if (w < 640 || w > HK_WS_MAX || (w & 1)) w = 640;
    g_hk_ws_w = w;
    if (w != 640) rt_log("[ws] native widescreen: screen %dx480\n", w);
}

// Before 0x40D1DD: static init of the global camera context 0x4AA330; [ebp-0x10..-0x4] holds its rect
// (0,0,640,480), written at 0x40D1CF/0x40D1D6. Its view rect 0x4AA340 is the full-screen clip rect.
void hk_ws_global_rect(CPU *c) {
    if (g_ws_pillar) return;   // 640-only frame: keep the original rect (the present centres it)
    uint8_t *const mb = g_mem;
    W32(c->ebp - 0x8u, (uint32_t)g_hk_ws_w);
}

// Before 0x4220DC: 0x422098 clears the back buffer with the rect [ebp-0x18..-0xC] = (0,0,640,480)
// (right written at 0x4220CD).
void hk_ws_clear_rect(CPU *c) {
    if (g_ws_pillar) return;   // 640-only frame: keep the original rect (the present centres it)
    uint8_t *const mb = g_mem;
    W32(c->ebp - 0x10u, (uint32_t)g_hk_ws_w);
}

// Before 0x45219E: 0x452093, screen size 0 only. The view rect (0,32)-(640,352) is in [ebp-0x10..-0x4] (right
// written at 0x452190) and is copied to the arguments after this point. The follow rect (270,168)-(370,256) was
// already copied to the outgoing arguments at 0x452185..0x452188, so its stack copy [esp+0x0]/[esp+0x8] (L/R) is
// shifted, not [ebp-0x20]/[ebp-0x18].
void hk_ws_view_rect(CPU *c) {
    if (g_ws_pillar) return;   // 640-only frame: keep the 640 view; the present centres it
    uint8_t *const mb = g_mem;
    uint32_t off = (uint32_t)(g_hk_ws_w - 640) / 2;
    W32(c->ebp - 0x8u, (uint32_t)g_hk_ws_w);
    W32(c->esp, R32(c->esp) + off);
    W32(c->esp + 0x8u, R32(c->esp + 0x8u) + off);
}

// ---- Pillarbox for the scenes that stay 640 wide ----
// The stage scene (vtable 0x4724B8, ctor near 0x45427E, derived from the scene base 0x451C30; stages and the
// territory title card) is widened. Everything else draws its 640 columns at the left of the wider primary: intro
// logos, the title screen (vtable 0x472500, ctor 0x455548 -> view (0,0,640,480) at 0x4565AA; its logo 0x455D87 and
// menu panel 0x4556B0 are drawn at 640-centred positions), menus, game over. Those frames are shown centred with
// black side pillars. Evidence: the scene draw 0x454908 (esi = this, `mov esi,ecx` at 0x454920) runs hk_ws_bar_x
// every stage frame; HK_WIDE=848 run to A0 saw only vtable 0x4724B8 there (frame 1314 on), none during intro/title.
// The title class is excluded explicitly in case it reaches that draw. The decision is taken at each Flip (the
// game's frame boundary) and kept for the other presents (palette changes, port dialogs, direct blits to the
// primary), so a modal dialog over a stage does not switch the layout.
#define WS_TITLE_SCENE_VT 0x472500u
static uint32_t g_ws_hud_list, g_ws_hud_box;   // HUD list and box being drawn (see hk_ws_hud_list)
int g_hk_in_stage;   // a stage was drawn in the last game frame (any width): used by the Vita crouch tap
void hk_ws_flip(void) {
    g_hk_in_stage = g_ws_stage_drawn;
    if (g_hk_ws_w == 640) { g_ws_stage_drawn = 0; return; }
    const int p = !g_ws_stage_drawn;
    g_ws_stage_drawn = 0;
    if (p != g_ws_pillar) { extern unsigned g_frame; rt_log("[ws] layout %s (frame %u)\n", p ? "pillarbox 640" : "wide"); }
    g_ws_pillar = p;
}
// Offset of the game's 640 columns in the presented frame: 0 (wide or default 640) or (g_hk_ws_w - 640) / 2.
int hk_ws_frame_ox(void) { return g_hk_ws_w != 640 && g_ws_pillar ? (g_hk_ws_w - 640) / 2 : 0; }
// Moves columns [0,640) of a w-wide XRGB frame to [ox,ox+640) and blacks the rest.
void hk_ws_pillarbox(uint32_t *frame, int w, int h, int ox) {
    if (ox <= 0 || 640 + ox > w) return;
    for (int y = 0; y < h; y++) {
        uint32_t *row = frame + y * w;
        memmove(row + ox, row, 640 * 4);
        memset(row, 0, (size_t)ox * 4);
        memset(row + ox + 640, 0, (size_t)(w - 640 - ox) * 4);
    }
}

// Before 0x42208E (diagnostic): 0x42206D just stored the area size (edx = W, esi = H) in the camera context
// +0x40/+0x44. Logged once per distinct size.
void hk_ws_area_size(CPU *c) {
    static uint32_t last_w, last_h;
    if (c->edx == last_w && c->esi == last_h) return;
    last_w = c->edx; last_h = c->esi;
    rt_log("[ws] area W=%u H=%u%s\n", c->edx, c->esi, (int)c->edx < g_hk_ws_w ? " (narrower than the screen)" : "");
}

// ---- Milestone 2: sky and HUD ----

void F_00408b7b(CPU *c);   // 0x4014EC -> 0x408B7B: blit(x, y, clip rect*, flag, src rect*, surface), cdecl

// Calls the game's blit 0x408B7B like `push surface/src/flag/clip/y/x; call; add esp,0x18` does.
static void ws_blit(CPU *c, uint32_t x, uint32_t y, uint32_t clip, uint32_t flag, uint32_t src, uint32_t surf) {
    uint8_t *const mb = g_mem;
    const uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx;
    uint32_t sp = c->esp;
    sp -= 4; W32(sp, surf); sp -= 4; W32(sp, src); sp -= 4; W32(sp, flag);
    sp -= 4; W32(sp, clip); sp -= 4; W32(sp, y); sp -= 4; W32(sp, x);
    sp -= 4; W32(sp, 0x4014ECu);   // return address (not used by the recompiled ret)
    c->esp = sp;
    F_00408b7b(c);
    c->esp += 0x18;                // cdecl: caller pops the 6 arguments
    c->eax = eax; c->ecx = ecx; c->edx = edx;
}

// Extra copies of a layer with a horizontal period of 640 px: the original drew it at x_lo and x_lo + 640; add
// copies until [clip.L, clip.R) is covered. Nothing is drawn when the two copies already cover the clip rect, so
// with the default width 640 the frame is unchanged.
static void ws_tile(CPU *c, int32_t x_lo, uint32_t y, uint32_t clip, uint32_t flag, uint32_t src) {
    uint8_t *const mb = g_mem;
    const int32_t cl = (int32_t)R32(clip), cr = (int32_t)R32(clip + 8);
    const uint32_t surf = R32(0x4AA410u);
    for (int32_t x = x_lo + 1280; x < cr; x += 640) ws_blit(c, (uint32_t)x, y, clip, flag, src, surf);
    for (int32_t x = x_lo - 640; x + 640 > cl; x -= 640) ws_blit(c, (uint32_t)x, y, clip, flag, src, surf);
}

// Before 0x447994 (sky draw 0x447929, vtable 0x46EC78, HdkSKY%02d.BMP): the 640-wide sky was blitted at ebx + 640
// and ebx (0x44796F, 0x44798C after `sub ebx,0x280` at 0x44797A); edi = y, esi = clip rect (camera + 0x10),
// [ebp-0x8] = source rect (from [0x4ABE50]), flag 0.
void hk_ws_sky(CPU *c) {
    if (g_hk_ws_w == 640) return;
    ws_tile(c, (int32_t)c->ebx, c->edi, c->esi, 0, c->ebp - 0x8u);
}

// Sky variant 0x447A89 (vtable 0x46ECB8): a centred picture ([ebp-0x10], drawn at x + [this+0x5C], 0x447B47) with
// colour fills 0x402CB1 on its left (x .. x+[this+0x5C], 0x447B08) and right (.. x+640, 0x447B2C), then two
// scrolling layers drawn at x+s and x+s-640: [ebp-0x20] at (edi, esi) flag 1, [ebp-0x28] at (ebx, [ebp-0x4]) flag 0.
// Before 0x447AA4: edi = x (= [this+0x18] + camera X); centre the 640-wide composition in the screen.
void hk_ws_sky2_x(CPU *c) {
    if (g_hk_ws_w == 640) return;
    c->edi += (uint32_t)(g_hk_ws_w - 640) / 2;
}
// Before 0x447B08 (left fill, rect [ebp-0x38]) and 0x447B2C (right fill): stretch the fills to the clip rect
// ([ebp+0x8] = camera + 0x10 since 0x447AF2).
void hk_ws_sky2_fill_l(CPU *c) {
    uint8_t *const mb = g_mem;
    if (g_hk_ws_w == 640) return;
    W32(c->ebp - 0x38u, R32(R32(c->ebp + 0x8u)));
}
void hk_ws_sky2_fill_r(CPU *c) {
    uint8_t *const mb = g_mem;
    if (g_hk_ws_w == 640) return;
    W32(c->ebp - 0x30u, R32(R32(c->ebp + 0x8u) + 8));
}
// Before 0x447BD1: the four layer blits are done (edi, ebx = the x - 640 copies).
void hk_ws_sky2_layers(CPU *c) {
    uint8_t *const mb = g_mem;
    if (g_hk_ws_w == 640) return;
    const uint32_t clip = R32(c->ebp + 0x8u);
    ws_tile(c, (int32_t)c->edi, c->esi, clip, 1, c->ebp - 0x20u);
    ws_tile(c, (int32_t)c->ebx, R32(c->ebp - 0x4u), clip, 0, c->ebp - 0x28u);
}

// Before the panel constructor calls 0x4556B0 (0x455548, second scene class, x = 0xDC), 0x4567F9 (0x4567CA, x = 8,
// stored in [scene+0xA8] by 0x4532AC) and 0x4570E5 (0x4570B6, x = 8, the territory title card stored in
// [scene+0x68] by 0x455414): [esp] = x (push of the 0x434629 x argument). These panels are not children of the list
// shifted by hk_ws_bar_x, so they are centred here. The bottom HUD panels (0x4345BD, 0x434810) are children of that
// list and are left alone.
void hk_ws_panel_x(CPU *c) {
    if (g_ws_pillar) return;   // 640-only frame (a card drawn on its own): leave its 640 x
    uint8_t *const mb = g_mem;
    W32(c->esp, R32(c->esp) + (uint32_t)(g_hk_ws_w - 640) / 2);
}

// Formerly also before 0x454AF7 (list [scene+0x38], top bar and HUD; now hk_ws_hud_list below, which anchors the HUD
// to the screen edges instead of centring it). The top bar is a sprite element (0x413B64, draw 0x413DBF) at x = 320
// with sprite 0x28008005 at offset -320, i.e. HDKCON00 rows 0..31 at (0,0).
void hk_ws_bar_x(CPU *c) {
    uint8_t *const mb = g_mem;
    if (R32(c->esi) == WS_TITLE_SCENE_VT) return;   // drawn 640 wide, pillarboxed
    g_ws_stage_drawn = 1;
    c->eax += (uint32_t)(g_hk_ws_w - 640) / 2;
}
// Before 0x454AD4 (hk_ws_bar_x): x origin of the list [scene+0x48] (eax = x origin 0, `xor eax,eax` at 0x454AD1), drawn instead
// while [scene+0x68] == 0: the territory title card (fish and name drawn by 0x435FD1 / 0x436039 / 0x436092 at
// 640-centred positions such as (280,88)).

// Sprite bank b: surface [0x4AA3E4 + 4b], rect table [0x4ABD74 + 20b] (int16 x,y,w,h per entry), count
// [0x4ABD7C + 20b] (0x408C75). The top bar is bank 10 entry 5 = HDKCON00 (640x160: top bar in rows 0..31, the
// bottom HUD background in rows 32..159).
#define WS_CON_BANK 10
static void ws_blit_rect(CPU *c, int x, int y, int sx, int sy, int w, int h, uint32_t surf) {
    uint8_t *const mb = g_mem;
    const uint32_t sp0 = c->esp;
    c->esp -= 8;   // the int16 source rect lives on the guest stack during the call
    W16(c->esp, (uint16_t)sx); W16(c->esp + 2, (uint16_t)sy); W16(c->esp + 4, (uint16_t)w); W16(c->esp + 6, (uint16_t)h);
    ws_blit(c, (uint32_t)x, (uint32_t)y, 0x4AA340u, 0, c->esp, surf);
    c->esp = sp0;
}

// Before 0x454AFE: the top bar and the bottom HUD panels (children of the same list) were just drawn, centred by
// hk_ws_bar_x. Fill the side margins from HDKCON00 itself (never from the screen, so no moving sprite or text is
// copied): top bar = its flat left end on the left, plain texture, its bevelled right end at the right edge of the
// screen; HUD = plain background from inside its inner frame. Skipped unless bank 10 entry 5 is the 640x32 bar.
void hk_ws_bars(CPU *c) {
    uint8_t *const mb = g_mem;
    g_ws_hud_list = g_ws_hud_box = 0;
    const int ox = (g_hk_ws_w - 640) / 2;
    if (ox <= 0 || ox > 104 || R32(c->esi) == WS_TITLE_SCENE_VT) return;
    const uint32_t surf = R32(0x4AA3E4u + 4 * WS_CON_BANK), tab = R32(0x4ABD74u + 20 * WS_CON_BANK);
    if (!surf || !tab || (int32_t)R32(0x4ABD7Cu + 20 * WS_CON_BANK) <= 5) return;
    const uint32_t e = tab + 5 * 8;
    if (R16(e) != 0 || R16(e + 2) != 0 || R16(e + 4) != 640 || R16(e + 6) != 32) return;
    // top bar, rows 0..31: [0,ox) <- cols [0,ox); [ox,2ox) <- [ox,2ox) (hides the left end drawn at ox);
    // [640,640+ox) <- plain [432,432+ox); [640+ox,..) <- right end [640-ox,640)
    ws_blit_rect(c, 0, 0, 0, 0, ox, 32, surf);
    ws_blit_rect(c, ox, 0, ox, 0, ox, 32, surf);
    ws_blit_rect(c, 640, 0, 432, 0, ox, 32, surf);
    ws_blit_rect(c, 640 + ox, 0, 640 - ox, 0, ox, 32, surf);
    // The bottom HUD box covers the full width itself (hk_ws_hud_box).
}

// ---- HUD anchored to the screen edges ----
// The list [scene+0x38] drawn at 0x454AFB holds (A0, traced with HK_WIDE=848): an element of vtable 0x46FBB0 at
// (320,160), the top bar sprite element (vtable 0x46E218, x 320) and the bottom HUD box (vtable 0x46FAF0, pos
// (0,352)). The box draw 0x434890 blits three background sprites side by side from its x: [box+0x74] = HDKCON00
// (0,32) 384x128 (left frame), [box+0x78] = (248,32) 136x128 (plain filler, at x+384) and [box+0x7C] = (520,32)
// 120x128 (right frame, at x+520, call 0x4349A3); it then (0x414254) builds its child context at box+0x24 (full
// rect at box+0x44..0x50, absolute, 640 wide, intersected with the parent clip by 0x411BED) and draws its list
// with origin (0,0): the soldier list (vtable 0x46FB68, x 0), the weapon panel (0x46FB98, x 8) and the
// fish/money panel (0x46FB80, x 0x212). Widescreen layout: the box stays at x 0, the filler is repeated, the
// right frame and every box child with x >= 320 move to the right edge (+ g_hk_ws_w - 640); the other top-list
// elements stay centred (+ (g_hk_ws_w - 640) / 2). The generic list draw 0x4135A0 (hook before its child call
// 0x4135C8: ecx = child, esi = list, [esp] = context, [esp+4] = x, [esp+8] = y) applies the offsets.
#define WS_HUD_BOX_VT 0x46FAF0u
// Before 0x454AF7 (replaces hk_ws_bar_x there): eax = 0 stays; only the per-child offsets above are applied.
void hk_ws_hud_list(CPU *c) {
    uint8_t *const mb = g_mem;
    if (R32(c->esi) == WS_TITLE_SCENE_VT) return;   // drawn 640 wide, pillarboxed
    g_ws_stage_drawn = 1;
    g_ws_hud_list = g_hk_ws_w != 640 ? c->ecx : 0;
}
void hk_ws_list_child(CPU *c) {
    uint8_t *const mb = g_mem;
    if (!g_ws_hud_list || g_ws_pillar) return;   // nothing for a 640-only frame: it is centred as a whole
    const uint32_t child = c->ecx;
    if (c->esi == g_ws_hud_list) {
        if (R32(child) == WS_HUD_BOX_VT) { g_ws_hud_box = child; return; }
        W32(c->esp + 4, R32(c->esp + 4) + (uint32_t)(g_hk_ws_w - 640) / 2);
    } else if (c->esi == g_ws_hud_box && (int32_t)R32(child + 0x14) >= 320) {
        W32(c->esp + 4, R32(c->esp + 4) + (uint32_t)(g_hk_ws_w - 640));
    }
}
// Before 0x4349A3 (right frame blit of the box draw 0x434890, esi = box; [esp] = x, [esp+4] = y, [esp+8] = clip, [esp+0xC] =
// flag, [esp+0x10] = source rect, [esp+0x14] = surface): fill the extra width with the filler sprite, move the
// right frame to the right edge, and widen the box's 640-wide rect so its children are not clipped at x 640.
void hk_ws_hud_box(CPU *c) {
    uint8_t *const mb = g_mem;
    if (!g_ws_hud_list || c->esi != g_ws_hud_box) return;
    const int32_t extra = g_hk_ws_w - 640, x = (int32_t)R32(c->esp);
    const uint32_t id = R32(c->esi + 0x78), b = (id >> 26) & 15;
    const uint32_t rect = R32(0x4ABD74u + 20 * b) + (id & 0x7FFF) * 8;
    const int32_t fw = (int16_t)R16(rect + 4);
    if (fw > 0)
        for (int32_t fx = x; fx < x + extra; fx += fw)
            ws_blit(c, (uint32_t)fx, R32(c->esp + 4), R32(c->esp + 8), (id >> 15) & 1, rect, R32(0x4AA3E4u + 4 * b));
    W32(c->esp, (uint32_t)(x + extra));
    const uint32_t l = R32(c->esi + 0x44), r = R32(c->esi + 0x4C);
    if (r - l == 640) W32(c->esi + 0x4C, r + (uint32_t)extra);
}
