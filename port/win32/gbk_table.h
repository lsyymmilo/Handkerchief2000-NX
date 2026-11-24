// Unicode (BMP) -> GBK (CP936) conversion, used when ANSI (A-suffix) Win32 entry points receive UTF-16 data:
// RT_STRING entries, MENU templates and DIALOG titles. Matches what a Chinese-edition Windows does.
#pragma once

// One code point to GBK: returns 1 or 2, writes '?' (returning 1) for characters GBK cannot represent.
int w32_uni_to_gbk(unsigned u, char out[2]);

// NUL-terminated UTF-16 -> GBK into `out`, at most cap-1 bytes plus NUL; returns bytes written.
int w32_utf16_to_gbk(const unsigned short *w, char *out, int cap);

// GBK two-byte character -> Unicode code point; 0 when the pair is not valid GBK.
unsigned w32_gbk_to_uni(unsigned lead, unsigned trail);