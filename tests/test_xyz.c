#include "surfcomp.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static float src[4096][3], out[4096][3];
static uint8_t mask[4096], valid[4096];
static void fixture(unsigned kind) {
  for (unsigned i = 0; i < 4096; i++) {
    double x = i % 64, y = i / 64,
           origin = kind == 4   ? 100000
                    : kind == 5 ? 1000000
                    : kind == 6 ? 4000000
                                : 1000;
    double fold =
        kind == 2 ? fabs(x - 31) * 8 : sin(x * .11) * cos(y * .08) * 2;
    if (kind == 9)
      fold = (i * 7919 % 8192) * .37;
    src[i][0] = (float)(origin + x * .63 + fold);
    src[i][1] = (float)(origin + y * .7 - fold * .8);
    src[i][2] = (float)(origin + x * .1 + y * .2 + fold * .3);
    mask[i] = kind == 0   ? 0
              : kind == 3 ? (x < 17 || y < 9)
              : kind == 7 ? i == 2048
              : kind == 8 ? x == 31
                          : 1;
  }
}
static void check(double error) {
  for (unsigned i = 0; i < 4096; i++) {
    assert(valid[i] == mask[i]);
    double d = 0;
    for (unsigned k = 0; k < 3; k++) {
      if (!mask[i])
        assert(out[i][k] == -1);
      else {
        double e = (double)out[i][k] - src[i][k];
        d += e * e;
      }
    }
    if (mask[i]) {
      assert(sqrt(d) <= error);
      assert(out[i][2] > 0);
    }
  }
}
static void masks(void) {
  sfc_channel c = {.dtype = SFC_F32, .flags = SFC_EXACT | SFC_COORDINATE};
  float data[4096], decoded[4096];
  uint8_t vm[4096], got[4096];
  for (unsigned pattern = 0; pattern < 2; pattern++) {
    for (unsigned i = 0; i < 4096; i++) {
      data[i] = 42;
      vm[i] = pattern ? (i % 2) : (i < 1024);
    }
    uint8_t *b;
    size_t n;
    assert(!sfc_encode_block(&c, data, 4, 256, vm, &b, &n));
    assert(((b[6] & 8) != 0) == !pattern);
    assert(!sfc_decode_block(b, n, SFC_F32, decoded, 4, 256, got));
    assert(!memcmp(vm, got, 4096));
    if (!pattern) {
      unsigned offsets[] = {32, 33, 34, 35};
      for (unsigned j = 0; j < 4; j++) {
        uint8_t old = b[offsets[j]];
        b[offsets[j]] = 255;
        memset(decoded, 0x55, sizeof decoded);
        assert(sfc_decode_block(b, n, SFC_F32, decoded, 4, 256, got));
        assert(((uint8_t *)decoded)[0] == 0x55);
        b[offsets[j]] = old;
      }
    }
    free(b);
  }
}
int main(int argc, char **argv) {
  FILE *f = argc == 3 ? fopen(argv[2], argv[1][0] == 'w' ? "wb" : "rb") : NULL;
  if (argc == 3)
    assert(f);
  for (unsigned kind = 0; kind < 10; kind++) {
    fixture(kind);
    uint8_t *packet = NULL;
    size_t n = 0;
    double e = .25;
    if (f && argv[1][0] == 'r') {
      assert(fread(&n, sizeof n, 1, f) == 1 && n <= SFC_XYZ_BOUND);
      packet = malloc(n);
      assert(packet);
      assert(fread(packet, 1, n, f) == n);
    } else
      assert(!sfc_encode_xyz_block(src, 12, 768, mask, e, &packet, &n));
    assert(n >= 224 && n <= SFC_XYZ_BOUND);
    if (f && argv[1][0] == 'w') {
      assert(fwrite(&n, sizeof n, 1, f) == 1);
      assert(fwrite(packet, 1, n, f) == n);
    }
    assert(!sfc_decode_xyz_block(packet, n, out, 12, 768, valid));
    check(e);
    if (!f) {
      /* Every header byte is either validated or carries numeric parameters;
       * target reserved/structural fields, plus truncations. Output is atomic.
       */
      unsigned bad[] = {4,  5,   7,   16,  17,  20,  24,
                        28, 104, 111, 112, 116, 120, 124};
      for (unsigned j = 0; j < sizeof bad / sizeof bad[0]; j++) {
        uint8_t old = packet[bad[j]];
        packet[bad[j]] = 255;
        memset(out, 0x55, sizeof out);
        memset(valid, 0x55, sizeof valid);
        assert(sfc_decode_xyz_block(packet, n, out, 12, 768, valid));
        assert(((uint8_t *)out)[0] == 0x55 && valid[0] == 0x55);
        packet[bad[j]] = old;
      }
      assert(sfc_decode_xyz_block(packet, n - 1, out, 12, 768, valid));
    }
    if (!f && kind == 1) {
      unsigned rng = 1234567;
      for (unsigned trial = 0; trial < 128; trial++) {
        rng = rng * 1664525u + 1013904223u;
        size_t at = rng % n;
        uint8_t old = packet[at];
        packet[at] ^= (uint8_t)(1u << (trial % 8));
        memset(out, 0x55, sizeof out);
        memset(valid, 0x55, sizeof valid);
        int rc = sfc_decode_xyz_block(packet, trial % 4 ? n : n - 1, out, 12,
                                      768, valid);
        if (rc) {
          for (size_t j = 0; j < sizeof out; j++)
            assert(((uint8_t *)out)[j] == 0x55);
          for (unsigned j = 0; j < 4096; j++)
            assert(valid[j] == 0x55);
        }
        packet[at] = old;
      }
    }
    free(packet);
  }
  if (f) {
    assert(!fclose(f));
    return 0;
  }
  masks();
  /* Group index entries, partial edge, scalar/ROI and joint APIs agree. */
  char path[128];
  snprintf(path, sizeof path, "/tmp/sfc-xyz-%ld.sfc", (long)getpid());
  unlink(path);
  sfc_channel c[3] = {0};
  for (unsigned k = 0; k < 3; k++) {
    c[k].name[0] = (char)('x' + k);
    c[k].width = 65;
    c[k].height = 63;
    c[k].dtype = SFC_F32;
    c[k].flags = SFC_XYZ | SFC_COORDINATE;
    c[k].components = 3;
    c[k].component = k;
    c[k].tolerance = .25;
  }
  sfc_writer *w;
  assert(!sfc_create(path, c, 3, NULL, 0, &w));
  fixture(1);
  assert(sfc_write_block(w, src, 12, 768, mask) == SFC_INVALID);
  assert(!sfc_write_xyz_block(w, src, 12, 768, mask));
  assert(!sfc_write_xyz_block(w, src, 12, 768, mask));
  assert(!sfc_finish(w));
  sfc_reader *r;
  assert(!sfc_open_file(path, &r));
  assert(!sfc_read_xyz(r, 0, 1, 0, out, 12, 768, valid));
  for (unsigned i = 0; i < 4096; i++)
    mask[i] = (i % 64 == 0 && i / 64 < 63);
  check(.25);
  float scalar[4096];
  assert(!sfc_read_block(r, 1, 1, 0, scalar, 4, 256, NULL));
  for (unsigned i = 0; i < 4096; i++)
    assert(scalar[i] == out[i][1]);
  float roi[8];
  assert(!sfc_read_region(r, 2, 63, 0, 2, 4, roi, 4, 8, NULL));
  sfc_close(r);
  unlink(path);
  puts("joint XYZ: folds, holes, sparse/degenerate masks, large coordinates "
       "and malformed packets passed");
}
