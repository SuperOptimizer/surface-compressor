/* Strict test oracle; the linked codec is built with the selected FP policy. */
#include "surfcomp.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static void put32(unsigned char *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (unsigned char)(v >> (8 * i));
}
static void put64(unsigned char *p, uint64_t v) {
  for (unsigned i = 0; i < 8; i++)
    p[i] = (unsigned char)(v >> (8 * i));
}
int main(void) {
  float in[4096], out[4096], xyz[4096][3];
  uint8_t mask[4096], valid[4096];
  sfc_channel c = {.name = "image",
                   .width = 64,
                   .height = 64,
                   .dtype = SFC_F32,
                   .tolerance = .05};
  uint32_t specials[] = {0x7fc12345, 0xffc54321, 0x7f800000, 0xff800000,
                         0x7f812345};
  for (unsigned special = 0; special < 5; special++) {
    for (unsigned i = 0; i < 4096; i++)
      in[i] = 100 + (i % 64) * .125f + (i / 64) * .25f;
    memcpy(in, &specials[special], 4);
    memcpy(in + 4095, &specials[special], 4);
    unsigned char *enc = NULL;
    size_t size = 0;
    CHECK(!sfc_encode_block(&c, in, 4, 256, NULL, &enc, &size));
    CHECK(!sfc_decode_block(enc, size, SFC_F32, out, 4, 256, valid));
    for (unsigned i = 0; i < 4096; i++) {
      CHECK(valid[i]);
      if (i == 0 || i == 4095)
        CHECK(!memcmp(in + i, out + i, 4));
      else
        CHECK(isfinite(out[i]) && fabs((double)in[i] - out[i]) <= c.tolerance);
    }
    free(enc);
  }
  for (unsigned i = 0; i < 4096; i++)
    in[i] = 100 + (i % 64) * .125f + (i / 64) * .25f;
  unsigned char *enc = NULL;
  size_t size = 0;
  CHECK(!sfc_encode_block(&c, in, 4, 256, NULL, &enc, &size));
  CHECK(enc[5] == 1);
  unsigned char *bad = malloc(size);
  CHECK(bad);
  uint64_t doubles[] = {UINT64_C(0x7ff8000000000123),
                        UINT64_C(0x7ff0000000000000),
                        UINT64_C(0xfff0000000000000)};
  for (unsigned k = 0; k < 3; k++) {
    memcpy(bad, enc, size);
    put64(bad + 8, doubles[k]);
    memset(out, 0xa5, sizeof(out));
    CHECK(sfc_decode_block(bad, size, SFC_F32, out, 4, 256, valid) ==
          SFC_INVALID);
    CHECK(((unsigned char *)out)[0] == 0xa5);
    memcpy(bad, enc, size);
    put32(bad + 16, specials[k]);
    CHECK(sfc_decode_block(bad, size, SFC_F32, out, 4, 256, valid) ==
          SFC_INVALID);
    memcpy(&c.tolerance, &doubles[k], 8);
    unsigned char *rejected = NULL;
    size_t n = 0;
    CHECK(sfc_encode_block(&c, in, 4, 256, NULL, &rejected, &n) == SFC_INVALID);
    CHECK(!rejected);
  }
  free(bad);
  free(enc);
  for (unsigned i = 0; i < 4096; i++) {
    mask[i] = 1;
    xyz[i][0] = 100 + i % 64;
    xyz[i][1] = 100 + i / 64;
    xyz[i][2] = 100;
  }
  for (unsigned k = 0; k < 5; k++) {
    memcpy(xyz[0], &specials[k], 4);
    enc = NULL;
    CHECK(sfc_encode_xyz_block(xyz, 12, 768, mask, .25, &enc, &size) ==
          SFC_INVALID);
    CHECK(!enc);
  }
  xyz[0][0] = 100;
  for (unsigned k = 0; k < 3; k++) {
    double error;
    memcpy(&error, &doubles[k], 8);
    enc = NULL;
    CHECK(sfc_encode_xyz_block(xyz, 12, 768, mask, error, &enc, &size) ==
          SFC_INVALID);
    CHECK(!enc);
  }
  CHECK(!sfc_encode_xyz_block(xyz, 12, 768, mask, .25, &enc, &size));
  bad = malloc(size);
  CHECK(bad);
  for (unsigned k = 0; k < 3; k++) {
    memcpy(bad, enc, size);
    put64(bad + 8, doubles[k]);
    CHECK(sfc_decode_xyz_block(bad, size, xyz, 12, 768, valid) == SFC_INVALID);
    memcpy(bad, enc, size);
    put32(bad + 32, specials[k]);
    CHECK(sfc_decode_xyz_block(bad, size, xyz, 12, 768, valid) == SFC_INVALID);
  }
  free(bad);
  free(enc);
  puts("nonfinite samples preserved; invalid scalar/XYZ parameters and packets "
       "rejected");
  return 0;
}
