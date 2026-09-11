#include "surfcomp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static float scalar(unsigned x, unsigned y) {
  return (float)(10000 + x * .13 + y * .21 + sin(x * .09) * cos(y * .11));
}
static void xyz(unsigned x, unsigned y, float p[3]) {
  p[0] = scalar(x, y);
  p[1] = (float)(20000 + y * .7 + sin(x * .08));
  p[2] = (float)(30000 + x * .3 + y * .1 + cos(y * .07));
}
int main(int argc, char **argv) {
  CHECK(argc == 3 || argc == 4);
  const char *path = argv[2];
  if (!strcmp(argv[1], "write")) {
    CHECK(argc == 4);
    int joint = !strcmp(argv[3], "xyz"), exact = !strcmp(argv[3], "exact");
    sfc_channel c[3] = {{0}};
    for (unsigned k = 0; k < (joint ? 3u : 1u); k++) {
      strcpy(c[k].name, joint   ? (k == 0   ? "x"
                                   : k == 1 ? "y"
                                            : "z")
                        : exact ? "image"
                                : "x");
      c[k].width = exact ? 64 : 129;
      c[k].height = exact ? 64 : 70;
      c[k].dtype = exact ? SFC_U16 : SFC_F32;
      c[k].flags = exact ? SFC_EXACT : SFC_COORDINATE;
      c[k].tolerance = exact ? 0 : .25;
#ifdef SFC_XYZ
      if (joint) {
        c[k].flags |= SFC_XYZ;
        c[k].components = 3;
        c[k].component = k;
      }
#else
      CHECK(!joint);
#endif
    }
    sfc_writer *w = NULL;
    CHECK(!sfc_create(path, c, joint ? 3 : 1, "compat", 6, &w));
    for (unsigned by = 0; by < (c[0].height + 63) / 64; by++)
      for (unsigned bx = 0; bx < (c[0].width + 63) / 64; bx++) {
        float data[4096][3];
        uint16_t integers[4096];
        uint8_t valid[4096];
        for (unsigned i = 0; i < 4096; i++) {
          unsigned x = bx * 64 + i % 64, y = by * 64 + i / 64;
          xyz(x, y, data[i]);
          integers[i] = (uint16_t)((i * 7919u) % 65536);
          valid[i] = exact ? 1 : (x % 17 != 0 && y % 19 != 0);
        }
#ifdef SFC_XYZ
        if (joint)
          CHECK(!sfc_write_xyz_block(w, data, 12, 768, valid));
        else
#endif
          CHECK(!sfc_write_block(w, exact ? (void *)integers : (void *)data,
                                 exact ? 2 : 12, exact ? 128 : 768, valid));
      }
    CHECK(!sfc_finish(w));
    return 0;
  }
  CHECK(!strcmp(argv[1], "read"));
  sfc_reader *r = NULL;
  CHECK(!sfc_open_file(path, &r));
  const sfc_channel *c = sfc_channel_info(r, 0);
  CHECK(c);
  char metadata[6];
  CHECK(sfc_metadata_size(r) == 6 && !sfc_read_metadata(r, metadata, 6) &&
        !memcmp(metadata, "compat", 6));
  unsigned joint = sfc_channel_count(r) == 3, exact = c->dtype == SFC_U16;
  for (unsigned by = 0; by < (c->height + 63) / 64; by++)
    for (unsigned bx = 0; bx < (c->width + 63) / 64; bx++) {
      float out[4096][3];
      uint16_t integers[4096];
      uint8_t valid[4096];
#ifdef SFC_XYZ
      if (joint)
        CHECK(!sfc_read_xyz(r, 0, bx, by, out, 12, 768, valid));
      else
#endif
        CHECK(!sfc_read_block(r, 0, bx, by,
                              exact ? (void *)integers : (void *)out,
                              exact ? 2 : 12, exact ? 128 : 768, valid));
      for (unsigned i = 0; i < 4096; i++) {
        unsigned x = bx * 64 + i % 64, y = by * 64 + i / 64;
        int expected = x < c->width && y < c->height &&
                       (exact || (x % 17 != 0 && y % 19 != 0));
        CHECK(valid[i] == expected);
        if (exact) {
          CHECK(integers[i] == (uint16_t)((i * 7919u) % 65536));
          continue;
        }
        float source[3];
        xyz(x, y, source);
        double square = 0;
        for (unsigned k = 0; k < (joint ? 3u : 1u); k++) {
          if (!expected)
            CHECK(out[i][k] == -1);
          else {
            double d = (double)source[k] - out[i][k];
            square += d * d;
          }
        }
        CHECK(!expected || square <= .25 * .25);
      }
    }
  sfc_close(r);
  return 0;
}
