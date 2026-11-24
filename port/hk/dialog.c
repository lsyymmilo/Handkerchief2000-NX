// Win32 dialogs for the recompiled Handkerchief, drawn by the port over the game screen and driven by the keyboard
// or the pad. The game's own dialog procedures run unchanged: the port reads the DIALOG template from HDKWIN.EXE,
// sends WM_INITDIALOG / WM_COMMAND / WM_DESTROY, and answers the control functions the game imports
// (Get/SetDlgItemText/Int, CheckDlgButton, SendDlgItemMessageA with button, combo box and list box messages).
//
// Dialogs used by the game (tools/dump_dialogs.py): 1498 "Exit Handkerchief ?" (Esc, modeless), 1491 Game
// Configuration and 1492 Key Configuration (F2, modal), 1490 secret options, 1493/1497/1499 messages and input.
//
// Controls: arrows move the focus between controls (spatially, for a pad); space or X (pad Cross) press the focused
// control (a combo box opens its list); Enter or C (pad Triangle) press the default (OK) button;
// Esc or Z (pad Circle, Select) send IDCANCEL. The game's menu bar (LoadMenuA/SetMenu) is drawn and driven the same
// way; F10 (pad: Select on the pause screen) asks the game to show it.
#include "win32.h"
#include "plat.h"
#include "hk/font_data.h"
#include "hk/cjk_font_data.h"
#include "win32/gbk_table.h"
#include "safe_write.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_DLG 8
#define MAX_CTL 72
#define MAX_ITEMS 160
enum { C_BUTTON, C_EDIT, C_STATIC, C_LISTBOX, C_COMBO, C_OTHER };

typedef struct {
    uint16_t id;
    int cls;
    uint32_t style;
    int x, y, w, h;          // pixels, relative to the dialog client area
    char text[160];
    int checked, enabled, visible;
    int nitems, cursel, top;
    int limit;               // EM_LIMITTEXT (0: the port's 159-character buffer)
    char (*items)[48];
    uint32_t *itemdata;
} Ctl;

typedef struct {
    int used, modal, ended, visible, res;
    uint32_t hwnd, proc, result;
    char title[96];
    int x, y, w, h;          // pixels on the 640x480 screen, including the title bar
    Ctl ctl[MAX_CTL];
    int n, focus;
    int drop, drop_orig;     // the focused combo box shows its list; selection to restore on cancel
} Dlg;

static Dlg g_dlg[MAX_DLG];
#define DLG_HWND(k) (0x00030000u | ((uint32_t)(k) << 8))
#define TITLE_H 18
#define DU_X(v) ((v) * 3 / 2)   // dialog units -> pixels: MS P Gothic 9pt has ~6x12 base units
#define DU_Y(v) ((v) * 3 / 2)

static Dlg *dlg_of(uint32_t hwnd) {
    if ((hwnd & 0xffff00ffu) != 0x00030000u) return NULL;
    unsigned k = (hwnd >> 8) & 0xffu;
    return k < MAX_DLG && g_dlg[k].used ? &g_dlg[k] : NULL;
}
static Ctl *ctl_of_hwnd(uint32_t hwnd) {
    if ((hwnd & 0xffff0000u) != 0x00030000u) return NULL;
    unsigned k = (hwnd >> 8) & 0xffu, i = hwnd & 0xffu;
    if (k >= MAX_DLG || !g_dlg[k].used || i == 0 || (int)i > g_dlg[k].n) return NULL;
    return &g_dlg[k].ctl[i - 1];
}
static Ctl *ctl_by_id(Dlg *d, uint32_t id) {
    if (!d) return NULL;
    for (int i = 0; i < d->n; i++) if (d->ctl[i].id == (uint16_t)id) return &d->ctl[i];
    return NULL;
}
static uint32_t ctl_hwnd(Dlg *d, Ctl *t) { return d->hwnd | (uint32_t)(t - d->ctl + 1); }
int hk_dlg_is(uint32_t hwnd) { return dlg_of(hwnd) || ctl_of_hwnd(hwnd); }

// Topmost visible dialog (the one that gets input).
static Dlg *top_dlg(void) {
    for (int k = MAX_DLG - 1; k > 0; k--) if (g_dlg[k].used && g_dlg[k].visible) return &g_dlg[k];
    return NULL;
}

// ---------- menu bar ----------
// HDKWIN.EXE loads a MENU resource per state (1505 title, 1504 in a stage, 1502 with Network, 1503 the developers'
// editor) and, in full screen, shows it only on demand: a right click (WM_RBUTTONDOWN, window procedure 0x40ED30)
// or WM_INITMENU calls 0x40EEE9, which pauses the game and SetMenu(hwnd, menu); WM_COMMAND (0x40ED65 -> 0x40EF4A)
// hides it again with SetMenu(hwnd, 0) and leaves the command id in the window object for the main loop.
// The port draws the menu set by SetMenu as a bar at the top of the screen; the keyboard or pad picks an item, which
// is posted as WM_COMMAND; cancelling posts another right click, the game's own toggle. F10 (pad: Select on the
// port's pause screen) opens it with a right click.
static void strip_amp(char *s);
#define MENU_HANDLE(res) (0x00050000u | ((res) & 0xffffu))
#define MAX_MTOP 10
#define MAX_MITEM 24
typedef struct { char text[64], key[16]; uint16_t id; int grayed, sep; } MItem;
typedef struct { char text[64]; uint16_t id; int popup, grayed, n; MItem it[MAX_MITEM]; } MTop;
static struct {
    uint32_t res;                 // resource shown, 0 = no menu bar
    int ntop, top, item, open;    // selected title; selected item and whether its drop-down is open
    MTop t[MAX_MTOP];
} g_menu;
static void menu_text(const uint16_t **w, char *out, size_t cap, char *key, size_t kcap) {
    size_t n = 0, k = 0;
    int tab = 0;
    for (; **w; (*w)++) {
        uint16_t ch = **w;
        if (ch == '\t') { tab = 1; continue; }
        char b[2];
        int len = w32_uni_to_gbk(ch, b);          // GBK, exactly as Chinese-edition Windows would do for *A APIs
        if (tab) { if (key && k + (size_t)len < kcap) { key[k++] = b[0]; if (len == 2) key[k++] = b[1]; } }
        else if (n + (size_t)len < cap) { out[n++] = b[0]; if (len == 2) out[n++] = b[1]; }
    }
    (*w)++;
    out[n] = 0;
    if (key && kcap) key[k] = 0;
}
// Standard MENU template: (flags, [id], text) items; MF_POPUP (0x10) opens a sub-list, MF_END (0x80) closes one.
// Nested popups (the editor menu) are flattened into their top-level drop-down.
static int menu_parse(uint32_t res) {
    HOSTFN;
    uint32_t size, ga = w32_resource(res, 4, &size);   // RT_MENU
    if (!ga || size < 8) { rt_log("[menu] menu %u not found\n", res); return 0; }
    const uint16_t *w = (const uint16_t *)(mb + ga + 4), *end = (const uint16_t *)(mb + ga + size);
    memset(&g_menu.t, 0, sizeof g_menu.t);
    g_menu.ntop = 0;
    int lvl = 0, ends[8] = {0};   // ends[l]: the popup that opened level l+1 also ended level l
    while (w < end && lvl >= 0) {
        uint16_t fl = *w++, id = 0;
        if (!(fl & 0x10)) id = *w++;
        char text[64], key[16];
        menu_text(&w, text, sizeof text, key, sizeof key);
        strip_amp(text);
        if (lvl == 0) {
            if (g_menu.ntop < MAX_MTOP) {
                MTop *t = &g_menu.t[g_menu.ntop++];
                snprintf(t->text, sizeof t->text, "%s", text);
                t->id = id; t->popup = (fl & 0x10) != 0; t->grayed = (fl & 3) != 0;
            }
        } else if (!(fl & 0x10) && g_menu.ntop && g_menu.t[g_menu.ntop - 1].n < MAX_MITEM) {
            MTop *t = &g_menu.t[g_menu.ntop - 1];
            MItem *m = &t->it[t->n++];
            snprintf(m->text, sizeof m->text, "%s", text);
            snprintf(m->key, sizeof m->key, "%s", key);
            m->id = id; m->grayed = (fl & 3) != 0; m->sep = !id && !text[0];
            // The port is always full screen and has no DirectPlay: Toggle Display Mode (windowed mode lost the
            // palette: grey picture) and Network are shown greyed out.
            if (id == 40064 || id == 40059) m->grayed = 1;
        }
        if (fl & 0x10) { if (lvl < 7) ends[lvl] = (fl & 0x80) != 0; lvl++; }
        else if (fl & 0x80) { lvl--; while (lvl >= 0 && lvl < 8 && ends[lvl]) { ends[lvl] = 0; lvl--; } }
    }
    return g_menu.ntop > 0;
}
static int menu_shown(void) { return g_menu.res != 0; }
static int mtop_ok(int i) { return i >= 0 && i < g_menu.ntop && !g_menu.t[i].grayed && g_menu.t[i].text[0]; }
static int mitem_ok(const MTop *t, int i) { return i >= 0 && i < t->n && !t->it[i].grayed && !t->it[i].sep; }
static void menu_set(uint32_t hmenu) {
    uint32_t res = (hmenu & 0xffff0000u) == 0x00050000u ? hmenu & 0xffffu : 0;
    rt_log("[menu] SetMenu(%u)\n", res);
    if (res && res != g_menu.res && !menu_parse(res)) res = 0;
    if (res && !menu_shown()) {
        g_menu.top = 0;
        while (g_menu.top < g_menu.ntop && !mtop_ok(g_menu.top)) g_menu.top++;
        g_menu.open = 0; g_menu.item = 0;
    }
    g_menu.res = res;
}
int hk_dlg_active(void) { return top_dlg() != NULL || menu_shown(); }
int hk_dlg_destroy(CPU *c, uint32_t hwnd);   // below: takes a port dialog down (orphaned GAME DATA cleanup)

// ---------- template ----------
static const uint8_t *res_sz(const uint8_t *p, char *out, size_t cap, int *ord) {
    const uint16_t *w = (const uint16_t *)p;
    if (ord) *ord = -1;
    if (w[0] == 0xffff) { if (ord) *ord = w[1]; if (out && cap) out[0] = 0; return p + 4; }
    if (out && cap) w32_utf16_to_gbk(w, out, (int)cap);   // GBK, as Chinese-edition Windows converts resource text
    while (*w) w++;
    return (const uint8_t *)(w + 1);
}
static void strip_amp(char *s) {   // "&OK" -> "OK", "&&" -> "&"
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '&') { if (p[1] == '&') p++; else continue; }
        *o++ = *p;
    }
    *o = 0;
}

// DIALOG and DIALOGEX templates (the About box 1516 and the save slots 1500 are DIALOGEX).
static int parse_template(Dlg *d, uint32_t ga) {
    HOSTFN;
    const uint8_t *p = mb + ga;
    uint32_t style; memcpy(&style, p, 4);
    int ex = style == 0xffff0001u;
    uint16_t n; int16_t cx, cy;
    if (ex) { memcpy(&style, p + 12, 4); memcpy(&n, p + 16, 2); memcpy(&cx, p + 22, 2); memcpy(&cy, p + 24, 2); p += 26; }
    else { memcpy(&n, p + 8, 2); memcpy(&cx, p + 14, 2); memcpy(&cy, p + 16, 2); p += 18; }
    p = res_sz(p, NULL, 0, NULL);                      // menu
    p = res_sz(p, NULL, 0, NULL);                      // class
    p = res_sz(p, d->title, sizeof d->title, NULL);    // title
    if (style & 0x40) { p += ex ? 6 : 2; p = res_sz(p, NULL, 0, NULL); }   // DS_SETFONT: size (weight, italic, charset), face
    d->w = DU_X(cx) + 6; d->h = DU_Y(cy) + TITLE_H + 6;
    if (d->w > 636) d->w = 636;
    if (d->h > 476) d->h = 476;
    d->x = (640 - d->w) / 2; d->y = (480 - d->h) / 2;
    d->n = 0;
    for (int i = 0; i < n && d->n < MAX_CTL; i++) {
        p = mb + ((uint32_t)(p - mb + 3) & ~3u);
        Ctl *t = &d->ctl[d->n];
        memset(t, 0, sizeof *t);
        int16_t x, y, w, h; uint16_t id;
        if (ex) {   // helpID, exStyle, style, x, y, cx, cy, DWORD id
            memcpy(&t->style, p + 8, 4); memcpy(&x, p + 12, 2); memcpy(&y, p + 14, 2); memcpy(&w, p + 16, 2);
            memcpy(&h, p + 18, 2); memcpy(&id, p + 20, 2);
            p += 24;
        } else {
            memcpy(&t->style, p, 4); memcpy(&x, p + 8, 2); memcpy(&y, p + 10, 2); memcpy(&w, p + 12, 2);
            memcpy(&h, p + 14, 2); memcpy(&id, p + 16, 2);
            p += 18;
        }
        int ord; char cls[32];
        p = res_sz(p, cls, sizeof cls, &ord);
        p = res_sz(p, t->text, sizeof t->text, NULL);
        uint16_t extra; memcpy(&extra, p, 2); p += 2 + extra;
        t->cls = ord == 0x80 || !strcasecmp(cls, "button") ? C_BUTTON : ord == 0x81 || !strcasecmp(cls, "edit") ? C_EDIT
               : ord == 0x82 || !strcasecmp(cls, "static") ? C_STATIC : ord == 0x83 || !strcasecmp(cls, "listbox") ? C_LISTBOX
               : ord == 0x85 || !strcasecmp(cls, "combobox") ? C_COMBO : C_OTHER;
        t->id = id;
        t->x = DU_X(x); t->y = DU_Y(y); t->w = DU_X(w);
        t->h = t->cls == C_COMBO ? 20 : DU_Y(h);
        t->enabled = !(t->style & 0x08000000u);
        t->visible = (t->style & 0x10000000u) != 0;
        t->cursel = -1;
        strip_amp(t->text);
        d->n++;
    }
    return 1;
}

static int btype(const Ctl *t) { return (int)(t->style & 15u); }
static int focusable(const Ctl *t) {
    if (!t->visible || !t->enabled) return 0;
    if (t->cls == C_BUTTON) return btype(t) != 7;   // not a group box
    return t->cls == C_COMBO || t->cls == C_LISTBOX || t->cls == C_EDIT;
}

// ---------- drawing ----------
static const HkFont *font(void) { return &hk_fonts[0]; }
static int text_w(const char *s) {
    const HkFont *f = font();
    const HkCjkFont *cf = hk_cjk_font(f->cell);
    int w = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p; ) {
        if (hk_gbk_lead(*p) && p[1]) { w += cf->cell; p += 2; }   // GBK two-byte: full-width cell
        else { uint8_t ch = *p++; w += f->glyphs[(ch < 0x20 ? '?' : ch) - 0x20].advance; }
    }
    return w;
}
static void put_text(uint32_t *fb, int x, int y, const char *s, uint32_t color, int maxx) {
    const HkFont *f = font();
    const HkCjkFont *cf = hk_cjk_font(f->cell);
    for (const uint8_t *p = (const uint8_t *)s; *p; ) {
        if (hk_gbk_lead(*p) && p[1]) {                            // GBK two-byte: full-width 1-bit glyph
            unsigned code = (unsigned)p[0] | ((unsigned)p[1] << 8);
            const HkCjkGlyph *cg = hk_cjk_lookup(cf, code);
            if (x + cf->cell > maxx) break;
            if (cg) {
                const uint32_t *rows = cf->rows + (int)cg->idx * cf->cell;
                for (int gy = 0; gy < cf->cell; gy++) {
                    uint32_t bits = rows[gy];
                    for (int gx = 0; gx < cf->cell; gx++)
                        if (((bits >> gx) & 1u) && x + gx >= 0 && x + gx < 640 && y + gy >= 0 && y + gy < 480)
                            fb[(y + gy) * 640 + x + gx] = color;
                }
            }
            x += cf->cell; p += 2;
            continue;
        }
        uint8_t ch = *p++;
        const HkGlyph *g = &f->glyphs[(ch < 0x20 ? '?' : ch) - 0x20];
        if (x + g->advance > maxx) break;
        for (int gy = 0; gy < f->cell; gy++)
            for (int gx = 0; gx < g->width; gx++)
                if (((f->rows[g->offset + gy] >> gx) & 1u) && x + gx >= 0 && x + gx < 640 && y + gy >= 0 && y + gy < 480)
                    fb[(y + gy) * 640 + x + gx] = color;
        x += g->advance;
    }
}
static void fill(uint32_t *fb, int x, int y, int w, int h, uint32_t c) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < 480; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < 640; i++) fb[j * 640 + i] = c;
}
static void frame(uint32_t *fb, int x, int y, int w, int h, uint32_t tl, uint32_t br) {
    fill(fb, x, y, w, 1, tl); fill(fb, x, y, 1, h, tl);
    fill(fb, x, y + h - 1, w, 1, br); fill(fb, x + w - 1, y, 1, h, br);
}
#define C_FACE 0xc0c0c0u
#define C_LIGHT 0xffffffu
#define C_SHADOW 0x808080u
#define C_DARK 0x000000u
#define C_FOCUS 0xffd000u

// Static text as Windows draws it: word-wrapped to the control width, '\n' breaks lines, aligned left (0), centre (1)
// or right (2); vertically centred when vc is set. A word wider than the control is not split (it may spill over).
// The port's font is wider than the game's MS P Gothic, so a control only one line high is never wrapped: its text
// may spill a little past the control instead of being cut.
static void text_box(uint32_t *fb, int x, int y, int w, int h, const char *s, int align, int vc, uint32_t ink) {
    char lines[8][160];
    int nl = 0, w0 = w;
    if (h < 2 * font()->cell) w = 1 << 20;
    const char *p = s;
    while (*p && nl < 8) {
        char *o = lines[nl];
        int len = 0, last_space = -1, last_cjk = -1;
        const char *q = p, *brk = NULL;
        while (*q && *q != '\n') {
            const uint8_t *u = (const uint8_t *)q;
            int clen = (hk_gbk_lead(u[0]) && u[1]) ? 2 : 1;
            if (len + clen > 158) break;
            if (clen == 2) last_cjk = len;                 // full-width text may break between characters
            if (*q == ' ') last_space = len;
            for (int k = 0; k < clen; k++) o[len + k] = q[k];
            len += clen;
            q += clen;
            o[len] = 0;
            if (text_w(o) > w) {
                if (last_space > 0) { o[last_space] = 0; brk = p + last_space + 1; }
                else if (last_cjk > 0) { o[last_cjk] = 0; brk = p + last_cjk; }
                else brk = p + len;                        // nothing to break at: keep one character
                break;
            }
        }
        if (!brk) { o[len] = 0; brk = *q == '\n' ? q + 1 : q; }
        nl++;
        p = brk;
    }
    int lh = font()->cell, top = vc ? y + (h - nl * lh) / 2 : y;
    for (int i = 0; i < nl; i++) {
        int lw = text_w(lines[i]), tx = align == 1 ? x + (w0 - lw) / 2 : align == 2 ? x + w0 - lw : x;
        if (tx < x - 20 && align != 1) tx = x;
        put_text(fb, tx, top + i * lh, lines[i], ink, 640);
    }
}

// EDIT controls take text: typed on a keyboard while focused (PC), or through the system keyboard (Vita IME),
// opened by pressing the control (Cross). ES_READONLY (0x800) controls only show text.
static Ctl *g_ime_ctl;   // the EDIT the system keyboard is editing

// --- "Delete" on the save list (GAME DATA 1500) ---
// Pressing Delete runs the game's own chain first: 0x432688 asks for confirmation on its dialog 1513 and, on "yes",
// 0x4324a8 deletes User%02d.HDK (and nothing else). The game leaves the slot's name in its list, so the port shows
// the slot list from the disk (SetDlgItemTextA below empties a slot whose .HDK is gone - that is what makes a
// deleted save visibly disappear). When no 1513 confirmation comes up at all, the port asks and deletes the slot's
// four files itself (User%02d.HDK / .ser / .mck / .bak).
static int g_del_ctx;        // 1 while the Delete click runs: the dialog 1497 created then is its confirmation
static int g_del_seen;       // set by dlg_create when that 1497 is created
static int g_del_result;     // its EndDialog result: 1 = the player confirmed, 2 = cancelled
static int g_confirm;        // 1: the port's own confirmation box is up
static int g_confirm_result; // written by hk_dlg_keys: 1 = yes, 2 = no
static int slot_has_save(int slot) {   // a slot counts when any of its files still exists: the game removes
                                       // the .HDK marker itself (title screen: DeleteFileA -> 1) while the .ser
                                       // load data stays, and that slot still loads (so it must be deletable).
    static const char *ext[] = {"HDK", "HDK.ser", "HDK.mck", "HDK.bak"};
    char path[600];
    for (unsigned i = 0; i < sizeof ext / sizeof *ext; i++) {
        snprintf(path, sizeof path, "%s/User/User%02d.%s", g_game_dir, slot - 1, ext[i]);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}
static void slot_del_files(int slot) {
    static const char *ext[] = {"HDK", "HDK.ser", "HDK.mck", "HDK.bak"};
    char path[600];
    for (unsigned i = 0; i < sizeof ext / sizeof *ext; i++) {
        snprintf(path, sizeof path, "%s/User/User%02d.%s", g_game_dir, slot - 1, ext[i]);
        int r = remove(path);
        rt_log("[port] delete %s -> %d\n", path, r);
    }
}
static int editing(const Ctl *t) { return t->cls == C_EDIT && t->enabled && t->visible && !(t->style & 0x800u); }
static int edit_limit(const Ctl *t) { return t->limit > 0 && t->limit < 159 ? t->limit : 159; }
static void set_edit_text(Ctl *t, const char *s) {
    snprintf(t->text, sizeof t->text, "%s", s);
    if ((int)strlen(t->text) > edit_limit(t)) t->text[edit_limit(t)] = 0;
}

static const char *item_text(const Ctl *t, int i) { return i >= 0 && i < t->nitems ? t->items[i] : ""; }

static void draw_ctl(uint32_t *fb, const Dlg *d, const Ctl *t, int focused) {
    int x = d->x + 3 + t->x, y = d->y + TITLE_H + 3 + t->y, w = t->w, h = t->h;
    uint32_t ink = t->enabled ? C_DARK : C_SHADOW;
    int ty = y + (h - font()->cell) / 2;
    switch (t->cls) {
    case C_BUTTON:
        switch (btype(t)) {
        case 7:   // group box
            frame(fb, x, y + 6, w, h - 6, C_SHADOW, C_LIGHT);
            if (t->text[0]) { fill(fb, x + 6, y, text_w(t->text) + 4, 13, C_FACE); put_text(fb, x + 8, y, t->text, ink, x + w); }
            break;
        case 2: case 3: case 4: case 9: {   // check box / radio button
            if (t->style & 0x1000u) {      // BS_PUSHLIKE (save slots 1500): a push button, sunk while checked
                fill(fb, x, y, w, h, t->checked ? 0xe0e0e0u : C_FACE);
                if (t->checked) { frame(fb, x, y, w, h, C_DARK, C_LIGHT); frame(fb, x + 1, y + 1, w - 2, h - 2, C_SHADOW, C_FACE); }
                else { frame(fb, x, y, w, h, C_LIGHT, C_DARK); frame(fb, x + 1, y + 1, w - 2, h - 2, C_FACE, C_SHADOW); }
                text_box(fb, x + 3, y, w - 6, h, t->text, 1, 1, ink);
                if (focused) { frame(fb, x - 2, y - 2, w + 4, h + 4, C_FOCUS, C_FOCUS); frame(fb, x - 3, y - 3, w + 6, h + 6, C_FOCUS, C_FOCUS); }
                break;
            }
            int r = btype(t) == 4 || btype(t) == 9, by = y + (h - 11) / 2;
            if (r) {   // round: rows of a 11x11 circle
                static const uint8_t half[11] = {2, 1, 0, 0, 0, 0, 0, 0, 0, 1, 2};
                for (int j = 0; j < 11; j++) {
                    int k = half[j] + (j == 0 || j == 10 ? 1 : 0);
                    fill(fb, x + k, by + j, 11 - 2 * k, 1, C_LIGHT);
                    fill(fb, x + k, by + j, 1, 1, j < 6 ? C_SHADOW : C_LIGHT);
                    fill(fb, x + 10 - k, by + j, 1, 1, j < 6 ? C_SHADOW : C_LIGHT);
                }
                fill(fb, x + 3, by, 5, 1, C_SHADOW);
                if (t->checked) { fill(fb, x + 4, by + 3, 3, 5, C_DARK); fill(fb, x + 3, by + 4, 5, 3, C_DARK); }
            } else {
                fill(fb, x, by, 11, 11, C_LIGHT);
                frame(fb, x, by, 11, 11, C_SHADOW, C_LIGHT);
                if (t->checked) for (int i = 0; i < 7; i++) { fill(fb, x + 2 + i, by + 2 + i, 1, 1, C_DARK); fill(fb, x + 8 - i, by + 2 + i, 1, 1, C_DARK); }
            }
            put_text(fb, x + 15, ty, t->text, ink, x + w + 40);
            if (focused) frame(fb, x - 2, ty - 1, text_w(t->text) + 19, font()->cell + 2, C_FOCUS, C_FOCUS);
            break;
        }
        default:  // push button
            fill(fb, x, y, w, h, C_FACE);
            frame(fb, x, y, w, h, C_LIGHT, C_DARK);
            frame(fb, x + 1, y + 1, w - 2, h - 2, C_FACE, C_SHADOW);
            put_text(fb, x + (w - text_w(t->text)) / 2, ty, t->text, ink, x + w);
            if (btype(t) == 1) frame(fb, x - 1, y - 1, w + 2, h + 2, C_DARK, C_DARK);
            if (focused) { frame(fb, x - 2, y - 2, w + 4, h + 4, C_FOCUS, C_FOCUS); frame(fb, x - 3, y - 3, w + 6, h + 6, C_FOCUS, C_FOCUS); }
        }
        break;
    case C_STATIC:
        if ((t->style & 0x1fu) == 0x12u) { frame(fb, x, y, w, h, C_SHADOW, C_LIGHT); break; }   // SS_ETCHEDFRAME
        if ((t->style & 0x1fu) >= 3 && (t->style & 0x1fu) <= 9) break;                         // icon, rectangles, frames
        if ((t->style & 0x1fu) == 0xEu) break;                                                 // SS_BITMAP
        text_box(fb, x, y, w, h, t->text, (int)(t->style & 3u), (t->style & 0x200u) != 0, ink);   // SS_CENTERIMAGE: vertically centred
        break;
    case C_EDIT: {
        fill(fb, x, y, w, h, t->enabled ? C_LIGHT : C_FACE);
        frame(fb, x, y, w, h, C_SHADOW, C_LIGHT);
        int a = (int)(t->style & 3u), tw = text_w(t->text);   // ES_CENTER 1, ES_RIGHT 2
        int tx = a == 1 ? x + (w - tw) / 2 : a == 2 ? x + w - 3 - tw : x + 3;
        if (tx < x + 3) tx = x + 3;
        put_text(fb, tx, ty, t->text, C_DARK, x + w - 2);
        if (focused && editing(t)) fill(fb, tx + tw + 1 < x + w - 2 ? tx + tw + 1 : x + w - 3, ty, 1, font()->cell, C_DARK);   // caret
        if (focused) frame(fb, x - 2, y - 2, w + 4, h + 4, C_FOCUS, C_FOCUS);
        break;
    }
    case C_COMBO:
        fill(fb, x, y, w, h, t->enabled ? C_LIGHT : C_FACE);
        frame(fb, x, y, w, h, C_SHADOW, C_LIGHT);
        put_text(fb, x + 3, ty, item_text(t, t->cursel), ink, x + w - 14);
        fill(fb, x + w - 13, y + 2, 11, h - 4, C_FACE);   // drop-down button with a small triangle
        frame(fb, x + w - 13, y + 2, 11, h - 4, C_LIGHT, C_SHADOW);
        for (int i = 0; i < 3; i++) fill(fb, x + w - 11 + i, y + h / 2 - 1 + i, 7 - 2 * i, 1, ink);
        if (focused) frame(fb, x - 2, y - 2, w + 4, h + 4, C_FOCUS, C_FOCUS);
        break;
    case C_LISTBOX: {
        fill(fb, x, y, w, h, C_LIGHT);
        frame(fb, x, y, w, h, C_SHADOW, C_LIGHT);
        int rows = (h - 4) / 14;
        for (int i = 0; i < rows && t->top + i < t->nitems; i++) {
            int it = t->top + i;
            if (it == t->cursel) fill(fb, x + 2, y + 2 + i * 14, w - 4, 14, 0x000080u);
            put_text(fb, x + 3, y + 2 + i * 14, t->items[it], it == t->cursel ? C_LIGHT : C_DARK, x + w - 2);
        }
        if (focused) frame(fb, x - 2, y - 2, w + 4, h + 4, C_FOCUS, C_FOCUS);
        break;
    }
    default: break;
    }
}

// The open list of a combo box, below it (above if it would leave the screen), scrolled to the selection.
static void draw_drop(uint32_t *fb, const Dlg *d, const Ctl *t) {
    int rows = t->nitems < 10 ? t->nitems : 10, rh = 14, w = t->w, h = rows * rh + 4;
    int x = d->x + 3 + t->x, y = d->y + TITLE_H + 3 + t->y + t->h;
    if (y + h > 478) y = d->y + TITLE_H + 3 + t->y - h;
    int first = t->cursel - rows / 2;
    if (first > t->nitems - rows) first = t->nitems - rows;
    if (first < 0) first = 0;
    fill(fb, x, y, w, h, C_LIGHT);
    frame(fb, x, y, w, h, C_DARK, C_DARK);
    for (int i = 0; i < rows; i++) {
        int it = first + i, sel = it == t->cursel;
        if (sel) fill(fb, x + 2, y + 2 + i * rh, w - 4, rh, 0x000080u);
        put_text(fb, x + 4, y + 2 + i * rh, t->items[it], sel ? C_LIGHT : C_DARK, x + w - 2);
    }
}

// Called by the DirectDraw present with the converted 640x480 frame.
#ifdef __vita__
#define HINT_BAR "Left/Right: menu   Cross: open   Circle: close"
#define HINT_DROP "Up/Down: choose   Cross: select   Circle: back"
#define HINT_DLG "D-pad: move   Cross: select   Triangle: OK   Circle: cancel"
#define HINT_LIST "Up/Down: choose   Cross: keep   Circle: undo"
#define HINT_EDIT "Cross: type text   Triangle: OK   Circle: cancel"
#elif defined(__SWITCH__)
#define HINT_BAR "Left/Right: menu   A: open   B/-: close"
#define HINT_DROP "Up/Down: choose   A: select   B: back"
#define HINT_DLG "D-pad: move   A: select   Y: OK   B: cancel"
#define HINT_LIST "Up/Down: choose   A: keep   B: undo"
#define HINT_EDIT "A: type text   Y: OK   B: cancel"
#else
#define HINT_BAR "Left/Right: menu   Enter: open   Esc/F10: close"
#define HINT_DROP "Up/Down: choose   Enter: select   Esc: back"
#define HINT_DLG "Arrows: move   Space: select   Enter: OK   Esc: cancel"
#define HINT_LIST "Up/Down: choose   Enter/Space: keep   Esc: undo"
#define HINT_EDIT "Type text   Backspace: erase   Enter: OK   Esc: cancel"
#endif
static void menu_draw(uint32_t *fb) {
    const int bh = 20;
    fill(fb, 0, 0, 640, bh, C_FACE);
    fill(fb, 0, bh - 1, 640, 1, C_SHADOW);
    int x = 6, top_x = 6, has_dlg = top_dlg() != NULL;
    for (int i = 0; i < g_menu.ntop; i++) {
        MTop *t = &g_menu.t[i];
        int w = text_w(t->text) + 14;
        if (i == g_menu.top && !has_dlg) { fill(fb, x, 1, w, bh - 2, 0x000080u); top_x = x; }
        put_text(fb, x + 7, 3, t->text, i == g_menu.top && !has_dlg ? C_LIGHT : t->grayed ? C_SHADOW : C_DARK, 640);
        x += w;
    }
    if (has_dlg) return;
    if (!g_menu.open || g_menu.top >= g_menu.ntop || !g_menu.t[g_menu.top].popup) {
        const char *hint = HINT_BAR;
        fill(fb, (640 - text_w(hint)) / 2 - 4, 465, text_w(hint) + 8, 15, 0x202020u);
        put_text(fb, (640 - text_w(hint)) / 2, 466, hint, 0xe0e0e0u, 640);
        return;
    }
    MTop *t = &g_menu.t[g_menu.top];
    int w = 0;
    for (int i = 0; i < t->n; i++) { int iw = text_w(t->it[i].text) + (t->it[i].key[0] ? text_w(t->it[i].key) + 30 : 0); if (iw > w) w = iw; }
    w += 34;
    int h = 4, px = top_x + w > 638 ? 638 - w : top_x, py = bh;
    for (int i = 0; i < t->n; i++) h += t->it[i].sep ? 8 : 18;
    fill(fb, px, py, w, h, C_FACE);
    frame(fb, px, py, w, h, C_LIGHT, C_DARK);
    frame(fb, px + 1, py + 1, w - 2, h - 2, C_FACE, C_SHADOW);
    int y = py + 2;
    for (int i = 0; i < t->n; i++) {
        MItem *m = &t->it[i];
        if (m->sep) { fill(fb, px + 4, y + 3, w - 8, 1, C_SHADOW); fill(fb, px + 4, y + 4, w - 8, 1, C_LIGHT); y += 8; continue; }
        int sel = i == g_menu.item;
        uint32_t ink = sel ? C_LIGHT : m->grayed ? C_SHADOW : C_DARK;
        if (sel) fill(fb, px + 3, y, w - 6, 18, 0x000080u);
        put_text(fb, px + 18, y + 2, m->text, ink, px + w);
        if (m->key[0]) put_text(fb, px + w - 12 - text_w(m->key), y + 2, m->key, ink, px + w);
        y += 18;
    }
    const char *hint = HINT_DROP;
    fill(fb, (640 - text_w(hint)) / 2 - 4, 465, text_w(hint) + 8, 15, 0x202020u);
    put_text(fb, (640 - text_w(hint)) / 2, 466, hint, 0xe0e0e0u, 640);
}

// "saving..." / "loading..." feedback while the game writes or reads the save files (set from CreateFileA/CopyFileA
// on the User files; cleared by the game loop when it starts presenting frames again).
const char *g_saving;
void hk_dlg_overlay(uint32_t *fb) {
    if (menu_shown()) menu_draw(fb);
    for (int k = 1; k < MAX_DLG; k++) {
        Dlg *d = &g_dlg[k];
        if (!d->used || !d->visible) continue;
        fill(fb, d->x, d->y, d->w, d->h, C_FACE);
        frame(fb, d->x, d->y, d->w, d->h, C_LIGHT, C_DARK);
        frame(fb, d->x + 1, d->y + 1, d->w - 2, d->h - 2, C_FACE, C_SHADOW);
        fill(fb, d->x + 3, d->y + 3, d->w - 6, TITLE_H - 2, 0x000080u);
        put_text(fb, d->x + 6, d->y + 4, d->title, C_LIGHT, d->x + d->w - 4);
        for (int i = 0; i < d->n; i++) if (d->ctl[i].visible) draw_ctl(fb, d, &d->ctl[i], i == d->focus && d == top_dlg());
        if (d->drop && d->focus >= 0) draw_drop(fb, d, &d->ctl[d->focus]);
        if (d == top_dlg()) {
            const char *hint = d->drop ? HINT_LIST : HINT_DLG;
            if (d->focus >= 0 && editing(&d->ctl[d->focus])) hint = g_ime_ctl ? "System keyboard open" : HINT_EDIT;
            if (d->res == 1500) {   // GAME DATA: which slot is checked, as the game itself reads it (0x432512)
                int slot = 0;
                for (int i = 0; i < d->n; i++)
                    if (d->ctl[i].id >= 0x58Cu && d->ctl[i].id <= 0x595u && d->ctl[i].checked) slot = (int)d->ctl[i].id - 0x58B;
                char buf[64];
                if (d->focus >= 0 && d->ctl[d->focus].id == 40024u)   // Delete has the focus: say what A/Y does here
                    snprintf(buf, sizeof buf, "slot %d/10   A/Y: delete this slot   B: cancel", slot);
                else
                    snprintf(buf, sizeof buf, "slot %d/10   A/Y: save or load here   B: cancel", slot);
                hint = buf;
                if (g_saving) hint = g_saving;   // "saving..." while the game writes the files (set by CreateFileA)
            }
            int hy = d->y + d->h + 4 < 466 ? d->y + d->h + 4 : 466;
            for (int j = 1; j < MAX_DLG; j++)   // over another dialog: at the bottom of the screen instead
                if (&g_dlg[j] != d && g_dlg[j].used && g_dlg[j].visible && hy + 14 > g_dlg[j].y && hy < g_dlg[j].y + g_dlg[j].h) hy = 466;
            fill(fb, (640 - text_w(hint)) / 2 - 4, hy - 1, text_w(hint) + 8, 15, 0x202020u);
            put_text(fb, (640 - text_w(hint)) / 2, hy, hint, 0xe0e0e0u, 640);
        }
    }
    if (g_confirm) {   // the port's delete confirmation, Windows message box look (GBK text: put_text draws it)
        int w = 300, h = 132, x = (640 - w) / 2, y = (480 - h) / 2;
        fill(fb, x, y, w, h, C_FACE);
        frame(fb, x, y, w, h, C_LIGHT, C_DARK);
        frame(fb, x + 1, y + 1, w - 2, h - 2, C_FACE, C_SHADOW);
        fill(fb, x + 3, y + 3, w - 6, TITLE_H - 2, 0x000080u);
        put_text(fb, x + 6, y + 4, "\xC9\xBE\xB3\xFD\xD3\xCE\xCF\xB7\xCA\xFD\xBE\xDD", C_LIGHT, x + w - 4);
        text_box(fb, x + 12, y + TITLE_H + 10, w - 24, h - TITLE_H - 36,
                 "\xC4\xE3\xC8\xB7\xB6\xA8\xD2\xAA\xC9\xBE\xB3\xFD\xD3\xCE\xCF\xB7\xCA\xFD\xBE\xDD\xC2\xF0\xA3\xBF",
                 0, 0, C_DARK);
        fill(fb, x + 8, y + h - 24, w - 16, 17, 0x808080u);
        put_text(fb, x + 12, y + h - 22, "A: \xCA\xC7     B: \xB7\xF1", C_LIGHT, x + w - 8);
    }
}

// Presents the last game frame with the dialogs on top (the game stops flipping while a dialog is up).
static void refresh(void) {
    static uint32_t last;
    uint32_t now = SDL_GetTicks();
    if (now - last < 33) return;
    last = now;
    void hk_dd_present(void);
    hk_dd_present();
}
void hk_dlg_refresh(void) { if (hk_dlg_active()) refresh(); }

// ---------- creation ----------
static uint32_t dlg_send(CPU *c, Dlg *d, uint32_t msg, uint32_t wp, uint32_t lp) {
    return d->proc ? rt_guest_call(c, d->proc, 4, d->hwnd, msg, wp, lp) : 0;
}
// A message posted to a dialog (PostMessageA) and taken by the game's loop: DispatchMessageA hands it to the procedure.
// Dropped when the dialog is gone, as Windows drops messages for a destroyed window.
uint32_t hk_dlg_dispatch(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    Dlg *d = dlg_of(hwnd);
    return d ? dlg_send(c, d, msg, wp, lp) : 0;
}
static void set_focus_default(Dlg *d) {
    d->focus = -1;
    for (int i = 0; i < d->n; i++) if (focusable(&d->ctl[i]) && d->ctl[i].cls == C_BUTTON && btype(&d->ctl[i]) == 1) { d->focus = i; return; }
    for (int i = 0; i < d->n; i++) if (focusable(&d->ctl[i])) { d->focus = i; return; }
}
static Dlg *dlg_create(CPU *c, uint32_t res, uint32_t proc, uint32_t param, int modal) {
    HOSTFN;
    uint32_t size, ga = w32_resource(res, 5, &size);   // RT_DIALOG
    if (!ga) { rt_log("[dlg] dialog %u not found\n", res); return NULL; }
    if (g_del_ctx && res == 1497) { g_del_seen = 1; g_del_result = -1; }   // the game's delete confirmation (0x5d9)
    int k = 1;
    while (k < MAX_DLG && g_dlg[k].used) k++;
    if (k == MAX_DLG) { rt_log("[dlg] too many dialogs\n"); return NULL; }
    Dlg *d = &g_dlg[k];
    for (int i = 0; i < MAX_CTL; i++) { free(d->ctl[i].items); free(d->ctl[i].itemdata); }
    memset(d, 0, sizeof *d);
    if (!parse_template(d, ga)) return NULL;
    d->used = 1; d->modal = modal; d->res = (int)res; d->hwnd = DLG_HWND(k); d->proc = proc;
    for (int i = 0; i < d->n; i++)
        if (d->ctl[i].cls == C_COMBO || d->ctl[i].cls == C_LISTBOX) {
            d->ctl[i].items = calloc(MAX_ITEMS, sizeof *d->ctl[i].items);
            d->ctl[i].itemdata = calloc(MAX_ITEMS, sizeof *d->ctl[i].itemdata);
        }
    set_focus_default(d);
    rt_log("[dlg] create %u \"%s\" (%s) hwnd %08x proc %08x\n", res, d->title, modal ? "modal" : "modeless", d->hwnd, proc);
    d->visible = modal;   // a modeless dialog without WS_VISIBLE waits for ShowWindow
    uint32_t r = dlg_send(c, d, 0x110u, d->n ? ctl_hwnd(d, &d->ctl[d->focus >= 0 ? d->focus : 0]) : 0, param);   // WM_INITDIALOG
    (void)r;
    if (d->focus < 0 || !focusable(&d->ctl[d->focus])) set_focus_default(d);
    return d;
}
static void dlg_free(Dlg *d) {
    plat_text_enable(0);
    for (int i = 0; i < MAX_CTL; i++) { free(d->ctl[i].items); free(d->ctl[i].itemdata); d->ctl[i].items = NULL; d->ctl[i].itemdata = NULL; }
    d->used = 0; d->visible = 0;
    void hk_dd_present(void);
    hk_dd_present();   // remove it from the screen
}

// ---------- input ----------
static void command(CPU *c, Dlg *d, Ctl *t, uint32_t code) {
    dlg_send(c, d, 0x111u, (uint32_t)t->id | (code << 16), ctl_hwnd(d, t));   // WM_COMMAND
}
static void click(CPU *c, Dlg *d, Ctl *t) {
    if (!t->enabled) return;
    rt_log("[dlg] click %u \"%s\"\n", t->id, t->text);
    if (t->cls == C_BUTTON) {
        int b = btype(t);
        if (b == 3) t->checked = !t->checked;           // BS_AUTOCHECKBOX
        else if (b == 9) {                              // BS_AUTORADIOBUTTON: uncheck the rest of its group
            int i = (int)(t - d->ctl), s = i, e = i;
            while (s > 0 && !(d->ctl[s].style & 0x20000u) && d->ctl[s - 1].cls == C_BUTTON && btype(&d->ctl[s - 1]) == 9) s--;
            while (e + 1 < d->n && !(d->ctl[e + 1].style & 0x20000u) && d->ctl[e + 1].cls == C_BUTTON && btype(&d->ctl[e + 1]) == 9) e++;
            for (int j = s; j <= e; j++) d->ctl[j].checked = j == i;
        }
        command(c, d, t, 0);   // BN_CLICKED
    } else if (t->cls == C_COMBO || t->cls == C_LISTBOX) {
        if (t->nitems) { t->cursel = (t->cursel + 1) % t->nitems; command(c, d, t, 1); }   // CBN_SELCHANGE / LBN_SELCHANGE
    }
}
static void change(CPU *c, Dlg *d, Ctl *t, int dir) {   // left/right on a combo or list box
    if (!t->nitems) return;
    int n = t->cursel < 0 ? (dir > 0 ? 0 : t->nitems - 1) : (t->cursel + dir + t->nitems) % t->nitems;
    t->cursel = n;
    if (t->cls == C_LISTBOX) { int rows = (t->h - 4) / 14; if (rows < 1) rows = 1; if (n < t->top) t->top = n; if (n >= t->top + rows) t->top = n - rows + 1; }
    command(c, d, t, 1);
}
// Spatial focus move for a pad: the next control must lie past the focused one's edge in that direction; controls
// that overlap it across the direction (same row or column) win, then the nearest.
static void move_focus(Dlg *d, int dx, int dy) {
    if (d->focus < 0) { set_focus_default(d); return; }
    Ctl *f = &d->ctl[d->focus];
    int best = -1, bestv = 1 << 30;
    for (int i = 0; i < d->n; i++) {
        Ctl *t = &d->ctl[i];
        if (i == d->focus || !focusable(t)) continue;
        int tx = t->x + t->w / 2, ty = t->y + t->h / 2, along, gap, overlap;
        if (dx) {
            along = dx < 0 ? f->x - tx : tx - (f->x + f->w);
            if (along < -f->w / 2 || (dx < 0 ? tx >= f->x + f->w / 4 : tx <= f->x + f->w * 3 / 4)) continue;
            overlap = t->y < f->y + f->h && f->y < t->y + t->h;
            gap = overlap ? 0 : abs(ty - (f->y + f->h / 2));
        } else {
            along = dy < 0 ? f->y - ty : ty - (f->y + f->h);
            if (along < 0) continue;
            overlap = t->x < f->x + f->w && f->x < t->x + t->w;
            gap = overlap ? abs(t->x - f->x) / 4 : abs(tx - (f->x + f->w / 2));
        }
        int v = (overlap ? 0 : 1 << 20) + (along > 0 ? along : 0) + 2 * gap;
        if (v < bestv) { bestv = v; best = i; }
    }
    if (best >= 0) { d->focus = best; rt_log("[dlg] focus id %u \"%s\"\n", d->ctl[best].id, d->ctl[best].text); }
}
// K_OK presses the focused control (space, X, pad Cross); K_DEFAULT is Enter (pad Triangle): the dialog's default
// button (OK), wherever the focus is.
enum { K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8, K_OK = 16, K_CANCEL = 32, K_DEFAULT = 64 };
// GAME DATA (1500, save/load slots) also runs through the generic navigation below. Its own game-side loop polls
// GetKeyState, but on the port that state never reaches it usefully (the loop presents no frames and its debounce
// swallows repeats), while the generic path does everything the dialog needs: arrows move the focus between the
// slot radio list (0x58C..0x595) and the OK/Cancel/Rename/Delete column, OK presses the focused control (a slot
// radio checks it, the OK button runs save/load, which opens the naming dialog with the EDIT soft keyboard),
// Y/Enter press the default (OK) button and B/'-' send IDCANCEL.
// GAME DATA (1500) again: the game reads the selected slot purely through IsDlgButtonChecked over the radio ids
// 0x58C..0x595 (0x432512), and the slot index drives which User%02d.HDK is saved (0x426790). Focus alone changes
// nothing there, so the port keeps focus and selection together: arrows move focus AND check that radio (the group
// unchecks the rest, exactly as CheckRadioButton would), and OK/Enter first presses the focused control when it is
// a slot, so IsDlgButtonChecked reports it before the game's save runs.
static void game_data_sync(Dlg *d) {
    const uint32_t FIRST = 0x58Cu, LAST = 0x595u;
    for (int i = 0; i < d->n; i++)
        if (d->ctl[i].id >= FIRST && d->ctl[i].id <= LAST && d->ctl[i].checked) return;   // one is already checked
    for (int i = 0; i < d->n; i++)
        if (d->ctl[i].id >= FIRST && d->ctl[i].id <= LAST) { d->ctl[i].checked = 1; return; }
}
static void dlg_input(CPU *c, int keys) {
    Dlg *d = top_dlg();
    if (!d) return;
    if (d->res == 1500) {   // keep the slot radio in step with the focus movement
        game_data_sync(d);
        Ctl *f0 = d->focus >= 0 ? &d->ctl[d->focus] : NULL;
        if (f0 && f0->cls == C_BUTTON && f0->id == 40024u && (keys & (K_OK | K_DEFAULT))) {
            // Delete (id 40024 = 0x9C58, the only command the game answers with a file deletion). A works, and so
            // does Y: on this button Y means "press it", as players expect (Y is the OK button everywhere else,
            // but pressing it here used to save/load instead, which read as "delete does nothing").
            int slot = 0;
            for (int i = 0; i < d->n; i++)
                if (d->ctl[i].id >= 0x58Cu && d->ctl[i].id <= 0x595u && d->ctl[i].checked)
                    slot = (int)d->ctl[i].id - 0x58B;
            rt_log("[dlg] delete pressed (slot %d, save %s)\n", slot, slot && slot_has_save(slot) ? "present" : "absent");
            if (slot && slot_has_save(slot)) {
                void w32_purge_confirm_commands(void);   // port/win32/user32.c
                void w32_block_confirm_keys(uint32_t ms);
                w32_purge_confirm_commands();   // the 1500 loop posted 0x9CA4 when this very press went down;
                g_del_ctx = 1; g_del_seen = 0; g_del_result = -1;   // dispatched later it would load this slot
                click(c, d, f0);   // the game's own chain; its confirmation is dialog 1497 (CWnd::MessageBox, 0x4104ab)
                w32_purge_confirm_commands();   // and again: the confirmation dialog loop dispatches the queue
                g_del_ctx = 0;
                if (g_del_seen) {
                    if (g_del_result == 1) {   // the player confirmed: the game took User%02d.HDK; drop the rest
                        slot_del_files(slot);
                        w32_block_confirm_keys(800);   // the still-held key must not read as a new "confirm"
                        rt_log("[dlg] delete: slot %d removed by the game (+ port finished the slot)\n", slot);
                        for (int i = 0; i < d->n; i++)
                            if (d->ctl[i].id >= 0x58Cu && d->ctl[i].id <= 0x595u && (int)d->ctl[i].id - 0x58B == slot)
                                d->ctl[i].text[0] = 0;
                    } else {
                        rt_log("[dlg] delete: cancelled on the game's confirm (result %d)\n", g_del_result);
                    }
                } else {
                    void w32_modal_step(CPU *c);   // port/win32/user32.c
                    g_confirm = 1; g_confirm_result = 0;
                    rt_log("[dlg] delete: no 1497 from the game, port asks for slot %d\n", slot);
                    while (g_confirm) {
                        w32_purge_confirm_commands();   // keep 0x9CA4/0x9CA5 out while the box is up
                        w32_modal_step(c);
                        refresh();
                        SDL_Delay(10);
                    }
                    if (g_confirm_result == 1) {
                        slot_del_files(slot);
                        w32_block_confirm_keys(800);   // the still-held key must not read as a new "confirm"
                        rt_log("[dlg] delete: slot %d cleared by the port\n", slot);
                    }
                    refresh();
                }
            }
            return;
        }
        if (f0 && f0->cls == C_BUTTON && btype(f0) == 9) {
            int updown = (keys & (K_UP | K_DOWN)) != 0;
            if (updown) {
                int before = d->focus;
                move_focus(d, 0, (keys & K_UP) ? -1 : 1);
                if (before != d->focus) {
                    Ctl *nf = &d->ctl[d->focus];
                    if (nf->cls == C_BUTTON && btype(nf) == 9) {
                        int i = (int)(nf - d->ctl), s = i, e = i;
                        while (s > 0 && !(d->ctl[s].style & 0x20000u) && d->ctl[s - 1].cls == C_BUTTON && btype(&d->ctl[s - 1]) == 9) s--;
                        while (e + 1 < d->n && !(d->ctl[e + 1].style & 0x20000u) && d->ctl[e + 1].cls == C_BUTTON && btype(&d->ctl[e + 1]) == 9) e++;
                        for (int j = s; j <= e; j++) d->ctl[j].checked = j == i;
                        return;
                    }
                }
                return;
            }
        }
        if (f0 && f0->cls == C_BUTTON && btype(f0) == 9 && (keys & K_OK) && !(keys & K_DEFAULT)) {
            // A on a slot: select it (visual + IsDlgButtonChecked state), but never treat it as the save action:
            // the game's procedure has no BN_CLICKED branch for the slot radios (0x4325ec chain), so a slot click
            // does nothing; save/load runs only through the OK button (id 1, wp=1 -> 0x43271a).
            int i = (int)(f0 - d->ctl), s = i, e = i;
            while (s > 0 && !(d->ctl[s].style & 0x20000u) && d->ctl[s - 1].cls == C_BUTTON && btype(&d->ctl[s - 1]) == 9) s--;
            while (e + 1 < d->n && !(d->ctl[e + 1].style & 0x20000u) && d->ctl[e + 1].cls == C_BUTTON && btype(&d->ctl[e + 1]) == 9) e++;
            for (int j = s; j <= e; j++) d->ctl[j].checked = j == i;
            return;
        }
    }
    Ctl *f = d->focus >= 0 ? &d->ctl[d->focus] : NULL;
    if (f && d->drop) {   // an open combo box list: arrows choose, OK keeps the choice, cancel restores the old one
        if (keys & K_CANCEL) { f->cursel = d->drop_orig; d->drop = 0; return; }
        if (keys & (K_OK | K_DEFAULT)) { d->drop = 0; if (f->cursel != d->drop_orig) command(c, d, f, 1); return; }   // CBN_SELCHANGE
        int step = (keys & (K_UP | K_LEFT)) ? -1 : (keys & (K_DOWN | K_RIGHT)) ? 1 : 0;
        if (step && f->nitems) {
            int n = f->cursel + step;
            f->cursel = n < 0 ? 0 : n >= f->nitems ? f->nitems - 1 : n;
        }
        return;
    }
    if (keys & K_CANCEL) {
        if (d->res == 1500) {   // GAME DATA: its window procedure only knows the game's own cancel command
            HOSTFN;
            uint32_t obj = R32(0x4aca9cu), slot = R32(0x4ac97cu);   // the dialog object and selected slot, as the game loop keeps them
            uint32_t r = dlg_send(c, d, 0x111u, 0x9CA5u, 0);
            rt_log("[dlg] cancel 1500 -> %u (obj %x slot %u)\n", r, obj, slot);
            if (!obj) {   // the loop already exited once (0x432704 cleared it) but the port dialog is still up:
                hk_dlg_destroy(c, d->hwnd);   // take it down ourselves, as the game's 0x432a32 cleanup would have
                rt_log("[dlg] cancel 1500: dialog was orphaned, destroyed by the port\n");
            }
            return;
        }
        Ctl *cancel = ctl_by_id(d, 2);
        dlg_send(c, d, 0x111u, 2u, cancel ? ctl_hwnd(d, cancel) : 0);   // IDCANCEL
        return;
    }
    if (keys & K_DEFAULT) {   // the default push button (BS_DEFPUSHBUTTON), else OK (id 1), else the focused control
        Ctl *ok = NULL;
        for (int i = 0; i < d->n && !ok; i++)
            if (d->ctl[i].cls == C_BUTTON && btype(&d->ctl[i]) == 1 && d->ctl[i].visible && d->ctl[i].enabled) ok = &d->ctl[i];
        if (!ok) ok = ctl_by_id(d, 1);
        // GAME DATA (1500): Y/Enter ALWAYS presses the OK button - on a slot too. The slot selection has already
        // been kept in step by the arrow movement above, so the game saves/loads the highlighted slot.
        if (ok && ok->enabled && ok->visible) { click(c, d, ok); return; }
        keys |= K_OK;
    }
    if (keys & K_OK) {
        if (f && editing(f) && !plat_has_keyboard()) {
            const char *title = d->title;   // the group box around the control names it ("SAVE DATA" in 1499)
            for (int i = 0; i < d->n; i++) {
                Ctl *g = &d->ctl[i];
                if (g->cls == C_BUTTON && btype(g) == 7 && g->text[0] && f->x >= g->x && f->y >= g->y &&
                    f->x + f->w <= g->x + g->w && f->y + f->h <= g->y + g->h) title = g->text;
            }
            if (plat_text_open(title, f->text, edit_limit(f))) g_ime_ctl = f;
            return;
        }
        if (f && f->cls == C_COMBO) { if (f->enabled && f->nitems) { d->drop = 1; d->drop_orig = f->cursel; } return; }
        if (f) click(c, d, f);
        return;
    }
    int lr = (keys & K_LEFT) ? -1 : (keys & K_RIGHT) ? 1 : 0;
    if (lr && f && f->cls == C_LISTBOX) { change(c, d, f, lr); return; }
    if (f && f->cls == C_LISTBOX && (keys & (K_UP | K_DOWN))) {
        int nf = d->focus;
        move_focus(d, 0, (keys & K_UP) ? -1 : 1);
        if (nf == d->focus) change(c, d, f, (keys & K_UP) ? -1 : 1);
        return;
    }
    if (keys & K_UP) move_focus(d, 0, -1);
    else if (keys & K_DOWN) move_focus(d, 0, 1);
    else if (lr) move_focus(d, lr, 0);
}
#define HWND_MAIN_DLG 0x00010010u   // the game's window (port/win32/user32.c HWND_MAIN)
static void menu_input(int keys) {
    if (keys & K_DEFAULT) keys |= K_OK;
    MTop *t = g_menu.top < g_menu.ntop ? &g_menu.t[g_menu.top] : NULL;
    if (!t) return;
    int lr = (keys & K_LEFT) ? -1 : (keys & K_RIGHT) ? 1 : 0;
    if (keys & K_CANCEL) {
        if (g_menu.open) g_menu.open = 0;
        else w32_post_message(HWND_MAIN_DLG, 0x204u, 0x2u, 0);   // right click again: the game hides the menu
        return;
    }
    if (lr) {
        int i = g_menu.top;
        for (int k = 0; k < g_menu.ntop; k++) { i = (i + lr + g_menu.ntop) % g_menu.ntop; if (mtop_ok(i)) break; }
        g_menu.top = i;
        g_menu.item = 0;
        while (g_menu.open && g_menu.item < g_menu.t[i].n && !mitem_ok(&g_menu.t[i], g_menu.item)) g_menu.item++;
        return;
    }
    if (!g_menu.open) {
        if (keys & (K_OK | K_DOWN)) {
            if (!t->popup) { if (keys & K_OK) { rt_log("[menu] command %u\n", t->id); w32_post_message(HWND_MAIN_DLG, 0x111u, t->id, 0); } return; }
            g_menu.open = 1; g_menu.item = 0;
            while (g_menu.item < t->n && !mitem_ok(t, g_menu.item)) g_menu.item++;
        }
        return;
    }
    if (keys & (K_UP | K_DOWN)) {
        int dir = (keys & K_UP) ? -1 : 1, i = g_menu.item;
        for (int k = 0; k < t->n; k++) { i = (i + dir + t->n) % t->n; if (mitem_ok(t, i)) break; }
        g_menu.item = i;
        return;
    }
    if ((keys & K_OK) && mitem_ok(t, g_menu.item)) {
        uint16_t id = t->it[g_menu.item].id;
        g_menu.open = 0;
        rt_log("[menu] command %u (%s)\n", id, t->it[g_menu.item].text);
        w32_post_message(HWND_MAIN_DLG, 0x111u, id, 0);   // WM_COMMAND from a menu: the game hides the menu itself
    }
}

// Key transitions while a dialog is up: called by USER32's message functions instead of posting WM_KEYDOWN.
// Several keys pressed together (pad Cross sends X and space) count as one action.
void hk_dlg_keys(CPU *c, const uint8_t *pressed_vks, int n) {
    Dlg *d = top_dlg();
    if (g_confirm) {   // the port's delete confirmation owns the pad: A/Y = yes, B = no
        for (int i = 0; i < n; i++)
            switch (pressed_vks[i]) {
            case 0x20: case 'X': case 0x0D: case 'C': g_confirm_result = 1; g_confirm = 0; refresh(); return;
            case 0x1B: case 'Z': g_confirm_result = 2; g_confirm = 0; refresh(); return;
            default: break;
            }
        refresh();
        return;
    }
    if (g_ime_ctl) {   // the system keyboard is up: wait for it, the pad belongs to it
        char buf[160];
        int st = plat_text_status(buf, sizeof buf);
        if (st) {
            Ctl *t = g_ime_ctl;
            g_ime_ctl = NULL;
            if (st == 1 && d && t >= d->ctl && t < d->ctl + d->n) {
                set_edit_text(t, buf);
                command(c, d, t, 0x300u);   // EN_CHANGE
            }
            rt_log("[dlg] system keyboard closed (%s)\n", st == 1 ? "text accepted" : "cancelled");
        }
        refresh();
        return;
    }
    Ctl *f = d && d->focus >= 0 ? &d->ctl[d->focus] : NULL;
    int typing = f && editing(f) && plat_has_keyboard();
    plat_text_enable(typing);
    if (typing) {   // PC: typed characters go into the focused EDIT; letters and space no longer act as dialog keys
        char buf[64];
        int m = plat_text_typed(buf, sizeof buf), changed = 0;
        for (int i = 0; i < m; i++) {
            size_t len = strlen(f->text);
            if (buf[i] == '\b') { if (len) { f->text[len - 1] = 0; changed = 1; } }
            else if ((int)len < edit_limit(f)) { f->text[len] = buf[i]; f->text[len + 1] = 0; changed = 1; }
        }
        if (changed) command(c, d, f, 0x300u);   // EN_CHANGE
    }
    int keys = 0;
    for (int i = 0; i < n; i++) {
        if (typing && (pressed_vks[i] == 0x20 || (pressed_vks[i] >= 'A' && pressed_vks[i] <= 'Z'))) continue;
        switch (pressed_vks[i]) {
        case 0x26: keys |= K_UP; break;
        case 0x28: keys |= K_DOWN; break;
        case 0x25: keys |= K_LEFT; break;
        case 0x27: keys |= K_RIGHT; break;
        case 0x20: case 'X': keys |= K_OK; break;
        case 0x0D: case 'C': keys |= K_DEFAULT; break;
        case 0x1B: case 'Z': keys |= K_CANCEL; break;
        default: break;
        }
    }
    if (keys && top_dlg()) dlg_input(c, keys);
    else if (keys && menu_shown()) menu_input(keys);
    refresh();
}
// F10 (pad: Select on the pause screen) asks the game for its menu with a right click; it also closes it.
void hk_menu_key(void) {
    if (top_dlg()) return;
    w32_post_message(HWND_MAIN_DLG, 0x204u, 0x2u, 0);   // WM_RBUTTONDOWN, MK_RBUTTON
}

// ---------- USER32 ----------
#define U(name) void imp_USER32_##name(CPU *c)
void w32_modal_step(CPU *c);   // port/win32/user32.c: one pass of the modal message loop

// CreateDialogParamA(hInstance, template, parent, proc, param)
U(CreateDialogParamA) {
    HOSTFN;
    Dlg *d = dlg_create(c, ARG(2), ARG(4), ARG(5), 0);
    if (d) {
        uint32_t size, ga = w32_resource(ARG(2), 5, &size);
        uint32_t style; memcpy(&style, mb + ga, 4);
        if (style == 0xffff0001u) memcpy(&style, mb + ga + 12, 4);   // DLGTEMPLATEEX
        if (style & 0x10000000u) d->visible = 1;
        refresh();
    }
    STDRET(5, d ? d->hwnd : 0);
}
// ---------- settings file ----------
// The game keeps its settings in a CConfig object built with defaults at every start (constructor 0x414CF6, called
// from 0x4513B2; layout from the dialog procedures 0x414F6C and 0x415242 and CConfig::Serialize 0x414D79):
// +0x04 View Sky, +0x08 View Background, +0x0C Full Effect, +0x10 Full Motion, +0x14 screen size (0 normal,
// 1 middle, 2 small), +0x18 Opening Animation, +0x1C Music On, +0x20 Sound Effects On, +0x24..+0x93 the key table
// (copied from 0x47B548 by Reset), +0x94 Move Down to Crouch, +0x98 Move Up to Jump. HDKWIN.EXE reads and writes no
// file or registry value for it (runs with both dialogs and Exit showed no CreateFileA/RegSetValueExA), so the
// settings were lost at exit. The port saves +0x04..+0x9B to hkconfig.txt in the game folder when Game or Key
// Configuration closes with OK, and loads it into each new CConfig (hook at 0x414D0B, after the constructor's
// defaults; recomp/image_patches.py).
#define CFG_FIRST 0x04u
#define CFG_END 0x9Cu
#define CFG_OBJ 0x4AC844u   // the CConfig the open configuration dialog edits (set by 0x4151CA / 0x415433)
static void cfg_path(char *out, size_t cap) { snprintf(out, cap, "%s/hkconfig.txt", g_game_dir); }
static void cfg_save(uint32_t obj) {
    HOSTFN;
    if (!obj) return;
    char path[600], tmp[640];
    cfg_path(path, sizeof path);
    if (sw_tmp_path(path, tmp, sizeof tmp)) return;
    FILE *f = fopen(tmp, "w");
    if (!f) { rt_log("[config] cannot write %s\n", tmp); return; }
    fprintf(f, "# Handkerchief port: the game's settings (CConfig fields, hex offset=value), written after OK\n");
    for (uint32_t o = CFG_FIRST; o < CFG_END; o += 4) fprintf(f, "%02x=%u\n", o, R32(obj + o));
    int ok = fclose(f) == 0;
    if (!ok || sw_commit(path)) { rt_log("[config] cannot save %s\n", path); return; }
    rt_log("[config] saved %s (music %u, sound %u, screen %u)\n", path, R32(obj + 0x1C), R32(obj + 0x20), R32(obj + 0x14));
}
// Hook at 0x414D0B (esi = the new CConfig). Unknown offsets and implausible values are ignored, field by field.
void hk_config_loaded(CPU *c) {
    HOSTFN;
    uint32_t obj = c->esi;
    char path[600], line[64];
    cfg_path(path, sizeof path);
    sw_recover(path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned o, v;
        if (line[0] == '#' || sscanf(line, "%x=%u", &o, &v) != 2) continue;
        if (o < CFG_FIRST || o >= CFG_END || (o & 3u) || v > 0xFFFFu) continue;
        if (o == 0x14 && v > 2) continue;
        W32(obj + o, v);
        n++;
    }
    fclose(f);
    rt_log("[config] loaded %d values from %s (music %u, sound %u)\n", n, path, R32(obj + 0x1C), R32(obj + 0x20));
}

// DialogBoxParamA(hInstance, template, parent, proc, param): runs its own message loop until EndDialog.
U(DialogBoxParamA) {
    HOSTFN;
    Dlg *d = dlg_create(c, ARG(2), ARG(4), ARG(5), 1);
    if (!d) STDRET(5, 0xffffffffu);
    while (d->used && !d->ended) {
        w32_modal_step(c);
        if (!d->ended) { refresh(); SDL_Delay(10); }
    }
    uint32_t r = d->result;
    int res = d->res;
    if (d->used) { dlg_send(c, d, 0x2u, 0, 0); dlg_free(d); }   // WM_DESTROY
    if (r == 1 && (res == 1491 || res == 1492)) cfg_save(R32(CFG_OBJ));   // Game / Key Configuration: OK
    rt_log("[dlg] dialog %u closed -> %d\n", ARG(2), (int)r);
    STDRET(5, r);
}
U(EndDialog) {
    HOSTFN;
    Dlg *d = dlg_of(ARG(1));
    if (d) {
        d->result = ARG(2);
        if (g_del_ctx && d->res == 1497) g_del_result = (int)ARG(2);   // delete confirmation: 1 = confirmed, 2 = cancelled
        if (d->modal) d->ended = 1;
        else d->visible = 0;   // a modeless dialog stays until DestroyWindow
    }
    STDRET(2, d ? 1 : 0);
}
int hk_dlg_destroy(CPU *c, uint32_t hwnd) {
    Dlg *d = dlg_of(hwnd);
    if (!d) return 0;
    if (d->modal) { d->ended = 1; return 1; }
    d->visible = 0;
    dlg_send(c, d, 0x2u, 0, 0);   // WM_DESTROY
    dlg_free(d);
    return 1;
}
int hk_dlg_show(uint32_t hwnd, int cmd) {
    Dlg *d = dlg_of(hwnd);
    Ctl *t = d ? NULL : ctl_of_hwnd(hwnd);
    if (!d && !t) return -1;
    int was = d ? d->visible : t->visible;
    if (d) d->visible = cmd != 0; else t->visible = cmd != 0;
    refresh();
    return was;
}
int hk_dlg_enable(uint32_t hwnd, int on) {
    Ctl *t = ctl_of_hwnd(hwnd);
    if (!t) return -1;
    int was = !t->enabled;
    t->enabled = on != 0;
    return was;
}
int hk_dlg_enabled(uint32_t hwnd) { Ctl *t = ctl_of_hwnd(hwnd); return t ? t->enabled : -1; }
int hk_dlg_set_focus(uint32_t hwnd) {
    for (int k = 1; k < MAX_DLG; k++) {
        Dlg *d = &g_dlg[k];
        if (!d->used) continue;
        for (int i = 0; i < d->n; i++) if (ctl_hwnd(d, &d->ctl[i]) == hwnd) { d->focus = i; return 1; }
    }
    return 0;
}

U(GetDlgItem) { HOSTFN; Dlg *d = dlg_of(ARG(1)); Ctl *t = ctl_by_id(d, ARG(2)); STDRET(2, t ? ctl_hwnd(d, t) : 0); }
U(SetDlgItemTextA) {
    HOSTFN;
    Dlg *d = dlg_of(ARG(1));
    Ctl *t = ctl_by_id(d, ARG(2));
    if (t) {
const char *s = ARG(3) ? GSTR(ARG(3)) : "";
        // GAME DATA (1500): the slot list shows what is on the disk. A slot with no file at all stays empty (that is
        // what a deleted slot looks like); a slot whose .HDK label is gone but whose data still loads keeps a name
        // (the game drops the .HDK on the title screen; it would otherwise show an empty row that still loads).
        if (d && d->res == 1500 && t->id >= 0x58Cu && t->id <= 0x595u) {
            int sl = (int)t->id - 0x58B;
            if (!slot_has_save(sl)) s = "";
            else if (!s[0]) {
                static char ph[24];
                snprintf(ph, sizeof ph, "\xB4\xE6\xB5\xB5 %d", sl);   // "存档 %d" (GBK)
                s = ph;
            }
        }
        snprintf(t->text, sizeof t->text, "%s", s);
        if (t->cls != C_EDIT) strip_amp(t->text);
    }
    STDRET(3, t ? 1 : 0);
}
U(GetDlgItemTextA) {
    HOSTFN;
    Ctl *t = ctl_by_id(dlg_of(ARG(1)), ARG(2));
    uint32_t buf = ARG(3), cap = ARG(4), n = 0;
    if (buf && cap) {
        const char *s = t ? (t->cls == C_COMBO && t->cursel >= 0 ? t->items[t->cursel] : t->text) : "";
        n = (uint32_t)strlen(s);
        if (n >= cap) n = cap - 1;
        memcpy(mb + buf, s, n);
        W8(buf + n, 0);
    }
    STDRET(4, n);
}
U(SetDlgItemInt) {
    HOSTFN;
    Ctl *t = ctl_by_id(dlg_of(ARG(1)), ARG(2));
    if (t) { if (ARG(4)) snprintf(t->text, sizeof t->text, "%d", (int32_t)ARG(3)); else snprintf(t->text, sizeof t->text, "%u", ARG(3)); }
    STDRET(4, t ? 1 : 0);
}
U(GetDlgItemInt) {   // (dlg, id, BOOL *translated, signed)
    HOSTFN;
    Ctl *t = ctl_by_id(dlg_of(ARG(1)), ARG(2));
    char *e = NULL;
    long v = t ? strtol(t->text, &e, 10) : 0;
    int ok = t && e && e != t->text;
    if (ARG(3)) W32(ARG(3), ok ? 1u : 0u);
    STDRET(4, ok ? (uint32_t)v : 0u);
}
U(CheckDlgButton) { HOSTFN; Ctl *t = ctl_by_id(dlg_of(ARG(1)), ARG(2)); if (t) t->checked = (int)ARG(3); STDRET(3, t ? 1 : 0); }
U(IsDlgButtonChecked) { HOSTFN; Ctl *t = ctl_by_id(dlg_of(ARG(1)), ARG(2)); STDRET(2, t ? (uint32_t)t->checked : 0u); }
U(CheckRadioButton) {   // (dlg, first, last, check)
    HOSTFN;
    Dlg *d = dlg_of(ARG(1));
    for (int i = 0; d && i < d->n; i++)
        if (d->ctl[i].id >= ARG(2) && d->ctl[i].id <= ARG(3)) d->ctl[i].checked = d->ctl[i].id == ARG(4);
    STDRET(4, d ? 1 : 0);
}

static int add_item(Ctl *t, const char *s, int at) {
    if (!t->items || t->nitems >= MAX_ITEMS) return -1;
    if (at < 0 || at > t->nitems) at = t->nitems;
    memmove(&t->items[at + 1], &t->items[at], (size_t)(t->nitems - at) * sizeof t->items[0]);
    memmove(&t->itemdata[at + 1], &t->itemdata[at], (size_t)(t->nitems - at) * sizeof t->itemdata[0]);
    snprintf(t->items[at], sizeof t->items[0], "%s", s);
    t->itemdata[at] = 0;
    t->nitems++;
    return at;
}
static int find_item(Ctl *t, const char *s, int start, int exact) {
    for (int k = 0; k < t->nitems; k++) {
        int i = (start + 1 + k) % (t->nitems ? t->nitems : 1);
        if (exact ? !strcasecmp(t->items[i], s) : !strncasecmp(t->items[i], s, strlen(s))) return i;
    }
    return -1;
}
// Messages to a control: buttons, combo boxes, list boxes and text. Unknown ones are logged once each.
static uint32_t ctl_message(CPU *c, Dlg *d, Ctl *t, uint32_t msg, uint32_t wp, uint32_t lp) {
    HOSTFN;
    int cb = t->cls == C_COMBO;
    switch (msg) {
    case 0x0C: snprintf(t->text, sizeof t->text, "%s", lp ? GSTR(lp) : ""); return 1;   // WM_SETTEXT
    case 0x0D: {                                                                      // WM_GETTEXT
        uint32_t n = (uint32_t)strlen(t->text);
        if (!wp) return 0;
        if (n >= wp) n = wp - 1;
        memcpy(mb + lp, t->text, n); W8(lp + n, 0);
        return n;
    }
    case 0x0E: return (uint32_t)strlen(t->text);                                     // WM_GETTEXTLENGTH
    case 0xF0: return (uint32_t)t->checked;                                          // BM_GETCHECK
    case 0xF1: t->checked = (int)wp; return 0;                                       // BM_SETCHECK
    case 0xC5: t->limit = (int)wp; return 0;                                         // EM_LIMITTEXT
    case 0xF5: click(c, d, t); return 0;                                             // BM_CLICK
    }
    if (cb || t->cls == C_LISTBOX) {
        uint32_t add = cb ? 0x143 : 0x180, ins = cb ? 0x14A : 0x181, reset = cb ? 0x14B : 0x184, setsel = cb ? 0x14E : 0x186,
                 getsel = cb ? 0x147 : 0x188, count = cb ? 0x146 : 0x18B, gettext = cb ? 0x148 : 0x189,
                 textlen = cb ? 0x149 : 0x18A, getdata = cb ? 0x150 : 0x199, setdata = cb ? 0x151 : 0x19A,
                 find = cb ? 0x14C : 0x18F, findx = cb ? 0x158 : 0x1A2, del = cb ? 0x144 : 0x182,
                 selstr = cb ? 0x14D : 0x18C;
        if (msg == add) return (uint32_t)add_item(t, lp ? GSTR(lp) : "", -1);
        if (msg == ins) return (uint32_t)add_item(t, lp ? GSTR(lp) : "", (int)wp);
        if (msg == reset) { t->nitems = 0; t->cursel = -1; t->top = 0; return 0; }
        if (msg == setsel) { t->cursel = (int)wp < t->nitems ? (int)wp : -1; if (t->cursel >= 0 && t->cursel < t->top) t->top = t->cursel; return (uint32_t)t->cursel; }
        if (msg == getsel) return (uint32_t)t->cursel;
        if (msg == count) return (uint32_t)t->nitems;
        if (msg == gettext) { if ((int)wp >= t->nitems) return 0xffffffffu; strcpy((char *)mb + lp, t->items[wp]); return (uint32_t)strlen(t->items[wp]); }
        if (msg == textlen) return (int)wp < t->nitems ? (uint32_t)strlen(t->items[wp]) : 0xffffffffu;
        if (msg == getdata) return (int)wp < t->nitems ? t->itemdata[wp] : 0xffffffffu;
        if (msg == setdata) { if ((int)wp < t->nitems) t->itemdata[wp] = lp; return 0; }
        if (msg == find) return (uint32_t)find_item(t, lp ? GSTR(lp) : "", (int32_t)wp, 0);
        if (msg == findx) return (uint32_t)find_item(t, lp ? GSTR(lp) : "", (int32_t)wp, 1);
        if (msg == selstr) {   // CB_SELECTSTRING / LB_SELECTSTRING: first item starting with the text after index wp
            int i = find_item(t, lp ? GSTR(lp) : "", (int32_t)wp, 0);
            if (i >= 0) t->cursel = i;
            return (uint32_t)i;
        }
        if (msg == del) {
            if ((int)wp >= t->nitems) return 0xffffffffu;
            memmove(&t->items[wp], &t->items[wp + 1], (size_t)(t->nitems - (int)wp - 1) * sizeof t->items[0]);
            memmove(&t->itemdata[wp], &t->itemdata[wp + 1], (size_t)(t->nitems - (int)wp - 1) * sizeof t->itemdata[0]);
            t->nitems--;
            if (t->cursel >= t->nitems) t->cursel = t->nitems - 1;
            return (uint32_t)t->nitems;
        }
        if (cb && (msg == 0x141 || msg == 0x14F || msg == 0x155 || msg == 0x15B)) return 0;   // limit, show dropdown, extended UI
    }
    static uint32_t said[32]; static int nsaid;
    int known = 0;
    for (int i = 0; i < nsaid; i++) known |= said[i] == msg;
    if (!known && nsaid < 32) { said[nsaid++] = msg; rt_log("[dlg] control message %04x to id %u (class %d) not handled\n", msg, t->id, t->cls); }
    return 0;
}
U(SendDlgItemMessageA) {   // (dlg, id, msg, wparam, lparam)
    HOSTFN;
    Dlg *d = dlg_of(ARG(1));
    Ctl *t = ctl_by_id(d, ARG(2));
    STDRET(5, t ? ctl_message(c, d, t, ARG(3), ARG(4), ARG(5)) : 0);
}

// Menus: LoadMenuA hands out a handle naming the resource; SetMenu shows it as the port's menu bar (see above).
U(LoadMenuA) { HOSTFN; rt_log("[menu] LoadMenuA(%u)\n", ARG(2)); STDRET(2, ARG(2) < 0x10000u ? MENU_HANDLE(ARG(2)) : 0u); }
U(SetMenu) { HOSTFN; menu_set(ARG(2)); refresh(); STDRET(2, 1); }
U(DestroyMenu) { HOSTFN; if (ARG(1) && (ARG(1) & 0xffffu) == g_menu.res) g_menu.res = 0; STDRET(1, 1); }
