/* High internal resolution: each layer of the frame (plane A with the window, plane B,
 * sprites) is scaled on its own with Scale2x / Scale3x (4x = 2x twice), then the layers
 * are composited at that size with vdp_compose(), like the VDP does at 1x.
 *
 * Scale2x/3x pick, for every output pixel, one whole source pixel of the 3x3
 * neighbourhood, so colours, priorities and transparency are never blended: edges get
 * rounded and diagonals smoothed, and layers stay separate. Mid-frame palette changes
 * and line scrolls are kept because every line was captured with its own colours. */
#include "hd.h"

#include <stdlib.h>
#include <string.h>

HdFrame hd_frame;
uint32_t *hd_px;
int hd_w, hd_h;

static uint32_t *scaled[3], *temp;
static size_t scaled_cap;

/* Source pixel with the edges clamped. */
#define SRC(x, y) src[(size_t)((y) < 0 ? 0 : (y) >= h ? h - 1 : (y)) * pitch + ((x) < 0 ? 0 : (x) >= w ? w - 1 : (x))]

static void scale2x(const uint32_t *src, int pitch, int w, int h, uint32_t *dst) {
    int dw = w * 2;
    for (int y = 0; y < h; y++) {
        uint32_t *d0 = dst + (size_t)(y * 2) * dw, *d1 = d0 + dw;
        for (int x = 0; x < w; x++) {
            uint32_t B = SRC(x, y - 1), D = SRC(x - 1, y), E = src[(size_t)y * pitch + x], F = SRC(x + 1, y), H = SRC(x, y + 1);
            if (B != H && D != F) {
                d0[x * 2]     = D == B ? D : E;
                d0[x * 2 + 1] = B == F ? F : E;
                d1[x * 2]     = D == H ? D : E;
                d1[x * 2 + 1] = H == F ? F : E;
            } else {
                d0[x * 2] = d0[x * 2 + 1] = d1[x * 2] = d1[x * 2 + 1] = E;
            }
        }
    }
}

static void scale3x(const uint32_t *src, int pitch, int w, int h, uint32_t *dst) {
    int dw = w * 3;
    for (int y = 0; y < h; y++) {
        uint32_t *d0 = dst + (size_t)(y * 3) * dw, *d1 = d0 + dw, *d2 = d1 + dw;
        for (int x = 0; x < w; x++) {
            uint32_t A = SRC(x - 1, y - 1), B = SRC(x, y - 1), C = SRC(x + 1, y - 1);
            uint32_t D = SRC(x - 1, y), E = src[(size_t)y * pitch + x], F = SRC(x + 1, y);
            uint32_t G = SRC(x - 1, y + 1), H = SRC(x, y + 1), I = SRC(x + 1, y + 1);
            uint32_t *o0 = d0 + x * 3, *o1 = d1 + x * 3, *o2 = d2 + x * 3;
            if (B != H && D != F) {
                o0[0] = D == B ? D : E;
                o0[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                o0[2] = B == F ? F : E;
                o1[0] = (D == B && E != G) || (D == H && E != A) ? D : E;
                o1[1] = E;
                o1[2] = (B == F && E != I) || (H == F && E != C) ? F : E;
                o2[0] = D == H ? D : E;
                o2[1] = (D == H && E != I) || (H == F && E != G) ? H : E;
                o2[2] = H == F ? F : E;
            } else {
                o0[0] = o0[1] = o0[2] = o1[0] = o1[1] = o1[2] = o2[0] = o2[1] = o2[2] = E;
            }
        }
    }
}

static void scale_layer(const uint32_t *src, int w, int h, int scale, uint32_t *dst) {
    if (scale == 3) scale3x(src, HD_MAX_W, w, h, dst);
    else if (scale == 4) { scale2x(src, HD_MAX_W, w, h, temp); scale2x(temp, w * 2, w * 2, h * 2, dst); }
    else scale2x(src, HD_MAX_W, w, h, dst);
}

void hd_compose(int scale) {
    if (scale < 2) scale = 2;
    if (scale > HD_MAX_SCALE) scale = HD_MAX_SCALE;
    int w = hd_frame.w, h = hd_frame.h;
    if (w <= 0 || h <= 0) return;
    size_t need = (size_t)w * scale * h * scale;
    if (need > scaled_cap) {
        for (int k = 0; k < 3; k++) { free(scaled[k]); scaled[k] = malloc(need * 4); }
        free(temp); temp = malloc(need * 4);
        free(hd_px); hd_px = malloc(need * 4);
        scaled_cap = need;
    }
    hd_w = w * scale;
    hd_h = h * scale;
    scale_layer(&hd_frame.a[0][0], w, h, scale, scaled[0]);
    scale_layer(&hd_frame.b[0][0], w, h, scale, scaled[1]);
    scale_layer(&hd_frame.s[0][0], w, h, scale, scaled[2]);

    int mask = hd_frame.mask_sides ? hd_frame.extra * scale : 0;
    for (int y = 0; y < hd_h; y++) {
        int line = y / scale;
        uint16_t backdrop = hd_frame.backdrop[line];
        uint8_t flags = hd_frame.flags[line];
        uint32_t *out = hd_px + (size_t)y * hd_w;
        if (!(flags & HD_LINE_DISPLAY)) {
            uint32_t c = vdp_compose(0, 0, 0, backdrop, false);
            for (int x = 0; x < hd_w; x++) out[x] = c;
            continue;
        }
        bool sh = flags & HD_LINE_SH;
        const uint32_t *ra = scaled[0] + (size_t)y * hd_w, *rb = scaled[1] + (size_t)y * hd_w, *rs = scaled[2] + (size_t)y * hd_w;
        for (int x = 0; x < hd_w; x++) out[x] = vdp_compose(ra[x], rb[x], rs[x], backdrop, sh);
        if (mask) {
            memset(out, 0, (size_t)mask * 4);
            memset(out + hd_w - mask, 0, (size_t)mask * 4);
        }
    }
}
