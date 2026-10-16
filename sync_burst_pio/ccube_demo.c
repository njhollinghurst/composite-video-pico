#include <stdint.h>
#include <string.h>

#include "ccube_demo.h"

/* Global state */

static int ccube_width_in_bytes = 752;
static int ccube_height = 240;
static int ccube_clear_x = 0;
static int ccube_clear_w = 752;
static int ccube_pal = 0;
static int ccube_nt;
static uint16_t ccube_palette[256][4];
static int32_t R[3][3] = { // Rotation matrix, initial values (Q14)
  { 16384, 0, 0 },
  { 0, 16384, 0 },
  { 0, 0, 16384 },
};


#define CLIP(v) (((v)<3)?3:((v)>15)?15:(v))

static void mk_colour(uint8_t r, uint8_t g, uint8_t b, uint16_t pattern[4])
{
  int y = (77*r + 150*g + 29*b) >> 8;
  int u = b - y;
  int v = r - y;

    if (ccube_pal) {
      // Convert to YUV. We want to scale U, V so that the 4.433 MHz component
      // of our "steppy" U and V waveforms will have the same power as a pure
      // sine wave at the specified level; which requires a boost of ~10%.
      u = (138 * u + 128) >> 8;
      v = (247 * v + 128) >> 8;

      // Generate the 4x4 pixel pattern; all rows are permutations of the
      // top one (except for dithering). Black level is 4, white level 14.
      pattern[0] = CLIP((0x440 + 10 * (y - u)) >> 8) +
	16 * CLIP((0x4c0 + 10 * (y + v)) >> 8) +
	256 * CLIP((0x440 + 10 * (y + u)) >> 8) +
        4096 * CLIP((0x4c0 + 10 * (y - v)) >> 8);
      pattern[1] = CLIP((0x4c0 + 10 * (y + v)) >> 8) +
	16 * CLIP((0x440 + 10 * (y - u)) >> 8) +
	256 * CLIP((0x4c0 + 10 * (y - v)) >> 8) +
        4096 * CLIP((0x440 + 10 * (y + u)) >> 8);
      pattern[2] = (pattern[0] >> 8) | (pattern[0] << 8);
      pattern[3] = (pattern[1] >> 8) | (pattern[1] << 8);
    }
    else {
      // Convert to YUV. Although NTSC specifies I and Q, the underlying U, V
      // are defined much as in PAL (if we ignore chroma bandwidth limiting).
      // Due to a sampling phase difference we'll scale them by 1.1 / sqrt(2).
      u = (98 * u + 128) >> 8;
      v = (175 * v + 128) >> 8;

      // Generate the 4x2 pixel pattern. For NTSC-M, black level is 5.
      pattern[0] = CLIP((0x510 + 9 * (y - u - v)) >> 8) +
	16 * CLIP((0x590 + 9 * (y - u + v)) >> 8) +
	256 * CLIP((0x510 + 9 * (y + u + v)) >> 8) +
        4096 * CLIP((0x590 + 9 * (y + u - v)) >> 8);
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
  // greyscale
  for(int i = 216; i < 256; i++) {
    mk_colour((1674*(i-216)) >> 8, (1674*(i-216)) >> 8, (1674*(i-216)) >> 8, ccube_palette[i]);
  }
}

#define MAXT (3*6*6*2)
uint8_t tri_col[MAXT];
int32_t triangs[MAXT][3]; // Display (Y<<16)|X in units of pixel/16

static int make_visible_triangles()
{
  int ntriang = 0;

  // for each axis XYZ, construct only the cube-face that faces towards us
  for(int axis = 0; axis < 3; axis++) {
    static const int primary_colours[3] = { 1, 6, 36 };
    int32_t basis[3][3];
    int32_t vv[7][8];
    int sgn = (R[axis][2] < 0) ? 1 : -1;
    int c0 = 5 * primary_colours[axis];
    int cp = primary_colours[(axis+3+sgn)%3];
    int cq = primary_colours[(axis+3-sgn)%3];
    if (sgn<0) {
      c0 = 215 - c0;
      cp = -cp;
      cq = -cq;
    }

    basis[0][0] = sgn*R[axis][0] >> 2;
    basis[0][1] = sgn*R[axis][1] >> 2;
    basis[0][2] = sgn*R[axis][2] >> 2;
    basis[1][0] = sgn*R[(axis+3+sgn)%3][0] >> 2;
    basis[1][1] = sgn*R[(axis+3+sgn)%3][1] >> 2;
    basis[1][2] = sgn*R[(axis+3+sgn)%3][2] >> 2;
    basis[2][0] = sgn*R[(axis+3-sgn)%3][0] >> 2;
    basis[2][1] = sgn*R[(axis+3-sgn)%3][1] >> 2;
    basis[2][2] = sgn*R[(axis+3-sgn)%3][2] >> 2;

    for(int p = 0; p < 7; ++p) {
      for(int q = 0; q < 7; ++q) {
	int x = (3*basis[0][0] + (p-3)*basis[1][0] + (q-3)*basis[2][0]);
	int y = (3*basis[0][1] + (p-3)*basis[1][1] + (q-3)*basis[2][1]);
	int z = (3*basis[0][2] + (p-3)*basis[1][2] + (q-3)*basis[2][2]);
	z = (ccube_height << 20) / ((1<<17) + z);
	x = (x * z) >> 15;
	y = (y * z) >> 15;
	vv[p][q] = ((ccube_height << 19) + (ccube_width_in_bytes << 3)) + x + (x >> 2) - (y << 16);
      }
    }
    if (((vv[0][0] & 0xffff) - (vv[0][6] & 0xffff)) * ((vv[0][0] >> 16) - (vv[6][0] >> 16)) <
	((vv[0][0] >> 16) - (vv[0][6] >> 16)) * ((vv[0][0] & 0xffff) - (vv[6][0] & 0xffff))) {
      // face really is visible - output the 6x6 colour squares
      for(int p = 0; p < 6; ++p) {
	for(int q = 0; q < 6; ++q) {
	  triangs[ntriang][0] = vv[p][q];
	  triangs[ntriang][1] = vv[p][q+1];
	  triangs[ntriang][2] = vv[p+1][q];
	  tri_col[ntriang++] = c0 + p*cp + q*cq;
	  triangs[ntriang][0] = vv[p+1][q+1];
	  triangs[ntriang][1] = vv[p][q+1];
	  triangs[ntriang][2] = vv[p+1][q];
	  tri_col[ntriang++] = c0 + p*cp + q*cq;
	}
      }
    }
  }
  return ntriang;
}

static void draw_triangle(uint8_t *buffer, int32_t v0, int32_t v1, int32_t v2, uint16_t const *pattern)
{
  int32_t slope02 = (v2 >> 16) - (v0 >> 16);
  int32_t slope01 = (v1 >> 16) - (v0 >> 16);
  int32_t slope12 = (v2 >> 16) - (v1 >> 16);
  slope02 = (slope02) ? ((1 << 18) * ((v2 & 0xffff) - (v0 & 0xffff)) + (slope02 >> 1)) / slope02 : 0;
  slope01 = (slope01) ? ((1 << 18) * ((v1 & 0xffff) - (v0 & 0xffff)) + (slope01 >> 1)) / slope01 : 0;
  slope12 = (slope12) ? ((1 << 18) * ((v2 & 0xffff) - (v1 & 0xffff)) + (slope12 >> 1)) / slope12 : 0;
  // iterate over all the integer Y positions that are inside the triangle
  for(int i = ((v0 + 0xf0000) >> 20); i <= (v2 >> 20); ++i) {
    int x0, x1;
    x0 = (v0 << 18) + slope02 * (int32_t)(16*i - (v0 >> 16));
    if ((i << 20) < v1) {
      x1 = (v0 << 18) + slope01 * (int32_t)(16*i - (v0 >> 16));
    }
    else {
      x1 = (v2 << 18) + slope12 * (int32_t)(16*i - (v2 >> 16));
    }
    if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
    x0 >>= 21;
    x1 = (x1 >> 21) - x0 + 1;
    uint32_t *p = (uint32_t *)(buffer + i * ccube_width_in_bytes) + (x0 >> 3);
    uint32_t c = pattern[i & 3];
    c |= (c << 16);
    if (x0 & 7) { // draw misaligned start (up to 7 nybbles)
      uint32_t mask = 0xffffffff;
      if (x1 < 8) mask >>= 4 * (8 - x1);
      mask <<= 4 * (x0 & 7);
      p[0] = (p[0] & ~mask) | (c & mask);
      p++;
      x1 -= 8 - (x0 & 7);
      if (x1 < 1) continue;
    }
    for(; x1 >= 8; x1 -= 8) {
      *p++ = c;
    }
    if (x1) { // draw misaligned end (up to 7 nybbles)
      uint32_t mask = 0xffffffff;
      mask <<= 4 * (x1 & 7);
      *p = (*p & mask) | (c & ~mask);
    }
  }
}

void ccube_demo_init(int width_in_nybbles, int height, int pal)
{
  ccube_width_in_bytes = width_in_nybbles >> 1;
  ccube_height = height;
  ccube_clear_x = 0;
  ccube_clear_w = width_in_nybbles >> 1;
  ccube_pal = pal;

  memset(R, 0, sizeof(R));
  R[0][0] = 16384;
  R[1][1] = 16384;
  R[2][2] = 16384;

  mk_palette();
  ccube_nt = make_visible_triangles();
}

void ccube_demo_render(uint8_t *buffer)
{
  for(int i = 0; i < ccube_height; ++i) {
    uint32_t c = ccube_palette[255 - 40 * i / ccube_height][i & 3];
    memset(buffer + i * ccube_width_in_bytes + ccube_clear_x, c, ccube_clear_w); // greys are single-byte patterns
  }

   for(int t = 0; t < ccube_nt; t++) {
    int32_t v0 = triangs[t][0];
    int32_t v1 = triangs[t][1];
    int32_t v2 = triangs[t][2];
    if (v0 > v1) { int32_t tmp = v0; v0 = v1; v1 = tmp; }
    if (v1 > v2) { int32_t tmp = v1; v1 = v2; v2 = tmp; }
    if (v0 > v1) { int32_t tmp = v0; v0 = v1; v1 = tmp; }
    if (v0 < 0 || v2 >= (ccube_height << 20) || v0 >= v2) continue;
    draw_triangle(buffer, v0, v1, v2, ccube_palette[tri_col[t]]);
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

  // On subsequent frames, only clear the central square region for speed
  ccube_clear_x = (ccube_width_in_bytes - ccube_height) >> 1;
  ccube_clear_w = ccube_height;

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
      R[i][j] = (R1[i][0]*R2[0][j] + R1[i][1]*R2[1][j] + R1[i][2]*R2[2][j] + 8192) >> 14;
    }
  }

  ccube_nt = make_visible_triangles();
}
