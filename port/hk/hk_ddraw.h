// Shared between the DirectDraw (ddraw.c) and GDI (gdi.c) emulations.
#pragma once
#include <stdint.h>

void hk_dd_present(void);   // converts the primary surface through its palette and shows it
// Surface pixels in guest memory (8 bits per pixel); returns 0 if guest is not a surface object.
int hk_dd_surface_info(uint32_t guest, int *w, int *h, int *pitch, uint32_t *pixels);
const uint8_t *hk_dd_primary_palette(void);   // 256 PALETTEENTRY {r, g, b, flags}, or NULL
uint32_t hk_gdi_surface_dc(uint32_t surface_guest);   // HDC drawing into a DirectDraw surface
