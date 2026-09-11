#include "surfcomp.h"
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
typedef struct {
  unsigned char *data;
  size_t size, reads;
  int fail;
} source;
static int read_at(void *p, uint64_t offset, void *dst, size_t size) {
  source *s = p;
  s->reads++;
  if (s->fail || offset > s->size || size > s->size - offset)
    return -1;
  memcpy(dst, s->data + (size_t)offset, size);
  return 0;
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  FILE *f = fopen(argv[1], "rb");
  CHECK(f);
  CHECK(!fseek(f, 0, SEEK_END));
  long size = ftell(f);
  CHECK(size > 0);
  rewind(f);
  source s = {malloc((size_t)size), (size_t)size, 0, 0};
  CHECK(s.data);
  CHECK(fread(s.data, 1, s.size, f) == s.size);
  CHECK(!fclose(f));
  sfc_reader *r = NULL;
  CHECK(!sfc_open(read_at, &s, s.size, &r));
  sfc_cache *c = NULL;
  CHECK(!sfc_cache_create(r, 1024 * 1024, &c));
  const sfc_channel *info = sfc_channel_info(r, 0);
  int xyz = sfc_channel_count(r) == 3;
  size_t z = sfc_sample_size(info->dtype);
  unsigned char a[4096 * 12], b[4096 * 12], am[4096], bm[4096];
  CHECK(!sfc_read_block(r, 0, 0, 0, a, z, 64 * z, am));
  CHECK(!sfc_cache_read_region(c, 0, 0, 0, 64, 64, b, z, 64 * z, bm));
  CHECK(!memcmp(a, b, 4096 * z) && !memcmp(am, bm, 4096));
  sfc_cache_stats stats;
  CHECK(!sfc_cache_get_stats(c, &stats));
  CHECK(stats.misses == 1 && stats.hits == 0 && stats.resident_tiles == 1);
  size_t tile_bytes = stats.resident_bytes, reads = s.reads;
  CHECK(!sfc_cache_read_region(c, 0, 0, 0, 64, 64, b, z, 64 * z, bm));
  CHECK(s.reads == reads);
  if (xyz) {
    CHECK(!sfc_cache_read_xyz_region(c, 0, 0, 0, 64, 64, b, 12, 768, bm));
    CHECK(s.reads == reads);
    CHECK(!sfc_read_xyz(r, 0, 0, 0, a, 12, 768, am));
    CHECK(!memcmp(a, b, sizeof(a)) && !memcmp(am, bm, 4096));
    reads = s.reads;
    CHECK(!sfc_cache_read_region(c, 2, 0, 0, 64, 64, b, 4, 256, bm));
    CHECK(s.reads == reads);
    for (unsigned i = 0; i < 4096; i++)
      CHECK(!memcmp(a + i * 12 + 8, b + i * 4, 4));
  }
  sfc_cache_destroy(c);
  CHECK(!sfc_cache_create(r, tile_bytes * 2, &c));
  if (info->width > 128) {
    unsigned xs[] = {0, 64, 0, 128, 0, 64};
    for (unsigned i = 0; i < 6; i++)
      CHECK(!sfc_cache_read_region(c, 0, xs[i], 0, 1, 1, b, z, z, bm));
    CHECK(!sfc_cache_get_stats(c, &stats));
    CHECK(stats.misses == 4 && stats.hits == 2 && stats.evictions == 2);
    CHECK(stats.resident_tiles == 2 && stats.resident_bytes == tile_bytes * 2);
    /* Unaligned, strided region crosses all six edge tiles. */
    unsigned w = 128, h = 69;
    size_t ps = xyz ? 16 : 8, rs = w * ps + 16;
    unsigned char *out = malloc(h * rs), *mask = malloc(w * h);
    CHECK(out && mask);
    memset(out, 0xa5, h * rs);
    CHECK(!(xyz ? sfc_cache_read_xyz_region(c, 0, 1, 1, w, h, out, ps, rs, mask)
                : sfc_cache_read_region(c, 0, 1, 1, w, h, out, ps, rs, mask)));
    for (unsigned by = 0; by < 2; by++)
      for (unsigned bx = 0; bx < 3; bx++) {
        size_t n = xyz ? 12 : z;
        CHECK(!(xyz ? sfc_read_xyz(r, 0, bx, by, a, n, n * 64, am)
                    : sfc_read_block(r, 0, bx, by, a, n, n * 64, am)));
        for (unsigned yy = 0; yy < 64; yy++)
          for (unsigned xx = 0; xx < 64; xx++) {
            unsigned x = bx * 64 + xx, y = by * 64 + yy;
            if (x < 1 || x > 128 || y < 1 || y > 69)
              continue;
            CHECK(!memcmp(out + (y - 1) * rs + (x - 1) * ps,
                          a + (yy * 64 + xx) * n, n));
            CHECK(mask[(y - 1) * w + x - 1] == am[yy * 64 + xx]);
            CHECK(out[(y - 1) * rs + (x - 1) * ps + n] == 0xa5);
          }
      }
    free(out);
    free(mask);
  }
  memset(b, 0xa5, sizeof(b));
  CHECK(sfc_cache_read_region(c, 0, UINT64_MAX, 0, 1, 1, b, z, z, bm) ==
        SFC_INVALID);
  CHECK(sfc_cache_read_region(c, 0, 0, 0, 0, 1, b, z, z, bm) == SFC_INVALID);
  CHECK(sfc_cache_read_region(c, 0, 0, 0, 2, 2, b, SIZE_MAX, SIZE_MAX, bm) ==
        SFC_INVALID);
  CHECK(b[0] == 0xa5);
  sfc_cache_clear(c);
  s.fail = 1;
  CHECK(sfc_cache_read_region(c, 0, 0, 0, 1, 1, b, z, z, bm) == SFC_IO);
  CHECK(b[0] == 0xa5);
  CHECK(!sfc_cache_get_stats(c, &stats) && !stats.resident_tiles &&
        !stats.resident_bytes);
  s.fail = 0;
  CHECK(!sfc_cache_read_region(c, 0, 0, 0, 1, 1, b, z, z, bm));
  sfc_cache_destroy(c);
  CHECK(!sfc_cache_create(r, tile_bytes - 1, &c));
  reads = s.reads;
  CHECK(sfc_cache_read_region(c, 0, 0, 0, 1, 1, b, z, z, bm) == SFC_LIMIT &&
        reads == s.reads);
  sfc_cache_destroy(c);
  sfc_close(r);
  free(s.data);
  puts("cache: reuse, budget, LRU, strided edges, masks and failure recovery "
       "passed");
  return 0;
}
