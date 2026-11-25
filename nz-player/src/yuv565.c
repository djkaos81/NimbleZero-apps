/* SPDX-License-Identifier: GPL-2.0-only
 * nz-player - a small YU12 (planar 4:2:0) picture to RGB565, same size, with ordered
 * dithering.  Used for the pictures the GPU scaler delivers: at 320x170 this takes about a
 * millisecond, where a general-purpose converter costs several.
 *
 * 16-bit colour has 32 levels of red and blue and 64 of green; cutting 8-bit values down
 * without dithering shows as bands in smooth areas.  A 4x4 pattern of small offsets added
 * before the cut spreads the error so that the eye averages it.
 */
#include "nzp.h"

void yuv420_to_rgb565(uint16_t *dst, int dstride_px, const uint8_t *y, int ystride, const uint8_t *u, const uint8_t *v, int cstride,
                      int w, int h, int bt709, int full_range)
{
	static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
	/* fixed point, 8 fractional bits: R = cy*(Y-yo) + crv*V', G = cy*(Y-yo) - cgu*U' - cgv*V', B = cy*(Y-yo) + cbu*U' */
	int cy, yo, crv, cgu, cgv, cbu;
	if (full_range) { cy = 256; yo = 0; if (bt709) { crv = 403; cgu = 48; cgv = 120; cbu = 475; } else { crv = 359; cgu = 88; cgv = 183; cbu = 454; } }
	else            { cy = 298; yo = 16; if (bt709) { crv = 459; cgu = 55; cgv = 136; cbu = 541; } else { crv = 409; cgu = 100; cgv = 208; cbu = 516; } }
	for (int j = 0; j < h; j++) {
		const uint8_t *yp = y + (size_t)j * ystride, *up = u + (size_t)(j / 2) * cstride, *vp = v + (size_t)(j / 2) * cstride;
		uint16_t *o = dst + (size_t)j * dstride_px; const uint8_t *bj = bayer[j & 3];
		for (int i = 0; i < w; i++) {
			int c = cy * (yp[i] - yo) + 128, d = up[i / 2] - 128, e = vp[i / 2] - 128, t = bj[i & 3];
			int r = ((c + crv * e) >> 8) + (t >> 1), g = ((c - cgu * d - cgv * e) >> 8) + (t >> 2), b = ((c + cbu * d) >> 8) + (t >> 1);
			r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
			o[i] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
		}
	}
}

/* RGB, three bytes a pixel (red first), to RGB565 with the same dithering.  The GPU scaler
 * can deliver this: then every pixel has its own colour, where YU12 has one colour value
 * for each 2x2 block of the small picture. */
void rgb24_to_rgb565(uint16_t *dst, int dstride_px, const uint8_t *src, int sstride, int w, int h)
{
	static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
	for (int j = 0; j < h; j++) {
		const uint8_t *p = src + (size_t)j * sstride, *bj = bayer[j & 3]; uint16_t *o = dst + (size_t)j * dstride_px;
		for (int i = 0; i < w; i++, p += 3) {
			int t = bj[i & 3], r = p[0] + (t >> 1), g = p[1] + (t >> 2), b = p[2] + (t >> 1);
			r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
			o[i] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
		}
	}
}
