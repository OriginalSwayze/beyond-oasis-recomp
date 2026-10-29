/* Anti-aliased text from the system's fonts, as 8-bit coverage masks.
 * Windows: GDI draws white text on black into a DIB; the red channel is the coverage. */
#include "text.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

TextMask text_render(const char *utf8, const char *face, int px, int weight, int tracking) {
    TextMask m = { 0, 0, NULL };
    wchar_t wtext[512], wface[64];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wtext, 512) - 1;
    MultiByteToWideChar(CP_UTF8, 0, face, -1, wface, 64);
    if (n <= 0) return m;

    HDC dc = CreateCompatibleDC(NULL);
    HFONT font = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, wface);
    HGDIOBJ old_font = SelectObject(dc, font);
    SetTextCharacterExtra(dc, tracking);
    SIZE size;
    GetTextExtentPoint32W(dc, wtext, n, &size);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int w = size.cx + 4, h = tm.tmHeight + 2;

    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;             /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)w * h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, 2, 1, wtext, n);
    GdiFlush();

    m.w = w; m.h = h;
    m.a = malloc((size_t)w * h);
    for (int i = 0; i < w * h; i++) m.a[i] = ((const uint8_t *)bits)[i * 4 + 2];
    m.baseline = tm.tmAscent + 1;

    SelectObject(dc, old_bmp);
    SelectObject(dc, old_font);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
    return m;
}
#else
/* No system text renderer on this platform yet: an empty mask keeps layouts working. */
TextMask text_render(const char *utf8, const char *face, int px, int weight, int tracking) {
    (void)utf8; (void)face; (void)weight; (void)tracking;
    TextMask m = { 1, px, NULL, px };
    m.a = calloc(1, (size_t)px);
    return m;
}
#endif

void text_free(TextMask *m) { free(m->a); m->a = NULL; }
