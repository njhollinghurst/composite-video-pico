/**
 * Copyright (c) 2026 Raspberry Pi Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <string.h>

#include "ccube_demo.h"

#define MAXT 1296 // Maximum number of visible triangles in the animation

/* Global state */

static int ccube_width_in_bytes = 752;
static int ccube_frame_height = 240;
static int ccube_pal = 0;
static int ccube_nt = 0;
static int32_t ccube_triangs[MAXT][4]; // Display (Y<<16)|X in units of pixel/8; last entry is colour
static uint16_t ccube_palette[256][4];
static uint32_t ccube_explode_timer;
static int ccube_explode;

static int32_t R[3][3] = { // Rotation matrix, initial values (Q12)
  { 4096, 0, 0 },
  { 0, 4096, 0 },
  { 0, 0, 4096 },
};
static int32_t S[3][3] = { // Expanded matrix
  { 4096, 0, 0 },
  { 0, 4096, 0 },
  { 0, 0, 4096 },
};


/*
 * Convert (R,G,B) to an array of four unsigned shorts.
 * The index should be the scanline number modulo 4 (modulo 2 for NTSC).
 * Each byte-pair yields for four samples, assuming 2-byte alignment.
 */

#define CLIP(v) (((v)<3)?3:((v)>15)?15:(v))

static void mk_colour(uint8_t r, uint8_t g, uint8_t b, uint16_t pattern[4])
{
    if (ccube_pal) {
      // PAL: See BT.470-6. Coefficients for 10 * [Y-U Y+V Y+U Y-V ]' are:
      //
      // [ 10 -10   0 ] [ 1    0    0  ] [  .299  .587  .114 ] [ R / 255 ]
      // [ 10   0  10 ] [ 0  .493   0  ] [ -.299 -.587  .886 ] [ G / 255 ]
      // [ 10  10   0 ] [ 0    0  .877 ] [  .701 -.587 -.114 ] [ B / 255 ]
      // [ 10   0 -10 ]
      //
      // which equals:  [  4.4641   8.7639  -3.2540 ] [ R / 255 ]
      //                [  9.1378   0.7220   0.1142 ] [ G / 255 ]
      //                [  1.5159   2.9761   5.4820 ] [ B / 255 ]
      //                [ -3.1578  11.0180   2.1138 ]
      //
      // But we'll use rounded coefficients (multiples of 5/256) to keep the
      // 6x6x6 cube evenly spaced, at the expense of absolute colour accuracy.
      // To get more grey levels, we add one bit of fixed-pattern dithering.
      pattern[0] = CLIP((0x440 + 5 * (2*g + r - b)) >> 8) +
              16 * CLIP((0x4c0 + 10 * r) >> 8) +
             256 * CLIP((0x440 + 5 * g + 5 * b) >> 8) +
            4096 * CLIP((0x4c0 + 15 * g - 5 * r) >> 8);
      pattern[1] = ((pattern[0] & 0xf0f0) >> 4) | ((pattern[0] & 0x0f0f) << 4);
      pattern[2] = (pattern[0] >> 8) | (pattern[0] << 8);
      pattern[3] = (pattern[1] >> 8) | (pattern[1] << 8);
    }
    else {
      // NTSC is specified in terms of I and Q, but the underlying B-Y and R-Y
      // (U, V) are the same as above (if we ignore chroma bandwidth limiting).
      // Our sample locations are such that U and V are square waves, so we'll
      // scale them by about 1.1/sqrt(2) for comparable power to a sine wave.
      // The NTSC colour space is not sRGB, but never mind.
      //
      // [ 9 -9 -9 ] [ 1   0   0 ] [ 1    0    0  ] [  .299  .587  .114 ] [ R / 255 ]
      // [ 9 -9  9 ] [ 0  .75  0 ] [ 0  .493   0  ] [ -.299 -.587  .886 ] [ G / 255 ]
      // [ 9  9  9 ] [ 0   0 .75 ] [ 0    0  .877 ] [  .701 -.587 -.114 ] [ B / 255 ]
      // [ 9  9 -9 ]
      //
      // which equals: [ -0.4637  10.7113  -1.2709 ] [ R / 255 ]
      //               [  7.8357   3.7615  -2.6206 ] [ G / 255 ]
      //               [  5.8457  -0.1453   3.2761 ] [ B / 255 ]
      //               [ -2.4537   6.8045   4.6258 ]
      //
      // Again, use rounded coefficients (though not all multiples of 5/256)
      // to linearize the 6x6x6 colour cube palette, plus dithering for greys.
      pattern[0] = CLIP((0x520 + 10 * g - b) >> 8) +
              16 * CLIP((0x5a0 + 9 * r + 5 * (g - b)) >> 8) +
             256 * CLIP((0x520 + 5 * r + 4 * b) >> 8) +
            4096 * CLIP((0x5a0 + 5 * (g + b) - r) >> 8);
      pattern[1] = (pattern[0] >> 8) | (pattern[0] << 8);
      pattern[2] = pattern[0];
      pattern[3] = pattern[1];
    }
}

#undef CLIP

static void mk_palette()
{
  // 6x6x6 colour cube
  for(int i = 0; i < 216; i++) {
    mk_colour(51 * (i%6), 51 * ((i/6)%6), 51 * ((i/36)%6), ccube_palette[i]);
  }
  // 40 grey levels
  for(int i = 216; i < 256; i++) {
    mk_colour((1674*(i-216)) >> 8, (1674*(i-216)) >> 8, (1674*(i-216)) >> 8, ccube_palette[i]);
  }
}

#define SORT3(a,b,c) do { \
    int32_t tmp;                                     \
    if ((a) > (b)) tmp = (a), (a) = (b), (b) = tmp;  \
    if ((b) > (c)) tmp = (b), (b) = (c), (c) = tmp;  \
    if ((a) > (b)) tmp = (a), (a) = (b), (b) = tmp;  \
  } while (0)                                        \

static void make_cubie(int r, int g, int b, int bmap)
{
  int ntriang = ccube_nt;

  // for each axis XYZ, construct only the cube-face that faces towards us
  for(int axis = 0; axis < 3; axis++) {
    for(int sgn = -1; sgn <= 1; sgn += 2) {
      int32_t basis[4][3];
      int32_t vv[2][2];

      if ((sgn > 0) ? !(bmap & (8<<axis)) : !(bmap & (1<<axis)))
        continue;

      basis[0][0] = (2*r-5)*S[0][0] + (2*g-5)*S[1][0] + (2*b-5)*S[2][0] + sgn*R[axis][0];
      basis[0][1] = (2*r-5)*S[0][1] + (2*g-5)*S[1][1] + (2*b-5)*S[2][1] + sgn*R[axis][1];
      basis[0][2] = (2*r-5)*S[0][2] + (2*g-5)*S[1][2] + (2*b-5)*S[2][2] + sgn*R[axis][2];
      basis[1][0] = sgn*R[(axis+3+sgn)%3][0];
      basis[1][1] = sgn*R[(axis+3+sgn)%3][1];
      basis[1][2] = sgn*R[(axis+3+sgn)%3][2];
      basis[2][0] = sgn*R[(axis+3-sgn)%3][0];
      basis[2][1] = sgn*R[(axis+3-sgn)%3][1];
      basis[2][2] = sgn*R[(axis+3-sgn)%3][2];

      for(int p = 0; p < 2; ++p) {
        for(int q = 0; q < 2; ++q) {
          int x = basis[0][0] + (2*p-1)*basis[1][0] + (2*q-1)*basis[2][0];
          int y = basis[0][1] + (2*p-1)*basis[1][1] + (2*q-1)*basis[2][1];
          int z = basis[0][2] + (2*p-1)*basis[1][2] + (2*q-1)*basis[2][2];
          z = (ccube_frame_height << 19) / ((9<<14) + z);
          x = (x * z) >> 16;
          y = (y * z) >> 16;
          vv[p][q] = ((ccube_frame_height << 18) + (ccube_width_in_bytes << 3)) + x + (x >> 2) - (y << 16);
          if ((vv[p][q]&0xffff) >= (ccube_width_in_bytes<<4)) goto skip;
        }
      }
      if (((vv[0][0] & 0xffff) - (vv[0][1] & 0xffff)) * ((vv[0][0] >> 16) - (vv[1][0] >> 16)) <
          ((vv[0][0] >> 16) - (vv[0][1] >> 16)) * ((vv[0][0] & 0xffff) - (vv[1][0] & 0xffff))) {
        // face really is visible
        int32_t t0, t1, t2;
        t0 = vv[0][0];
        t1 = vv[0][1];
        t2 = vv[1][0];
        SORT3(t0, t1, t2);
        ccube_triangs[ntriang][0] = t0;
        ccube_triangs[ntriang][1] = t1;
        ccube_triangs[ntriang][2] = t2;
        ccube_triangs[ntriang++][3] = r + 6*g + 36*b;
        t0 = vv[1][1];
        t1 = vv[0][1];
        t2 = vv[1][0];
        SORT3(t0, t1, t2);
        ccube_triangs[ntriang][0] = t0;
        ccube_triangs[ntriang][1] = t1;
        ccube_triangs[ntriang][2] = t2;
        ccube_triangs[ntriang++][3] = r + 6*g + 36*b;
      }
    skip:;
    }
  }
  ccube_nt = ntriang;
}

// Construct the 6x6x6 colour-cube visualization out of coloured "cubies".
// This function decides which cube-faces (each comprising two triangles)
// to draw, and draws them in (approximate) back-to-front order. It is a
// pile of ad-hoc hacks/heuristics, since we are close to the limit of what
// two RP2040 ARM cores can render at 150 MHz. Notably, to avoid tearing,
// we cull an increasing number of rear cubies as the cube collapses.

static void make_visible_triangles()
{
  int axes[3];

  ccube_nt = 0;

  axes[0] = 4 * ((R[0][2] < 0) ? -R[0][2] : R[0][2]);
  axes[1] = 4 * ((R[1][2] < 0) ? -R[1][2] : R[1][2]) + 1;
  axes[2] = 4 * ((R[2][2] < 0) ? -R[2][2] : R[2][2]) + 2;
  SORT3(axes[0], axes[1], axes[2]);
  int m = (440 - ccube_explode) >> 5;
  for(int i2 = 0; i2 < 6; ++i2) {
    for(int i1 = 0; i1 < 6; ++i1) {
      for(int i0 = 0; i0 < 6; ++i0) {
        if (i2 >= m || i1 >= m || i0 >= m) {
          int bmap;
          int rgb[4];
          rgb[axes[0] & 3] = i0;
          rgb[axes[1] & 3] = i1;
          rgb[axes[2] & 3] = i2;
          if (R[0][2] > 0) rgb[0] = 5-rgb[0];
          if (R[1][2] > 0) rgb[1] = 5-rgb[1];
          if (R[2][2] > 0 ) rgb[2] = 5-rgb[2];
          if (ccube_explode > 256) {
            bmap = 63;
          }
          else {
            bmap = 0;
            if (rgb[0] == 0) bmap |= 1;
            if (rgb[1] == 0) bmap |= 2;
            if (rgb[2] == 0) bmap |= 4;
            if (rgb[0] == 5) bmap |= 8;
            if (rgb[1] == 5) bmap |= 16;
            if (rgb[2] == 5) bmap |= 32;
            if (!bmap) continue;
          }
          make_cubie(rgb[0], rgb[1], rgb[2], bmap);
        }
      }
    }
  }
}

static void draw_triangle(uint8_t *buffer, int32_t v0, int32_t v1, int32_t v2,
                          uint16_t pattern[4], int stripe_height, int chroma_phase, int stride_in_bytes)
{
  int32_t slope02 = (v2 >> 16) - (v0 >> 16);
  int32_t slope01 = (v1 >> 16) - (v0 >> 16);
  int32_t slope12 = (v2 >> 16) - (v1 >> 16);
  slope02 = (slope02) ? ((v2 - v0) << 18) / slope02 : 0;
  slope01 = (slope01) ? ((v1 - v0) << 18) / slope01 : 0;
  slope12 = (slope12) ? ((v2 - v1) << 18) / slope12 : 0;
  // iterate over all the integer Y positions that are inside the triangle
  int i = (v0 >= 0) ? (v0 >> 20) + 1 : 0;
  int x0 = (v0 << 18) + slope02 * (int32_t)(16*i - (v0 >> 16));
  int x1 = (v0 << 18) + slope01 * (int32_t)(16*i - (v0 >> 16));
  slope02 <<= 4;
  slope01 <<= 4;
  for(; i <= (v1 >> 20); ++i) {
    if (i >= stripe_height) return;
    hfill_nybble_pattern(buffer + i * stride_in_bytes, pattern[(i + chroma_phase)&3], x0 >> 21, x1 >> 21);
    x0 += slope02;
    x1 += slope01;
  }
  x1 = (v1 << 18) + slope12 * (int32_t)(16*i - (v1 >> 16));
  slope12 <<= 4;
  for(; i <= (v2 >> 20) && i < stripe_height; ++i) {
    hfill_nybble_pattern(buffer + i * stride_in_bytes, pattern[(i + chroma_phase)&3], x0 >> 21, x1 >> 21);
    x0 += slope02;
    x1 += slope12;
  }
}

void ccube_demo_init(int width_in_nybbles, int full_frame_height, int pal)
{
  ccube_width_in_bytes = width_in_nybbles >> 1;
  ccube_frame_height = full_frame_height;
  ccube_pal = pal;
  ccube_explode_timer = 0;
  ccube_explode = 256;

  memset(R, 0, sizeof(R));
  R[0][0] = 4096;
  R[1][1] = 4096;
  R[2][2] = 4096;

  memset(S, 0, sizeof(S));
  S[0][0] = 4096;
  S[1][1] = 4096;
  S[2][2] = 4096;

  mk_palette();
  make_visible_triangles();
}

void ccube_demo_render(void *buffer, int start_y, int stripe_height, int chroma_phase)
{
  uint8_t *p8 = (uint8_t *)buffer;
  for(int i = 0; i < stripe_height; ++i) {
    uint32_t c = ccube_palette[255 - 40 * (start_y + 2 * i) / ccube_frame_height][i & 3];
    memset(p8 + i * ccube_width_in_bytes, c, ccube_width_in_bytes); // greys are single-byte patterns
  }

  for(int t = 0; t < ccube_nt; t++) {
    int32_t v0 = ccube_triangs[t][0] - (start_y << 19);
    int32_t v1 = ccube_triangs[t][1] - (start_y << 19);
    int32_t v2 = ccube_triangs[t][2] - (start_y << 19);
    if (v2 > 0 && v0 < (stripe_height << 20)) {
      draw_triangle(p8, v0, v1, v2, ccube_palette[ccube_triangs[t][3] & 255], stripe_height, chroma_phase, ccube_width_in_bytes);
    }
  }
}

void ccube_demo_update()
{
  static const int COSA_Q14 = 16381;
  static const int SINA_Q14 = 321;
  static const int COSB_Q14 = 16383;
  static const int SINB_Q14 = 181;
  static int s0 = 0, c0 = 16384;
  static int s1 = 0, c1 = 16384;
  int i, j, t;

  // Rotate the 2-vectors and renormalize them
  t  = (COSA_Q14*c0 + SINA_Q14*s0 + 8192) >> 14;
  s0 = (COSA_Q14*s0 - SINA_Q14*c0 + 8192) >> 14;
  c0 = t;
  t = ((3 << 28) - c0*c0 - s0*s0 + 8192) >> 14;
  c0 = (t * c0 + 32768) >> 15;
  s0 = (t * s0 + 32768) >> 15;
  t  = (COSB_Q14*c1 + SINB_Q14*s1 + 8192) >> 14;
  s1 = (COSB_Q14*s1 - SINB_Q14*c1 + 8192) >> 14;
  c1 = t;
  t = ((3 << 28) - c1*c1 - s1*s1 + 8192) >> 14;
  c1 = (t * c1 + 32768) >> 15;
  s1 = (t * s1 + 32768) >> 15;

  // Build the 3x3 rotation matrix
  int32_t R1[3][3] = {
    { c0,  0, s0 },
    { 0,16384, 0 },
    { -s0, 0, c0 }
  };
  int32_t R2[3][3] = {
    { 16384, 0, 0 },
    { 0,  c1,  s1 },
    { 0, -s1,  c1 }
  };
  for(i = 0; i < 3; i++) {
    for(j = 0; j < 3; j++) {
      R[i][j] = (R1[i][0]*R2[0][j] + R1[i][1]*R2[1][j] + R1[i][2]*R2[2][j] + 32768) >> 16;
    }
  }

  if (ccube_explode_timer < 1024)
    ccube_explode = 256;
  else if (ccube_explode_timer < 2032)
    ccube_explode = (512 + 15*ccube_explode) >> 4;
  else {
    ccube_explode -= 16;
    if (ccube_explode < 272) ccube_explode = 256;
  }

  for(i = 0; i < 3; i++) {
    for(j = 0; j < 3; j++) {
      S[i][j] = (R[i][j] * ccube_explode + 128) >> 8;
    }
  }

  ccube_explode_timer = (ccube_explode_timer + 1) & 2047;

  make_visible_triangles();
}
