#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/surfcomp.c"
#include <assert.h>
static uint32_t random_state = 42;
static float random_float(void) {
  random_state = random_state * 1664525u + 1013904223u;
  return (float)((int32_t)(random_state >> 8) - 8388608) / 8388608;
}
int main(void) {
  /* The format fixes the basis values, not floating-point operation results. */
  uint8_t basis_bytes[64*64*4];
  for(unsigned i=0;i<4096;i++) {
    uint32_t bits;memcpy(&bits,&sfc_basis[i/64][i%64],4);p32(basis_bytes+4*i,bits);
  }
  assert(sfc_crc32(basis_bytes,sizeof basis_bytes)==UINT32_C(0x0da929d3));
  for (unsigned k = 0; k < 64; k++)
    for (unsigned j = 0; j < 64; j++)
      assert(sfc_basis[k][63 - j] ==
             ((k & 1) ? -sfc_basis[k][j] : sfc_basis[k][j]));
  float coef[4096], actual[4096];
  double tmp[4096], ref[4096];
  double largest[2] = {0};
  for (unsigned trial = 0; trial < 32; trial++) {
    double l1 = 0;
    float scale = trial % 4 == 0   ? 1e-30f
                  : trial % 4 == 1 ? 1
                  : trial % 4 == 2 ? 1e10f
                                   : 1e30f;
    for (unsigned i = 0; i < 4096; i++) {
      coef[i] = random_float() * scale;
      if (trial % 3 == 0 && i % 19)
        coef[i] = 0;
      if (trial % 3 == 1)
        coef[i] = i == trial * 127 ? scale : 0;
      l1 += fabs((double)coef[i]);
    }
    for (unsigned y = 0; y < 64; y++)
      for (unsigned x = 0; x < 64; x++) {
        double a = 0;
        for (unsigned j = 0; j < 64; j++)
          a += (double)coef[y * 64 + j] * sfc_basis[j][x];
        tmp[y * 64 + x] = a;
      }
    for (unsigned y = 0; y < 64; y++)
      for (unsigned x = 0; x < 64; x++) {
        double a = 0;
        for (unsigned j = 0; j < 64; j++)
          a += tmp[j * 64 + x] * sfc_basis[j][y];
        ref[y * 64 + x] = a;
      }
    for (unsigned kernel = 1; kernel <= 2; kernel++) {
      dct(coef, actual, (int)kernel);
      for (unsigned i = 0; i < 4096; i++) {
        double error = fabs((double)actual[i] - ref[i]);
        double rounding = .5 * float_ulp(actual[i]);
        double normalized = fmax(0, error - rounding) / (l1 ? l1 : 1);
        largest[kernel - 1] = fmax(largest[kernel - 1], normalized);
        assert(error <= (kernel == 1 ? 1e-13 : 4e-7) * l1 + rounding + 1e-34);
      }
    }
  }
  for (unsigned shape = 0; shape < 5; shape++) {
    uint8_t known[4096];
    float input[4096], copy[4096];
    for (unsigned i = 0; i < 4096; i++) {
      unsigned x = i % 64, y = i / 64;
      known[i] = shape == 0   ? (x < 10 && y < 10)
                 : shape == 1 ? x == 10
                 : shape == 2 ? y == 10
                 : shape == 3 ? (x == y)
                              : (i % 7 == 0);
      input[i] = copy[i] =
          known[i] ? 2 + (float)x * .125f - (float)y * .25f : 0;
    }
    fill_invalid(input, known);
    for (unsigned i = 0; i < 4096; i++) {
      assert(finite_number(input[i]));
      if (known[i])
        assert(input[i] == copy[i]);
      if (shape == 0 || shape == 4)
        assert(fabs(input[i] - (2 + (float)(i % 64) * .125f -
                                (float)(i / 64) * .25f)) < 1e-4);
    }
  }
  /* Slicing implementation agrees with a scalar polynomial oracle, including
   * arbitrary alignment, lengths and incremental CRC boundaries. */
  uint8_t bytes[1024];
  for (unsigned i = 0; i < 1024; i++)
    bytes[i] = (uint8_t)(i * 73);
  for (unsigned off = 0; off < 8; off++)
    for (unsigned n = 0; n < 1000; n++) {
      uint32_t c = ~0u;
      for (unsigned i = 0; i < n; i++) {
        c ^= bytes[off + i];
        for (unsigned k = 0; k < 8; k++)
          c = (c >> 1) ^ (0xedb88320u & -(c & 1));
      }
      assert(sfc_crc32(bytes + off, n) == ~c);
      assert(~crc_update(crc_update(~0u, bytes + off, n / 2),
                         bytes + off + n / 2, n - n / 2) == ~c);
    }
  printf("inverse excess error/l1: f64 %.9g, f32 %.9g; fill and CRC OK\n",
         largest[0], largest[1]);
  return 0;
}
