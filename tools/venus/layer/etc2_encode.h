/*
 * Fast ETC2 / EAC block encoders, for transcoding the BC textures of Direct3D games into formats
 * Mali samples natively: the same bits per texel as the BC originals, where decoding them to RGBA8
 * took 2-8 times the memory.
 *
 * Speed over quality: a 4x4 colour block is encoded in ETC1's differential (or individual) mode
 * from the two half-blocks' average colours, with each pixel's modifier chosen from the summed
 * channel difference (an ETC modifier adds the same value to R, G and B, so the squared error over
 * the three channels depends on that sum alone, clamping aside). Alpha and single channels use
 * EAC with a base and multiplier fitted to the block's range.
 *
 * Block layouts follow the Khronos Data Format Specification (ETC2 / EAC sections).
 */
#ifndef ETC2_ENCODE_H
#define ETC2_ENCODE_H

#include <stdint.h>

static const int etc1_modifiers[8][2] = {
   { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 },
};

static const int eac_modifiers[16][8] = {
   { -3, -6, -9, -15, 2, 5, 8, 14 },  { -3, -7, -10, -13, 2, 6, 9, 12 }, { -2, -5, -8, -13, 1, 4, 7, 12 },
   { -2, -4, -6, -13, 1, 3, 5, 12 },  { -3, -6, -8, -12, 2, 5, 7, 11 },  { -3, -7, -9, -11, 2, 6, 8, 10 },
   { -4, -7, -8, -11, 3, 6, 7, 10 },  { -3, -5, -8, -11, 2, 4, 7, 10 },  { -2, -6, -8, -10, 1, 5, 7, 9 },
   { -2, -5, -8, -10, 1, 4, 7, 9 },   { -2, -4, -8, -10, 1, 3, 7, 9 },   { -2, -5, -7, -10, 1, 4, 6, 9 },
   { -3, -4, -7, -10, 2, 3, 6, 9 },   { -1, -2, -3, -10, 0, 1, 2, 9 },   { -4, -6, -8, -9, 3, 5, 7, 8 },
   { -3, -5, -7, -9, 2, 4, 6, 8 },
};

static inline int etc_clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* One half-block (8 pixels) against a base colour: the best table, its error, and the pixel
 * indices (ETC1 encoding: 0 = +small, 1 = +large, 2 = -small, 3 = -large). */
struct etc_half { uint32_t err; int table; uint8_t idx[8]; };

static void etc_fit_half(const uint8_t *const px[8], const int base[3], struct etc_half *out)
{
   int d[8], ad[8]; /* sum over channels of (pixel - base), and its magnitude */
   const int bsum = base[0] + base[1] + base[2];
   for (int i = 0; i < 8; i++) {
      d[i] = px[i][0] + px[i][1] + px[i][2] - bsum;
      ad[i] = d[i] < 0 ? -d[i] : d[i];
   }
   /* The table by the unclamped error, which is 3m^2 - 2m|d| per pixel up to a constant: the
    * three tables around the one whose mean modifier is nearest the half's mean |d| / 3. */
   int sum_ad = 0;
   for (int i = 0; i < 8; i++) sum_ad += ad[i];
   int guess = 0;
   while (guess < 7 && 3 * 8 * (etc1_modifiers[guess][0] + etc1_modifiers[guess][1]) < 2 * sum_ad) guess++;
   const int t_lo = guess > 0 ? guess - 1 : 0, t_hi = guess < 7 ? guess + 1 : 7;
   int best_t = guess, best_e = INT32_MAX;
   for (int t = t_lo; t <= t_hi; t++) {
      const int a = etc1_modifiers[t][0], b = etc1_modifiers[t][1], mid2 = 3 * (a + b);
      const int ea = 3 * a * a, eb = 3 * b * b;
      int e = 0;
      for (int i = 0; i < 8; i++)
         e += mid2 < 2 * ad[i] ? eb - 2 * b * ad[i] : ea - 2 * a * ad[i];
      if (e < best_e) { best_e = e; best_t = t; }
   }
   const int a = etc1_modifiers[best_t][0], b = etc1_modifiers[best_t][1], mid2 = 3 * (a + b);
   uint32_t err = 0;
   for (int i = 0; i < 8; i++) {
      const int big = mid2 < 2 * ad[i], neg = d[i] < 0;
      const int mod = (big ? b : a) * (neg ? -1 : 1);
      out->idx[i] = (uint8_t)(big | neg << 1);
      for (int c = 0; c < 3; c++) {
         const int e = etc_clamp255(base[c] + mod) - px[i][c];
         err += (uint32_t)(e * e);
      }
   }
   out->err = err;
   out->table = best_t;
}

/* rgba: 16 pixels, row-major, 4 bytes each (alpha ignored). out: 8 bytes, an opaque ETC2 RGB8
 * block (ETC1-compatible modes only). */
static void etc2_encode_rgb(const uint8_t *rgba, uint8_t out[8])
{
   uint64_t best_bits = 0;
   uint32_t best_err = UINT32_MAX;
   /* The split whose halves differ more: their averages then fit them better. */
   int sum[2][2] = { { 0, 0 }, { 0, 0 } }; /* [flip][half], over the three channels */
   for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++) {
         const uint8_t *p = rgba + (y * 4 + x) * 4;
         const int v = p[0] + p[1] + p[2];
         sum[0][x >= 2] += v;
         sum[1][y >= 2] += v;
      }
   const int d0 = sum[0][0] - sum[0][1], d1 = sum[1][0] - sum[1][1];
   const int only = (d0 < 0 ? -d0 : d0) >= (d1 < 0 ? -d1 : d1) ? 0 : 1;
   for (int flip = only; flip == only; flip++) {
      const uint8_t *half[2][8];
      int pos[2][8]; /* pixel number x * 4 + y, for the index bits */
      int n[2] = { 0, 0 };
      for (int y = 0; y < 4; y++)
         for (int x = 0; x < 4; x++) {
            const int h = flip ? (y >= 2) : (x >= 2);
            half[h][n[h]] = rgba + (y * 4 + x) * 4;
            pos[h][n[h]++] = x * 4 + y;
         }
      int avg[2][3];
      for (int h = 0; h < 2; h++)
         for (int c = 0; c < 3; c++) {
            int s = 0;
            for (int i = 0; i < 8; i++) s += half[h][i][c];
            avg[h][c] = s;
         }

      /* differential: 5-bit bases within -4..3 of each other */
      int q5[2][3], ok = 1;
      for (int h = 0; h < 2; h++)
         for (int c = 0; c < 3; c++) q5[h][c] = (avg[h][c] * 31 + 8 * 255 / 2) / (8 * 255);
      for (int c = 0; c < 3 && ok; c++) {
         const int dlt = q5[1][c] - q5[0][c];
         if (dlt < -4 || dlt > 3) ok = 0;
      }
      for (int mode = ok ? 0 : 1; mode < 2; mode++) {
         int base[2][3];
         for (int h = 0; h < 2; h++)
            for (int c = 0; c < 3; c++) {
               if (mode == 0) {
                  const int v = q5[h][c];
                  base[h][c] = (v << 3) | (v >> 2);
               } else {
                  const int v = (avg[h][c] * 15 + 8 * 255 / 2) / (8 * 255);
                  base[h][c] = (v << 4) | v;
               }
            }
         struct etc_half fit[2];
         etc_fit_half(half[0], base[0], &fit[0]);
         if (fit[0].err >= best_err) continue;
         etc_fit_half(half[1], base[1], &fit[1]);
         const uint32_t err = fit[0].err + fit[1].err;
         if (err >= best_err) continue;
         best_err = err;

         uint64_t bits = 0;
         for (int c = 0; c < 3; c++) {
            uint32_t byte;
            if (mode == 0) {
               const int dlt = q5[1][c] - q5[0][c];
               byte = (uint32_t)(q5[0][c] << 3) | (uint32_t)(dlt & 7);
            } else {
               byte = (uint32_t)((base[0][c] >> 4) << 4) | (uint32_t)(base[1][c] >> 4);
            }
            bits |= (uint64_t)byte << (56 - 8 * c);
         }
         bits |= (uint64_t)((uint32_t)fit[0].table << 5 | (uint32_t)fit[1].table << 2 |
                            (uint32_t)(mode == 0) << 1 | (uint32_t)flip) << 32;
         for (int h = 0; h < 2; h++)
            for (int i = 0; i < 8; i++) {
               const int p = pos[h][i], v = fit[h].idx[i];
               bits |= (uint64_t)(v >> 1) << (16 + p);
               bits |= (uint64_t)(v & 1) << p;
            }
         best_bits = bits;
      }
   }
   for (int i = 0; i < 8; i++) out[i] = (uint8_t)(best_bits >> (56 - 8 * i));
}

/* EAC: 16 target values in the output domain [0, max]. decode(v) = clamp(base * scale + bias +
 * modifier * multiplier * scale) - alpha (scale 1, bias 0, max 255) or R11 (scale 8, bias 4,
 * max 2047). values: row-major. */
static void eac_encode(const int values[16], int scale, int bias, int max, uint8_t out[8])
{
   int lo = values[0], hi = values[0];
   for (int i = 1; i < 16; i++) {
      if (values[i] < lo) lo = values[i];
      if (values[i] > hi) hi = values[i];
   }
   const int mid = (lo + hi + 1) / 2;
   int base = (mid - bias + scale / 2) / scale;
   if (base < 0) base = 0;
   if (base > 255) base = 255;
   uint64_t best_bits = (uint64_t)base << 56 | 1ull << 52; /* table 0, multiplier 1 */
   if (hi - lo <= scale) { /* flat: the base alone, every pixel at the smallest modifier */
      int best_k = 0, best_e = INT32_MAX;
      for (int k = 0; k < 8; k++) {
         int v = base * scale + bias + eac_modifiers[13][k] * scale;
         v = v < 0 ? 0 : v > max ? max : v;
         const int e = (v - mid) * (v - mid);
         if (e < best_e) { best_e = e; best_k = k; }
      }
      best_bits = (uint64_t)base << 56 | 1ull << 52 | 13ull << 48;
      for (int p = 0; p < 16; p++) best_bits |= (uint64_t)best_k << (45 - 3 * p);
   } else {
      uint64_t best_err = UINT64_MAX;
      const int c0 = base * scale + bias;
      for (int t = 0; t < 16 && best_err; t++) {
         const int *tab = eac_modifiers[t];
         const int span = tab[7] - tab[3]; /* largest minus smallest modifier */
         int m = ((hi - lo) + span * scale / 2) / (span * scale);
         if (m < 1) m = 1;
         if (m > 15) m = 15;
         int dec[8];
         for (int k = 0; k < 8; k++) {
            const int v = c0 + tab[k] * m * scale;
            dec[k] = v < 0 ? 0 : v > max ? max : v;
         }
         uint64_t err = 0, idxbits = 0;
         for (int i = 0; i < 16 && err < best_err; i++) {
            int best_e = INT32_MAX, best_k = 0;
            for (int k = 0; k < 8; k++) {
               const int e = (dec[k] - values[i]) * (dec[k] - values[i]);
               if (e < best_e) { best_e = e; best_k = k; }
            }
            err += (uint64_t)best_e;
            const int x = i & 3, y = i >> 2;
            idxbits |= (uint64_t)best_k << (45 - 3 * (x * 4 + y));
         }
         if (err < best_err) {
            best_err = err;
            best_bits = (uint64_t)base << 56 | (uint64_t)m << 52 | (uint64_t)t << 48 | idxbits;
         }
      }
   }
   for (int i = 0; i < 8; i++) out[i] = (uint8_t)(best_bits >> (56 - 8 * i));
}

/* rgba: 16 pixels, row-major. out: 16 bytes, an ETC2 RGBA8 block (EAC alpha, then colour). */
static void etc2_encode_rgba(const uint8_t *rgba, uint8_t out[16])
{
   int a[16];
   for (int i = 0; i < 16; i++) a[i] = rgba[i * 4 + 3];
   eac_encode(a, 1, 0, 255, out);
   etc2_encode_rgb(rgba, out + 8);
}

/* One channel of 16 pixels (stride bytes apart, 8-bit unsigned) to an EAC R11 unsigned block. */
static void eac_encode_r11(const uint8_t *src, int stride, uint8_t out[8])
{
   int v[16];
   for (int i = 0; i < 16; i++) v[i] = (src[i * stride] * 2047 + 127) / 255;
   eac_encode(v, 8, 4, 2047, out);
}

#endif
