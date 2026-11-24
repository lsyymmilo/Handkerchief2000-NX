// GDI for the recompiled Handkerchief: device contexts over DirectDraw surfaces and 8-bit bitmaps.
//
// HDKWIN.EXE loads its BMP banks with LoadImageA(LR_CREATEDIBSECTION [| LR_LOADFROMFILE]), reads
// BITMAP.bmBits through GetObjectA and copies rows itself, or uses StretchBlt into a surface DC
// (docs/engine-map.md, 0x40DEF1 / 0x40E25F). Bitmaps therefore keep bottom-up DIB pixels in guest memory.
// Text output uses 1-bit bitmap fonts standing in for MS Sans Serif (font_data.h, tools/make_font.py).
#include "win32.h"
#include "hk_ddraw.h"
#include "font_data.h"
#include "cjk_font_data.h"
#include <stdio.h>
#include <stdlib.h>

#define G(name) void imp_GDI32_##name(CPU *c)
#define U(name) void imp_USER32_##name(CPU *c)

// ---------- handles ----------
#define H_SURF_DC 0x00D00000u   // + slot: DC over a DirectDraw surface
#define H_SCREEN_DC 0x00D10000u // the display (GetDC, CreateICA, BeginPaint)
#define H_MEM_DC 0x00D20000u    // + slot: memory DC (CreateCompatibleDC)
#define H_BITMAP 0x00B00000u    // + slot
#define H_FONT 0x00F00000u      // + slot
#define H_STOCK 0x00E00000u     // + stock index
#define NSLOT 256

typedef struct {
    int used, w, h, stride;
    uint32_t bits;   // guest address, bottom-up rows
    uint8_t pal[256][4];   // RGBQUAD {b, g, r, 0}
} Bitmap;
typedef struct {
    int used;
    uint32_t surface;   // surface DC
    uint32_t bitmap, font;
    uint32_t text_color, bk_color, bk_mode, align, extra;
} DC;
static Bitmap g_bmp[NSLOT];
static DC g_dc[NSLOT * 2 + 1];   // [0, NSLOT) surface DCs, [NSLOT, 2*NSLOT) memory DCs, last = screen
static int32_t g_font_height[NSLOT];
static int g_font_used[NSLOT];

static DC *dc_of(uint32_t h) {
    if (h >= H_SURF_DC && h < H_SURF_DC + NSLOT) return &g_dc[h - H_SURF_DC];
    if (h >= H_MEM_DC && h < H_MEM_DC + NSLOT) return &g_dc[NSLOT + (h - H_MEM_DC)];
    return &g_dc[2 * NSLOT];   // any other value acts as the screen
}
static Bitmap *bmp_of(uint32_t h) {
    return h >= H_BITMAP && h < H_BITMAP + NSLOT && g_bmp[h - H_BITMAP].used ? &g_bmp[h - H_BITMAP] : NULL;
}

uint32_t hk_gdi_surface_dc(uint32_t surface_guest) {
    for (uint32_t i = 0; i < NSLOT; i++)
        if (g_dc[i].used && g_dc[i].surface == surface_guest) return H_SURF_DC + i;
    for (uint32_t i = 0; i < NSLOT; i++)
        if (!g_dc[i].used) { memset(&g_dc[i], 0, sizeof g_dc[i]); g_dc[i].used = 1; g_dc[i].surface = surface_guest; return H_SURF_DC + i; }
    rt_fatal("GDI: too many surface device contexts");
}

// Pixel view of a DC: rows top-down via row(y). Returns 0 if the DC has no pixels (screen, empty memory DC).
typedef struct { int w, h, pitch, bottom_up; uint32_t pixels; } View;
static int view_of(uint32_t hdc, View *v) {
    DC *d = dc_of(hdc);
    if (d->surface) {
        v->bottom_up = 0;
        return hk_dd_surface_info(d->surface, &v->w, &v->h, &v->pitch, &v->pixels);
    }
    Bitmap *b = bmp_of(d->bitmap);
    if (!b) return 0;
    v->w = b->w; v->h = b->h; v->pitch = b->stride; v->pixels = b->bits; v->bottom_up = 1;
    return 1;
}
static uint32_t px_addr(const View *v, int x, int y) {
    int row = v->bottom_up ? v->h - 1 - y : y;
    return v->pixels + (uint32_t)(row * v->pitch + x);
}

// ---------- bitmaps ----------
static int bmp_new(void) {
    for (int i = 0; i < NSLOT; i++) if (!g_bmp[i].used) { memset(&g_bmp[i], 0, sizeof g_bmp[i]); g_bmp[i].used = 1; return i; }
    rt_fatal("GDI: too many bitmaps");
}
// Builds a bitmap from a packed DIB (BITMAPINFOHEADER, colour table, pixels). 8-bit or 4-bit, uncompressed;
// 4-bit pixels are widened to 8-bit indices 0-15 with the same colour table.
static uint32_t bmp_from_dib(const uint8_t *dib, uint32_t size, const char *what) {
    HOSTFN;
    if (size < 40) return 0;
    uint32_t hsz; int32_t w, h; uint16_t bpp; uint32_t comp, used;
    memcpy(&hsz, dib, 4); memcpy(&w, dib + 4, 4); memcpy(&h, dib + 8, 4);
    memcpy(&bpp, dib + 14, 2); memcpy(&comp, dib + 16, 4); memcpy(&used, dib + 32, 4);
    if ((bpp != 8 && bpp != 4) || comp != 0 || w <= 0 || h == 0) {
        rt_log("[gdi] %s: unsupported DIB %dx%d %u bpp compression %u\n", what, w, h, bpp, comp);
        return 0;
    }
    int top_down = h < 0;
    if (h < 0) h = -h;
    if (!used) used = bpp == 8 ? 256 : 16;
    int stride = (w + 3) & ~3, src_stride = bpp == 8 ? stride : ((w + 1) / 2 + 3) & ~3;
    uint32_t off = hsz + used * 4u;
    if ((uint64_t)off + (uint64_t)src_stride * (uint32_t)h > size) { rt_log("[gdi] %s: truncated DIB\n", what); return 0; }
    int i = bmp_new();
    Bitmap *b = &g_bmp[i];
    b->w = w; b->h = h; b->stride = stride;
    b->bits = gheap_alloc((uint32_t)(stride * h));
    if (!b->bits) rt_fatal("GDI: bitmap %dx%d: out of guest memory", w, h);
    memcpy(b->pal, dib + hsz, used * 4u);
    for (int y = 0; y < h; y++) {   // keep bottom-up storage, as a DIB section
        const uint8_t *src = dib + off + (uint32_t)((top_down ? h - 1 - y : y) * src_stride);
        uint8_t *dst = mb + b->bits + (uint32_t)(y * stride);
        if (bpp == 8) memcpy(dst, src, (size_t)stride);
        else for (int x = 0; x < w; x++) dst[x] = (x & 1) ? (src[x / 2] & 15) : (src[x / 2] >> 4);
    }
    rt_log("[gdi] bitmap %s: %dx%d %u bpp -> #%d\n", what, w, h, bpp, i);
    return H_BITMAP + (uint32_t)i;
}

// USER32!LoadImageA(hinst, name, type, cx, cy, flags)
U(LoadImageA) {
    HOSTFN;
    uint32_t name = ARG(2), type = ARG(3), flags = ARG(6);
    if (type != 0) { rt_log("[gdi] LoadImageA type %u not supported\n", type); STDRET(6, 0); }
    if (flags & 0x10u) {   // LR_LOADFROMFILE
        char hp[600];
        extern uint64_t g_lt_dec_t, g_lt_read_t, g_lt_read_n, g_lt_read_b, g_lt_open_t, g_lt_open_n; extern int g_trace_load;
        uint64_t t0 = SDL_GetPerformanceCounter();
        w32_host_path(GSTR(name), hp, sizeof hp);
        extern const uint8_t *hk_preload_file(const char *hostpath, long *size);
        long pn = 0;
        const uint8_t *pd = hk_preload_file(hp, &pn);   // served from RAM when the background preload has it
        if (pd && pn > 14 && pd[0] == 'B' && pd[1] == 'M') {
            uint64_t t1 = SDL_GetPerformanceCounter();
            uint32_t h = bmp_from_dib(pd + 14, (uint32_t)pn - 14, GSTR(name));
            if (g_trace_load) { uint64_t t2 = SDL_GetPerformanceCounter(); g_lt_read_n++; g_lt_read_b += (uint64_t)pn; g_lt_open_n++; g_lt_dec_t += t2 - t1; (void)t0; }
            STDRET(6, h);
        }
        FILE *f = fopen(hp, "rb");
        if (!f) { rt_log("[gdi] LoadImageA: cannot open %s\n", hp); STDRET(6, 0); }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *d = n > 14 ? malloc((size_t)n) : NULL;
        uint32_t h = 0;
        int ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
        uint64_t t1 = SDL_GetPerformanceCounter();
        if (ok && d[0] == 'B' && d[1] == 'M')
            h = bmp_from_dib(d + 14, (uint32_t)n - 14, GSTR(name));
        free(d);
        fclose(f);
        if (g_trace_load) {   // open+read of the whole file counts as I/O, the rest as decode
            uint64_t t2 = SDL_GetPerformanceCounter();
            g_lt_read_t += t1 - t0; g_lt_read_n++; g_lt_read_b += (uint64_t)n; g_lt_open_n++; g_lt_dec_t += t2 - t1;
        }
        STDRET(6, h);
    }
    uint32_t size = 0, data = w32_resource(name, 2, &size);   // RT_BITMAP
    if (!data) { rt_log("[gdi] LoadImageA: no bitmap resource %s\n", name < 0x10000 ? "(id)" : GSTR(name)); STDRET(6, 0); }
    STDRET(6, bmp_from_dib(mb + data, size, name < 0x10000 ? "resource id" : GSTR(name)));
}

// GetObjectA(h, size, out): BITMAP (24 bytes) for bitmaps, LOGFONT height for fonts.
G(GetObjectA) {
    HOSTFN;
    uint32_t h = ARG(1), n = ARG(2), out = ARG(3);
    Bitmap *b = bmp_of(h);
    if (b) {
        if (!out) STDRET(3, 24);
        if (n < 24) STDRET(3, 0);
        memset(mb + out, 0, n < 84 ? n : 84);
        W32(out, 0); W32(out + 4, b->w); W32(out + 8, b->h); W32(out + 12, b->stride);
        W16(out + 16, 1); W16(out + 18, 8); W32(out + 20, b->bits);
        if (n >= 84) {   // DIBSECTION: BITMAPINFOHEADER after the BITMAP
            W32(out + 24, 40); W32(out + 28, b->w); W32(out + 32, b->h); W16(out + 36, 1); W16(out + 38, 8);
            W32(out + 44, (uint32_t)(b->stride * b->h));
            STDRET(3, 84);
        }
        STDRET(3, 24);
    }
    if (h >= H_FONT && h < H_FONT + NSLOT) {
        if (out && n >= 4) { memset(mb + out, 0, n < 60 ? n : 60); W32(out, (uint32_t)g_font_height[h - H_FONT]); }
        STDRET(3, n < 60 ? n : 60);
    }
    rt_log("[gdi] GetObjectA(%08x) unknown object\n", h);
    STDRET(3, 0);
}

// ---------- device contexts ----------
U(GetDC) { HOSTFN; STDRET(1, H_SCREEN_DC); }
G(CreateICA) { HOSTFN; STDRET(4, H_SCREEN_DC); }
G(CreateCompatibleDC) {
    HOSTFN;
    for (uint32_t i = 0; i < NSLOT; i++) {
        DC *d = &g_dc[NSLOT + i];
        if (!d->used) { memset(d, 0, sizeof *d); d->used = 1; STDRET(1, H_MEM_DC + i); }
    }
    rt_fatal("GDI: too many memory device contexts");
}
G(DeleteDC) {
    HOSTFN;
    uint32_t h = ARG(1);
    if ((h >= H_MEM_DC && h < H_MEM_DC + NSLOT) || (h >= H_SURF_DC && h < H_SURF_DC + NSLOT)) dc_of(h)->used = 0;
    STDRET(1, 1);
}
G(GetStockObject) { HOSTFN; STDRET(1, H_STOCK + (ARG(1) & 0xffu)); }
G(SelectObject) {
    HOSTFN;
    DC *d = dc_of(ARG(1));
    uint32_t o = ARG(2), prev = H_STOCK;
    if (bmp_of(o) || (o >= H_STOCK && o < H_STOCK + 0x100 && (o & 0xff) == 0)) { prev = d->bitmap ? d->bitmap : H_STOCK; d->bitmap = o; }
    else if (o >= H_FONT && o < H_FONT + NSLOT) { prev = d->font ? d->font : H_STOCK + 13; d->font = o; }
    else if (o >= H_STOCK && o < H_STOCK + 0x100) { if ((o & 0xff) >= 10) { prev = d->font ? d->font : H_STOCK + 13; d->font = o; } }
    STDRET(2, prev);
}
G(DeleteObject) {
    HOSTFN;
    Bitmap *b = bmp_of(ARG(1));
    if (b) { gheap_free(b->bits); b->used = 0; }
    if (ARG(1) >= H_FONT && ARG(1) < H_FONT + NSLOT) g_font_used[ARG(1) - H_FONT] = 0;
    STDRET(1, 1);
}

// GetDeviceCaps(hdc, index): a 640x480 palettized display.
G(GetDeviceCaps) {
    HOSTFN;
    uint32_t r;
    switch (ARG(2)) {
    case 8: r = 640; break;            // HORZRES
    case 10: r = 480; break;           // VERTRES
    case 12: r = 8; break;             // BITSPIXEL
    case 14: r = 1; break;             // PLANES
    case 24: r = 256; break;           // NUMCOLORS
    case 38: r = 0x100 | 0x1 | 0x8; break;   // RASTERCAPS: RC_PALETTE | RC_BITBLT | RC_BITMAP64
    case 88: case 90: r = 96; break;   // LOGPIXELSX / Y
    case 104: r = 256; break;          // SIZEPALETTE
    case 106: r = 20; break;           // NUMRESERVED
    case 108: r = 24; break;           // COLORRES
    default: r = 0; rt_log("[gdi] GetDeviceCaps(%u) -> 0\n", ARG(2));
    }
    STDRET(2, r);
}
// GetSystemPaletteEntries(hdc, start, count, entries): the primary surface palette (PALETTEENTRY r,g,b,flags).
G(GetSystemPaletteEntries) {
    HOSTFN;
    uint32_t start = ARG(2), n = ARG(3);
    if (!ARG(4)) STDRET(4, 256);
    const uint8_t *p = hk_dd_primary_palette();
    for (uint32_t i = 0; i < n && start + i < 256; i++) {
        uint32_t k = start + i;
        if (p) memcpy(mb + ARG(4) + 4 * i, p + 4 * k, 4);
        else { W8(ARG(4) + 4 * i, k); W8(ARG(4) + 4 * i + 1, k); W8(ARG(4) + 4 * i + 2, k); W8(ARG(4) + 4 * i + 3, 0); }
    }
    STDRET(4, n);
}
G(SetSystemPaletteUse) { HOSTFN; STDRET(2, 1); }   // previous: SYSPAL_STATIC

// ---------- blits and pixels ----------
// StretchBlt(dst, x, y, w, h, src, sx, sy, sw, sh, rop): 8-bit index copy (bitmap and surface share the game palette).
G(StretchBlt) {
    HOSTFN;
    View d, s;
    int dx = (int32_t)ARG(2), dy = (int32_t)ARG(3), dw = (int32_t)ARG(4), dh = (int32_t)ARG(5);
    int sx = (int32_t)ARG(7), sy = (int32_t)ARG(8), sw = (int32_t)ARG(9), sh = (int32_t)ARG(10);
    if (!view_of(ARG(1), &d) || !view_of(ARG(6), &s)) { rt_log("[gdi] StretchBlt without pixels\n"); STDRET(11, 0); }
    if (ARG(11) != 0x00CC0020u) rt_log("[gdi] StretchBlt rop %08x treated as SRCCOPY\n", ARG(11));
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) STDRET(11, 1);
    for (int y = 0; y < dh; y++) {
        int ty = dy + y, fy = sy + (int)((int64_t)y * sh / dh);
        if (ty < 0 || ty >= d.h || fy < 0 || fy >= s.h) continue;
        for (int x = 0; x < dw; x++) {
            int tx = dx + x, fx = sx + (int)((int64_t)x * sw / dw);
            if (tx < 0 || tx >= d.w || fx < 0 || fx >= s.w) continue;
            W8(px_addr(&d, tx, ty), R8(px_addr(&s, fx, fy)));
        }
    }
    STDRET(11, 1);
}
static uint32_t index_rgb(uint8_t i) {
    const uint8_t *p = hk_dd_primary_palette();
    return p ? (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) : i * 0x010101u;
}
static uint8_t nearest_index(uint32_t colorref) {
    const uint8_t *p = hk_dd_primary_palette();
    if ((colorref >> 24) == 1) return (uint8_t)colorref;   // PALETTEINDEX(i)
    if (!p) return (uint8_t)(colorref & 0xff);
    int r = colorref & 0xff, g = (colorref >> 8) & 0xff, b = (colorref >> 16) & 0xff, best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = p[4 * i] - r, dg = p[4 * i + 1] - g, db = p[4 * i + 2] - b, dd = dr * dr + dg * dg + db * db;
        if (dd < bd) { bd = dd; best = i; }
    }
    return (uint8_t)best;
}
G(GetPixel) {
    HOSTFN;
    View v;
    int x = (int32_t)ARG(2), y = (int32_t)ARG(3);
    if (!view_of(ARG(1), &v) || x < 0 || y < 0 || x >= v.w || y >= v.h) STDRET(3, 0xFFFFFFFFu);   // CLR_INVALID
    STDRET(3, index_rgb(R8(px_addr(&v, x, y))));
}
G(SetPixel) {
    HOSTFN;
    View v;
    int x = (int32_t)ARG(2), y = (int32_t)ARG(3);
    if (!view_of(ARG(1), &v) || x < 0 || y < 0 || x >= v.w || y >= v.h) STDRET(4, 0xFFFFFFFFu);
    uint8_t i = nearest_index(ARG(4));
    W8(px_addr(&v, x, y), i);
    STDRET(4, index_rgb(i));
}

// ---------- text: state kept, drawing logged only ----------
G(CreateFontIndirectA) {
    HOSTFN;
    for (int i = 0; i < NSLOT; i++)
        if (!g_font_used[i]) {
            g_font_used[i] = 1;
            g_font_height[i] = ARG(1) ? (int32_t)R32(ARG(1)) : 16;
            if (ARG(1) && (RT_TRACE("HK_TRACE_GDI") || rt_first_time(0x46000000ull ^ ((uint64_t)(uint32_t)R32(ARG(1)) << 32) ^ ((uint64_t)(uint32_t)R32(ARG(1) + 4) << 16) ^ (R32(ARG(1) + 16) * 31u) ^ R8(ARG(1) + 23)) ))
                rt_log("[gdi] CreateFontIndirectA height %d width %d weight %u charset %u face \"%s\" -> #%d\n",
                               (int32_t)R32(ARG(1)), (int32_t)R32(ARG(1) + 4), R32(ARG(1) + 16), R8(ARG(1) + 23),
                               (char *)mb + ARG(1) + 28, i);
            STDRET(1, H_FONT + (uint32_t)i);
        }
    STDRET(1, 0);
}
G(SetTextColor) { HOSTFN; DC *d = dc_of(ARG(1)); uint32_t p = d->text_color; d->text_color = ARG(2); STDRET(2, p); }
G(SetBkColor) { HOSTFN; DC *d = dc_of(ARG(1)); uint32_t p = d->bk_color; d->bk_color = ARG(2); STDRET(2, p); }
G(SetBkMode) { HOSTFN; DC *d = dc_of(ARG(1)); uint32_t p = d->bk_mode ? d->bk_mode : 2; d->bk_mode = ARG(2); STDRET(2, p); }
G(SetTextAlign) { HOSTFN; DC *d = dc_of(ARG(1)); uint32_t p = d->align; d->align = ARG(2); STDRET(2, p); }
G(SetTextCharacterExtra) { HOSTFN; DC *d = dc_of(ARG(1)); uint32_t p = d->extra; d->extra = ARG(2); STDRET(2, p); }
static int font_h(DC *d) {
    int h = d->font >= H_FONT && d->font < H_FONT + NSLOT ? g_font_height[d->font - H_FONT] : 14;
    return h < 0 ? -h : h ? h : 14;
}
// Bitmap font closest to the DC's LOGFONT cell height (port/hk/font_data.h, from tools/make_font.py).
static const HkFont *dc_font(DC *d) {
    int h = font_h(d);
    const HkFont *best = &hk_fonts[0];
    for (unsigned i = 0; i < sizeof hk_fonts / sizeof hk_fonts[0]; i++) {
        int dd = hk_fonts[i].cell - h, bd = best->cell - h;
        if ((dd < 0 ? -dd : dd) < (bd < 0 ? -bd : bd)) best = &hk_fonts[i];
    }
    return best;
}
static const HkGlyph *glyph(const HkFont *f, uint8_t ch) { return &f->glyphs[(ch < 0x20 ? '?' : ch) - 0x20]; }
static int text_width(DC *d, const HkFont *f, const uint8_t *s, uint32_t n) {
    const HkCjkFont *cf = hk_cjk_font(f->cell);
    int w = 0;
    for (uint32_t i = 0; i < n; ) {
        if (hk_gbk_lead(s[i]) && i + 1 < n) { w += cf->cell; i += 2; }
        else { w += glyph(f, s[i])->advance + (int)d->extra; i++; }
    }
    return w;
}
// GetTextExtentPoint32A(hdc, str, len, SIZE*)
G(GetTextExtentPoint32A) {
    HOSTFN;
    DC *d = dc_of(ARG(1));
    const HkFont *f = dc_font(d);
    W32(ARG(4), (uint32_t)text_width(d, f, mb + ARG(2), ARG(3)));
    W32(ARG(4) + 4, (uint32_t)f->cell);
    STDRET(4, 1);
}
// ExtTextOutA(hdc, x, y, options, rect, str, len, dx): 1-bit glyphs in the text colour's palette index.
// Alignment: TA_CENTER 6 / TA_RIGHT 2, TA_BOTTOM 8 / TA_BASELINE 24; ETO_OPAQUE 2 fills the rectangle and
// ETO_CLIPPED 4 clips to it; an OPAQUE background mode (2) fills the text cell with the background colour.
G(ExtTextOutA) {
    HOSTFN;
    DC *d = dc_of(ARG(1));
    View v;
    if (!view_of(ARG(1), &v)) STDRET(8, 1);
    const HkFont *f = dc_font(d);
    const uint8_t *s = mb + ARG(6);
    uint32_t n = ARG(6) ? ARG(7) : 0, opt = ARG(4), r = ARG(5), dx = ARG(8);
    int x = (int32_t)ARG(2), y = (int32_t)ARG(3);
    int cx0 = 0, cy0 = 0, cx1 = v.w, cy1 = v.h;
    if (r && (opt & 6u)) {
        int rl = (int32_t)R32(r), rt = (int32_t)R32(r + 4), rr = (int32_t)R32(r + 8), rb = (int32_t)R32(r + 12);
        if (opt & 2u) {
            uint8_t bk = nearest_index(d->bk_color);
            for (int yy = rt < 0 ? 0 : rt; yy < rb && yy < v.h; yy++)
                for (int xx = rl < 0 ? 0 : rl; xx < rr && xx < v.w; xx++) W8(px_addr(&v, xx, yy), bk);
        }
        if (opt & 4u) { if (rl > cx0) cx0 = rl; if (rt > cy0) cy0 = rt; if (rr < cx1) cx1 = rr; if (rb < cy1) cy1 = rb; }
    }
    int w = text_width(d, f, s, n);
    if ((d->align & 6u) == 6u) x -= w / 2; else if (d->align & 2u) x -= w;
    if ((d->align & 24u) == 24u) y -= f->ascent; else if (d->align & 8u) y -= f->cell;
    uint8_t fg = nearest_index(d->text_color), bg = nearest_index(d->bk_color);
    int opaque = d->bk_mode == 2;
    const HkCjkFont *cf = hk_cjk_font(f->cell);
    for (uint32_t i = 0; i < n; ) {
        if (hk_gbk_lead(s[i]) && i + 1 < n) {   // GBK two-byte: full-width CJK glyph, advance = cell
            const HkCjkGlyph *cg = hk_cjk_lookup(cf, (unsigned)s[i] | ((unsigned)s[i + 1] << 8));
            int adv = dx ? (int32_t)R32(dx + 4 * i) : cf->cell;
            if (cg) {
                const uint32_t *rows = cf->rows + (int)cg->idx * cf->cell;
                for (int gy = 0; gy < cf->cell; gy++) {
                    int py = y + gy;
                    if (py < cy0 || py >= cy1) continue;
                    uint32_t bits = rows[gy];
                    int span = opaque ? adv : cf->cell;
                    for (int gx = 0; gx < span; gx++) {
                        int px = x + gx;
                        if (px < cx0 || px >= cx1) continue;
                        if (gx < cf->cell && ((bits >> gx) & 1u)) W8(px_addr(&v, px, py), fg);
                        else if (opaque) W8(px_addr(&v, px, py), bg);
                    }
                }
            }
            x += adv; i += 2;
            continue;
        }
        const HkGlyph *g = glyph(f, s[i]);
        int adv = dx ? (int32_t)R32(dx + 4 * i) : g->advance + (int)d->extra;
        for (int gy = 0; gy < f->cell; gy++) {
            int py = y + gy;
            if (py < cy0 || py >= cy1) continue;
            uint32_t bits = f->rows[g->offset + gy];
            int span = opaque ? adv : g->width;
            for (int gx = 0; gx < span; gx++) {
                int px = x + gx;
                if (px < cx0 || px >= cx1) continue;
                if (gx < g->width && ((bits >> gx) & 1u)) W8(px_addr(&v, px, py), fg);
                else if (opaque) W8(px_addr(&v, px, py), bg);
            }
        }
        x += adv; i++;
    }
    STDRET(8, 1);
}
