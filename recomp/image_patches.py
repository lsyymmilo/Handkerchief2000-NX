"""Port-specific changes to HDKWIN.EXE behaviour, applied at recompile time.

Same interface as sh3vita's recomp/image_patches.py (GPL-3.0), which x86rec.py expects:
- IMAGE_PATCHES: bytes replaced in the image before translation (old bytes are checked first).
- ADDR_OVERRIDES: {va: native} the whole function is replaced by a native C function.
- ADDR_WRAPS: {va: native} the recompiled body becomes F_xxx_orig; F_xxx calls the native.
- HOOKS: {va: native} the native runs just before the instruction at va.

Handkerchief starts with no patches: every entry must be justified by a measured need.
The D3DX constants only exist for Silent Hill 3; the values below never match any HDKWIN.EXE code,
so the D3DX fast path in x86rec.py stays disabled.
"""
from dataclasses import dataclass

SPILL = "c->eax=eax;c->ecx=ecx;c->edx=edx;c->ebx=ebx;c->esp=esp;c->ebp=ebp;c->esi=esi;c->edi=edi;"
RELOAD = "eax=c->eax;ecx=c->ecx;edx=c->edx;ebx=c->ebx;esp=c->esp;ebp=c->ebp;esi=c->esi;edi=c->edi;"


@dataclass(frozen=True)
class Patch:
    va: int
    old: bytes
    new: bytes
    why: str


def apply_patches(pe, patches):
    """Replaces the bytes of each patch in pe.data (bytearray); all old bytes are checked before anything changes."""
    plan = []
    for p in patches:
        if len(p.old) != len(p.new):
            raise ValueError(f"patch {p.va:#x}: size change ({p.why})")
        o = pe.off(p.va)
        if o is None:
            raise ValueError(f"patch {p.va:#x}: address not in the image ({p.why})")
        if bytes(pe.data[o:o + len(p.old)]) != p.old:
            raise ValueError(f"patch {p.va:#x}: unexpected bytes (unsupported HDKWIN.EXE?) ({p.why})")
        plan.append((o, p.new))
    for o, new in plan:
        pe.data[o:o + len(new)] = new


def hook_code(native):
    """C code that runs native(c) with the recompiled function's registers, then takes back what it changed."""
    return f"{{ void {native}(CPU *c); {SPILL} {native}(c); {RELOAD} }}"


IMAGE_PATCHES = []
# MSVC CRT memcpy (0x45E4B0) and memmove (0x45F6C0): same prologue (overlap test at +0x14), then copies through
# computed jump tables the recompiler cannot follow (first run stopped on an indirect jump to 0x45F97C).
# Ghidra leaves them unnamed (FUN_...), so they are matched by address. Native versions: port/rt/natives.c.
ADDR_OVERRIDES = {0x45E4B0: "nat_memcpy", 0x45F6C0: "nat_memmove"}
ADDR_WRAPS = {}
# 0x414D0B: in the CConfig constructor, right after the defaults (Reset 0x414D1E, key table 0x414D49); esi = this.
# The port loads the settings saved by the Game / Key Configuration dialogs (hkconfig.txt, port/hk/dialog.c):
# the game itself never stores them.
# Test aid HK_STAGE (port/hk/glue.c): the game scene constructor marks only territory A's first-entry event as done
# (0x4542BC: mov byte [esi+0x70],1; bytes [esi+0x71..0x7A] were zeroed at 0x454296). Starting NEW GAME in another
# territory then runs its first-entry event (0x45525D -> 0x455414) before the player exists ([scene+0x4C] = 0) and
# calls through a null vtable at 0x45717F. The hook runs before 0x4542C0 and, only when HK_STAGE is set, marks
# territories B-J as entered too. Without HK_STAGE it does nothing.
# Native widescreen, milestone 1 (docs/widescreen-plan.md section 7, natives in port/hk/wide.c). Each hook rewrites a
# value the original instruction just stored, so with the default width 640 nothing changes:
# 0x40D1DD after 0x40D1CF (c7 45 f8 80 02 00 00): global camera context 0x4AA330 rect right = screen width.
# 0x4220DC after 0x4220CD (c7 45 f0 80 02 00 00): back-buffer clear rect right in 0x422098.
# 0x45219E after 0x452190 (c7 45 f8 80 02 00 00), screen size 0 path of 0x452093 only (sizes 2/1 jump from 0x45210D /
#   0x45215C to 0x4521A4): view rect right; the follow rect L/R (0x452169 / 0x452177) already copied to [esp]/[esp+8].
# (0x455E16, overlay centring in 0x455D87, and the panel call 0x4556B0 in the ctor 0x455548 are no longer hooked: both
#   belong to the title scene class, vtable 0x472500, drawn 640 wide and pillarboxed by port/hk/wide.c hk_ws_frame_ox.)
# 0x42208E after 0x422088/0x42208B: logs the area size (edx, esi) stored in the camera context by 0x42206D.
# Milestone 2 (sky tiling, panel centring; same rule: nothing changes at width 640):
# 0x447994 sky 0x447929: after the two blits (0x44796F at ebx+640, 0x44798C at ebx after 81 eb 80 02 00 00 at
#   0x44797A), more 640-period copies through 0x408B7B until the clip rect (esi) is covered.
# 0x447AA4 sky variant 0x447A89: edi = x (8b 7b 18 / 03 38 at 0x447A9C/0x447AA2) += centring offset.
# 0x447B08 / 0x447B2C: before the colour fills 0x402CB1, rect L ([ebp-0x38]) / R ([ebp-0x30], written from
#   8d 87 80 02 00 00 at 0x447B1D) widened to the clip rect [[ebp+0x8]].
# 0x447BD1: after the layer blits (x - 640 copies at 0x447BAE / 0x447BC9), more 640-period copies.
# 0x4567F9 / 0x4570E5: calls of the panel constructor 0x403DB9 -> 0x434629 whose panel is not in the list
#   below (x pushed by 6a 08 at 0x4567E8 and 0x4570D4): [esp] += centring offset.
# 0x454AF7 scene draw 0x454908: list [scene+0x38] (ecx, `8b 4e 38` at 0x454AED; x origin 0 from 33 c0 at 0x454AF2) holds
#   the top bar (sprite element at x 320, sprite 0x28008005), an element at (320,160) and the bottom HUD box
#   (vtable 0x46FAF0). The hook records the list; the offsets are applied per child by the next two hooks.
# 0x4135C8 generic list draw 0x4135A0, before `ff 50 04` (child draw; ecx = child, esi = list, [esp+4] = x): only while
#   that HUD list is drawn, its children other than the box += centring offset, and the box's children with x >= 320
#   (fish/money panel, x 0x212) += full extra width (right edge).
# 0x4349A3 box draw 0x434890, before the third background blit (e8 44 cb fc ff, right frame [box+0x7C] at x+520):
#   filler sprite [box+0x78] repeated over the extra width, right frame moved to the right edge, the box's 640-wide
#   rect (box+0x44/0x4C, child clip built by 0x411BED) widened.
# 0x454AD4 same function, list [scene+0x48] (territory title card; eax from 33 c0 at 0x454AD1): += centring offset.
# 0x454AFE after that list draw (ff 53 04 at 0x454AFB): side margins of the top bar and HUD from HDKCON00 (bank 10).
HOOKS = {0x414D0B: "hk_config_loaded", 0x4542C0: "hk_stage_scene_hook",
         0x40D1DD: "hk_ws_global_rect", 0x4220DC: "hk_ws_clear_rect", 0x45219E: "hk_ws_view_rect",
         0x42208E: "hk_ws_area_size",
         0x447994: "hk_ws_sky", 0x447AA4: "hk_ws_sky2_x", 0x447B08: "hk_ws_sky2_fill_l",
         0x447B2C: "hk_ws_sky2_fill_r", 0x447BD1: "hk_ws_sky2_layers",
         0x4567F9: "hk_ws_panel_x", 0x4570E5: "hk_ws_panel_x",
         0x454AF7: "hk_ws_hud_list", 0x4135C8: "hk_ws_list_child",
         0x4349A3: "hk_ws_hud_box", 0x454AD4: "hk_ws_bar_x", 0x454AFE: "hk_ws_bars"}
ABI_NATIVES = set()

FTOL2 = None   # MSVC __ftol is found by its Ghidra name in x86rec.py when present

# Silent Hill 3 only (D3DX dispatch thunks): disabled for Handkerchief.
D3DX_CPU_CHECK = 0xFFFFFFFF
D3DX_STATE = 0
D3DX_TABLE = 0
D3DX_TABLE_DEFAULT = 0
D3DX_TABLE_SLOTS = 0
D3DX_X87 = {}
