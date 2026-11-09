#ifndef PS2_RASTER_H
#define PS2_RASTER_H

/*
 * PS2 software-rasteriser helpers. Header-only so the same code can be checked against the original per-pixel algorithms on a host
 * compiler (see docs/STATUS_AND_PLAN.md). The including file must provide the RSDK integer typedefs (uint16, uint32, int32).
 */

#include <math.h>
#include <stdint.h>

// Fills `count` 16-bit pixels with one colour using aligned 32-bit stores (one 16-bit store per pixel is about half the speed on the EE).
// Output is identical to a plain per-pixel loop.
static inline void ps2Fill16(uint16 *dst, int32 count, uint16 color)
{
    if (count <= 0)
        return;
    if (((uintptr_t)dst) & 2) {
        *dst++ = color;
        --count;
    }
    const uint32 c2 = (uint32)color | ((uint32)color << 16);
    uint32 *out     = (uint32 *)dst;
    int32 pairs     = count >> 1;
    for (; pairs >= 4; pairs -= 4) {
        out[0] = c2;
        out[1] = c2;
        out[2] = c2;
        out[3] = c2;
        out += 4;
    }
    for (; pairs > 0; --pairs) *out++ = c2;
    if (count & 1)
        *(uint16 *)out = color;
}

// Flat (INK_NONE) polygon that is only a few rows tall: the same pixels as DrawFace, without the 480-row scan-edge buffer, the clear loop
// or the generic fill. The special stage draws about 2000 such faces a frame (see docs/STATUS_AND_PLAN.md). The edge walk below is
// ProcessScanEdge line for line, and the clamps/fill are DrawFace's; tools/raster_check (host build) compares it with the stock code on random
// faces. Vertices are 16.16 fixed point. Returns 0 (drew nothing, caller must use the generic path) if the face is more than 8 rows tall.
static inline int ps2DrawFaceTinyFlat(uint16 *fb, int32 pitch, int32 cx1, int32 cy1, int32 cx2, int32 cy2, const int32 *vx, const int32 *vy, int32 n,
                                      uint16 color)
{
    int32 top = 0x7FFFFFFF, bottom = -0x10000;
    for (int32 v = 0; v < n; ++v) {
        if (vy[v] < top)
            top = vy[v];
        if (vy[v] > bottom)
            bottom = vy[v];
    }
    int32 topS = top >> 16, botS = bottom >> 16;
    if (topS < cy1)
        topS = cy1;
    if (topS > cy2)
        topS = cy2;
    if (botS < cy1)
        botS = cy1;
    if (botS > cy2)
        botS = cy2;
    if (topS == botS)
        return 1; // DrawFace draws nothing for a zero-height face
    if (botS - topS >= 8)
        return 0;

    int32 st[8], en[8];
    for (int32 r = 0; r <= botS - topS; ++r) {
        st[r] = 0x7FFF;
        en[r] = -1;
    }

    for (int32 v = 0; v < n; ++v) {
        const int32 w = (v + 1 == n) ? 0 : v + 1;
        int32 ix1 = vx[v] >> 16, iy1 = vy[v] >> 16, ix2 = vx[w] >> 16, iy2 = vy[w] >> 16;
        if (iy1 == iy2)
            continue;
        if (iy1 > iy2) {
            int32 t = ix1;
            ix1     = ix2;
            ix2     = t;
            t       = iy1;
            iy1     = iy2;
            iy2     = t;
        }
        int32 eTop    = iy1;
        int32 eBottom = iy2 + 1;
        if (eTop < cy2 && eBottom >= cy1) {
            if (eBottom > cy2)
                eBottom = cy2;
            int32 scanPos = ix1 << 16;
            const int32 delta = ((ix2 - ix1) << 16) / (iy2 - iy1);
            if (eTop < 0) {
                scanPos -= eTop * delta;
                eTop = 0;
            }
            for (int32 i = eTop; i < eBottom; ++i) {
                if (i >= topS && i <= botS) {
                    const int32 sx = scanPos >> 16;
                    const int32 r  = i - topS;
                    if (sx < st[r])
                        st[r] = sx;
                    if (sx > en[r])
                        en[r] = sx;
                }
                scanPos += delta;
            }
        }
    }

    uint16 *row = fb + (size_t)topS * (size_t)pitch;
    for (int32 r = 0; r <= botS - topS; ++r, row += pitch) {
        int32 s = st[r], e = en[r];
        if (s < cx1)
            s = cx1;
        if (s > cx2)
            s = cx2;
        if (e < cx1)
            e = cx1;
        if (e > cx2)
            e = cx2;
        ps2Fill16(row + s, e - s, color);
    }
    return 1;
}

// Largest integer r with r*r <= v (0 for v <= 0). Exact: the float estimate is corrected in both directions.
static inline int32 ps2ISqrt(int32 v)
{
    if (v <= 0)
        return 0;
    int32 r = (int32)sqrtf((float)v);
    while (r * r > v) --r;
    while ((r + 1) * (r + 1) <= v) ++r;
    return r;
}

// Flat (INK_NONE) ring: sets every pixel p in columns [left, right) and rows [top, bottom) with ir2 <= dx*dx + dy*dy < or2, where
// dx = column - x and dy = row - y. Produces exactly the same pixels as testing each one, but derives each row's extents directly
// instead of testing the whole bounding box. `fb` points at the start of row 0 of the framebuffer (column 0).
static inline void ps2DrawRingFlat(uint16 *fb, int32 pitchPixels, int32 x, int32 y, int32 left, int32 right, int32 top, int32 bottom, int32 ir2,
                                   int32 or2, uint16 color)
{
    for (int32 row = top; row < bottom; ++row) {
        const int32 dy = row - y;
        const int32 y2 = dy * dy;
        const int32 n  = or2 - y2; // need dx*dx < n
        if (n <= 0)
            continue;
        const int32 ow = ps2ISqrt(n - 1); // largest ow with ow*ow < n
        const int32 m  = ir2 - y2;        // need dx*dx >= m
        uint16 *line   = fb + (size_t)row * (size_t)pitchPixels;

        if (m <= 0) {
            int32 a = x - ow, b = x + ow; // inclusive
            if (a < left)
                a = left;
            if (b > right - 1)
                b = right - 1;
            if (a <= b)
                ps2Fill16(line + a, b - a + 1, color);
        }
        else {
            const int32 iw = ps2ISqrt(m - 1) + 1; // smallest iw with iw*iw >= m
            if (iw > ow)
                continue;
            int32 a = x - ow, b = x - iw; // left segment, inclusive
            if (a < left)
                a = left;
            if (b > right - 1)
                b = right - 1;
            if (a <= b)
                ps2Fill16(line + a, b - a + 1, color);

            a = x + iw;
            b = x + ow; // right segment, inclusive
            if (a < left)
                a = left;
            if (b > right - 1)
                b = right - 1;
            if (a <= b)
                ps2Fill16(line + a, b - a + 1, color);
        }
    }
}

#endif
