/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#include "COM_GlareFogGlowOperation.h"

namespace blender::compositor {

/*
 *  2D Fast Hartley Transform, used for convolution
 */

using fREAL = float;

/* Returns next highest power of 2 of x, as well its log2 in L2. */
static unsigned int next_pow2(unsigned int x, unsigned int *L2)
{
  unsigned int pw, x_notpow2 = x & (x - 1);
  *L2 = 0;
  while (x >>= 1) {
    ++(*L2);
  }
  pw = 1 << (*L2);
  if (x_notpow2) {
    (*L2)++;
    pw <<= 1;
  }
  return pw;
}

//------------------------------------------------------------------------------

/* From FXT library by Joerg Arndt, faster in order bit-reversal
 * use: `r = revbin_upd(r, h)` where `h = N>>1`. */
static unsigned int revbin_upd(unsigned int r, unsigned int h)
{
  while (!((r ^= h) & h)) {
    h >>= 1;
  }
  return r;
}
//------------------------------------------------------------------------------
static void FHT(fREAL *data, unsigned int M, unsigned int inverse)
{
  double tt, fc, dc, fs, ds, a = M_PI;
  fREAL t1, t2;
  int n2, bd, bl, istep, k, len = 1 << M, n = 1;

  int i, j = 0;
  unsigned int Nh = len >> 1;
  for (i = 1; i < (len - 1); i++) {
    j = revbin_upd(j, Nh);
    if (j > i) {
      t1 = data[i];
      data[i] = data[j];
      data[j] = t1;
    }
  }

  do {
    fREAL *data_n = &data[n];

    istep = n << 1;
    for (k = 0; k < len; k += istep) {
      t1 = data_n[k];
      data_n[k] = data[k] - t1;
      data[k] += t1;
    }

    n2 = n >> 1;
    if (n > 2) {
      fc = dc = cos(a);
      fs = ds = sqrt(1.0 - fc * fc);  // sin(a);
      bd = n - 2;
      for (bl = 1; bl < n2; bl++) {
        fREAL *data_nbd = &data_n[bd];
        fREAL *data_bd = &data[bd];
        for (k = bl; k < len; k += istep) {
          t1 = fc * (double)data_n[k] + fs * (double)data_nbd[k];
          t2 = fs * (double)data_n[k] - fc * (double)data_nbd[k];
          data_n[k] = data[k] - t1;
          data_nbd[k] = data_bd[k] - t2;
          data[k] += t1;
          data_bd[k] += t2;
        }
        tt = fc * dc - fs * ds;
        fs = fs * dc + fc * ds;
        fc = tt;
        bd -= 2;
      }
    }

    if (n > 1) {
      for (k = n2; k < len; k += istep) {
        t1 = data_n[k];
        data_n[k] = data[k] - t1;
        data[k] += t1;
      }
    }

    n = istep;
    a *= 0.5;
  } while (n < len);

  if (inverse) {
    fREAL sc = (fREAL)1 / (fREAL)len;
    for (k = 0; k < len; k++) {
      data[k] *= sc;
    }
  }
}
//------------------------------------------------------------------------------
/* 2D Fast Hartley Transform, Mx/My -> log2 of width/height,
 * nzp -> the row where zero pad data starts,
 * inverse -> see above. */
static void FHT2D(
    fREAL *data, unsigned int Mx, unsigned int My, unsigned int nzp, unsigned int inverse)
{
  unsigned int i, j, Nx, Ny, maxy;

  Nx = 1 << Mx;
  Ny = 1 << My;

  /* Rows (forward transform skips 0 pad data). */
  maxy = inverse ? Ny : nzp;
  for (j = 0; j < maxy; j++) {
    FHT(&data[Nx * j], Mx, inverse);
  }

  /* Transpose data. */
  if (Nx == Ny) { /* Square. */
    for (j = 0; j < Ny; j++) {
      for (i = j + 1; i < Nx; i++) {
        unsigned int op = i + (j << Mx), np = j + (i << My);
        SWAP(fREAL, data[op], data[np]);
      }
    }
  }
  else { /* Rectangular. */
    unsigned int k, Nym = Ny - 1, stm = 1 << (Mx + My);
    for (i = 0; stm > 0; i++) {
#define PRED(k) (((k & Nym) << Mx) + (k >> My))
      for (j = PRED(i); j > i; j = PRED(j)) {
        /* Pass. */
      }
      if (j < i) {
        continue;
      }
      for (k = i, j = PRED(i); j != i; k = j, j = PRED(j), stm--) {
        SWAP(fREAL, data[j], data[k]);
      }
#undef PRED
      stm--;
    }
  }

  SWAP(unsigned int, Nx, Ny);
  SWAP(unsigned int, Mx, My);

  /* Now columns == transposed rows. */
  for (j = 0; j < Ny; j++) {
    FHT(&data[Nx * j], Mx, inverse);
  }

  /* Finalize. */
  for (j = 0; j <= (Ny >> 1); j++) {
    unsigned int jm = (Ny - j) & (Ny - 1);
    unsigned int ji = j << Mx;
    unsigned int jmi = jm << Mx;
    for (i = 0; i <= (Nx >> 1); i++) {
      unsigned int im = (Nx - i) & (Nx - 1);
      fREAL A = data[ji + i];
      fREAL B = data[jmi + i];
      fREAL C = data[ji + im];
      fREAL D = data[jmi + im];
      fREAL E = (fREAL)0.5 * ((A + D) - (B + C));
      data[ji + i] = A - E;
      data[jmi + i] = B + E;
      data[ji + im] = C + E;
      data[jmi + im] = D - E;
    }
  }
}

//------------------------------------------------------------------------------

/* 2D convolution calc, d1 *= d2, M/N - > log2 of width/height. */
static void fht_convolve(fREAL *d1, const fREAL *d2, unsigned int M, unsigned int N)
{
  fREAL a, b;
  unsigned int i, j, k, L, mj, mL;
  unsigned int m = 1 << M, n = 1 << N;
  unsigned int m2 = 1 << (M - 1), n2 = 1 << (N - 1);
  unsigned int mn2 = m << (N - 1);

  d1[0] *= d2[0];
  d1[mn2] *= d2[mn2];
  d1[m2] *= d2[m2];
  d1[m2 + mn2] *= d2[m2 + mn2];
  for (i = 1; i < m2; i++) {
    k = m - i;
    a = d1[i] * d2[i] - d1[k] * d2[k];
    b = d1[k] * d2[i] + d1[i] * d2[k];
    d1[i] = (b + a) * (fREAL)0.5;
    d1[k] = (b - a) * (fREAL)0.5;
    a = d1[i + mn2] * d2[i + mn2] - d1[k + mn2] * d2[k + mn2];
    b = d1[k + mn2] * d2[i + mn2] + d1[i + mn2] * d2[k + mn2];
    d1[i + mn2] = (b + a) * (fREAL)0.5;
    d1[k + mn2] = (b - a) * (fREAL)0.5;
  }
  for (j = 1; j < n2; j++) {
    L = n - j;
    mj = j << M;
    mL = L << M;
    a = d1[mj] * d2[mj] - d1[mL] * d2[mL];
    b = d1[mL] * d2[mj] + d1[mj] * d2[mL];
    d1[mj] = (b + a) * (fREAL)0.5;
    d1[mL] = (b - a) * (fREAL)0.5;
    a = d1[m2 + mj] * d2[m2 + mj] - d1[m2 + mL] * d2[m2 + mL];
    b = d1[m2 + mL] * d2[m2 + mj] + d1[m2 + mj] * d2[m2 + mL];
    d1[m2 + mj] = (b + a) * (fREAL)0.5;
    d1[m2 + mL] = (b - a) * (fREAL)0.5;
  }
  for (i = 1; i < m2; i++) {
    k = m - i;
    for (j = 1; j < n2; j++) {
      L = n - j;
      mj = j << M;
      mL = L << M;
      a = d1[i + mj] * d2[i + mj] - d1[k + mL] * d2[k + mL];
      b = d1[k + mL] * d2[i + mj] + d1[i + mj] * d2[k + mL];
      d1[i + mj] = (b + a) * (fREAL)0.5;
      d1[k + mL] = (b - a) * (fREAL)0.5;
      a = d1[i + mL] * d2[i + mL] - d1[k + mj] * d2[k + mj];
      b = d1[k + mj] * d2[i + mL] + d1[i + mL] * d2[k + mj];
      d1[i + mL] = (b + a) * (fREAL)0.5;
      d1[k + mj] = (b - a) * (fREAL)0.5;
    }
  }
}
//------------------------------------------------------------------------------

static void convolve(float *dst, MemoryBuffer *in1, MemoryBuffer *in2)
{
  fREAL *data1, *data2, *fp;
  unsigned int w2, h2, hw, hh, log2_w, log2_h;
  fRGB wt, *colp;
  int x, y, ch;
  int xbl, ybl, nxb, nyb, xbsz, ybsz;
  bool in2done = false;
  const unsigned int kernel_width = in2->get_width();
  const unsigned int kernel_height = in2->get_height();
  const unsigned int image_width = in1->get_width();
  const unsigned int image_height = in1->get_height();
  float *kernel_buffer = in2->get_buffer();
  float *image_buffer = in1->get_buffer();

  MemoryBuffer *rdst = new MemoryBuffer(DataType::Color, in1->get_rect());
  memset(rdst->get_buffer(),
         0,
         rdst->get_width() * rdst->get_height() * COM_DATA_TYPE_COLOR_CHANNELS * sizeof(float));

  /* Convolution result width & height. */
  w2 = 2 * kernel_width - 1;
  h2 = 2 * kernel_height - 1;
  /* FFT pow2 required size & log2. */
  w2 = next_pow2(w2, &log2_w);
  h2 = next_pow2(h2, &log2_h);

  /* Allocate space. */
  data1 = (fREAL *)MEM_callocN(3 * w2 * h2 * sizeof(fREAL), "convolve_fast FHT data1");
  data2 = (fREAL *)MEM_callocN(w2 * h2 * sizeof(fREAL), "convolve_fast FHT data2");

  /* Normalize convolutor. */
  wt[0] = wt[1] = wt[2] = 0.0f;
  for (y = 0; y < kernel_height; y++) {
    colp = (fRGB *)&kernel_buffer[y * kernel_width * COM_DATA_TYPE_COLOR_CHANNELS];
    for (x = 0; x < kernel_width; x++) {
      add_v3_v3(wt, colp[x]);
    }
  }
  if (wt[0] != 0.0f) {
    wt[0] = 1.0f / wt[0];
  }
  if (wt[1] != 0.0f) {
    wt[1] = 1.0f / wt[1];
  }
  if (wt[2] != 0.0f) {
    wt[2] = 1.0f / wt[2];
  }
  for (y = 0; y < kernel_height; y++) {
    colp = (fRGB *)&kernel_buffer[y * kernel_width * COM_DATA_TYPE_COLOR_CHANNELS];
    for (x = 0; x < kernel_width; x++) {
      mul_v3_v3(colp[x], wt);
    }
  }

  /* Copy image data, unpacking interleaved RGBA into separate channels
   * only need to calc data1 once. */

  /* Block add-overlap. */
  hw = kernel_width >> 1;
  hh = kernel_height >> 1;
  xbsz = (w2 + 1) - kernel_width;
  ybsz = (h2 + 1) - kernel_height;
  nxb = image_width / xbsz;
  if (image_width % xbsz) {
    nxb++;
  }
  nyb = image_height / ybsz;
  if (image_height % ybsz) {
    nyb++;
  }
  for (ybl = 0; ybl < nyb; ybl++) {
    for (xbl = 0; xbl < nxb; xbl++) {

      /* Each channel one by one. */
      for (ch = 0; ch < 3; ch++) {
        fREAL *data1ch = &data1[ch * w2 * h2];

        /* Only need to calc fht data from in2 once, can re-use for every block. */
        if (!in2done) {
          /* in2, channel ch -> data1 */
          for (y = 0; y < kernel_height; y++) {
            fp = &data1ch[y * w2];
            colp = (fRGB *)&kernel_buffer[y * kernel_width * COM_DATA_TYPE_COLOR_CHANNELS];
            for (x = 0; x < kernel_width; x++) {
              fp[x] = colp[x][ch];
            }
          }
        }

        /* in1, channel ch -> data2 */
        memset(data2, 0, w2 * h2 * sizeof(fREAL));
        for (y = 0; y < ybsz; y++) {
          int yy = ybl * ybsz + y;
          if (yy >= image_height) {
            continue;
          }
          fp = &data2[y * w2];
          colp = (fRGB *)&image_buffer[yy * image_width * COM_DATA_TYPE_COLOR_CHANNELS];
          for (x = 0; x < xbsz; x++) {
            int xx = xbl * xbsz + x;
            if (xx >= image_width) {
              continue;
            }
            fp[x] = colp[xx][ch];
          }
        }

        /* Forward FHT
         * zero pad data start is different for each == height+1. */
        if (!in2done) {
          FHT2D(data1ch, log2_w, log2_h, kernel_height + 1, 0);
        }
        FHT2D(data2, log2_w, log2_h, kernel_height + 1, 0);

        /* FHT2D transposed data, row/col now swapped
         * convolve & inverse FHT. */
        fht_convolve(data2, data1ch, log2_h, log2_w);
        FHT2D(data2, log2_h, log2_w, 0, 1);
        /* Data again transposed, so in order again. */

        /* Overlap-add result. */
        for (y = 0; y < (int)h2; y++) {
          const int yy = ybl * ybsz + y - hh;
          if ((yy < 0) || (yy >= image_height)) {
            continue;
          }
          fp = &data2[y * w2];
          colp = (fRGB *)&rdst->get_buffer()[yy * image_width * COM_DATA_TYPE_COLOR_CHANNELS];
          for (x = 0; x < (int)w2; x++) {
            const int xx = xbl * xbsz + x - hw;
            if ((xx < 0) || (xx >= image_width)) {
              continue;
            }
            colp[xx][ch] += fp[x];
          }
        }
      }
      in2done = true;
    }
  }

  MEM_freeN(data2);
  MEM_freeN(data1);
  memcpy(dst,
         rdst->get_buffer(),
         sizeof(float) * image_width * image_height * COM_DATA_TYPE_COLOR_CHANNELS);
  delete (rdst);
}

void GlareFogGlowOperation::generate_glare(float *data,
                                           MemoryBuffer *input_tile,
                                           const NodeGlare *settings)
{
  int x, y;
  float scale, u, v, r, w, d;
  fRGB fcol;
  MemoryBuffer *ckrn;
  unsigned int sz = 1 << settings->size;
  const float cs_r = 1.0f, cs_g = 1.0f, cs_b = 1.0f;

  /* Temp. src image
   * make the convolution kernel. */
  rcti kernel_rect;
  BLI_rcti_init(&kernel_rect, 0, sz, 0, sz);
  ckrn = new MemoryBuffer(DataType::Color, kernel_rect);

  scale = 0.25f * sqrtf((float)(sz * sz));

  for (y = 0; y < sz; y++) {
    v = 2.0f * (y / (float)sz) - 1.0f;
    for (x = 0; x < sz; x++) {
      u = 2.0f * (x / (float)sz) - 1.0f;
      r = (u * u + v * v) * scale;
      d = -sqrtf(sqrtf(sqrtf(r))) * 9.0f;
      fcol[0] = expf(d * cs_r);
      fcol[1] = expf(d * cs_g);
      fcol[2] = expf(d * cs_b);
      /* Linear window good enough here, visual result counts, not scientific analysis:
       * `w = (1.0f-fabs(u))*(1.0f-fabs(v));`
       * actually, Hanning window is ok, `cos^2` for some reason is slower. */
      w = (0.5f + 0.5f * cosf(u * (float)M_PI)) * (0.5f + 0.5f * cosf(v * (float)M_PI));
      mul_v3_fl(fcol, w);
      ckrn->write_pixel(x, y, fcol);
    }
  }

  convolve(data, input_tile, ckrn);
  delete ckrn;
}

}  // namespace blender::compositor
