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
static float input[4096], output[4096];
static void cases(FILE *write, FILE *read) {
  for (int kind = 0; kind < 8; kind++) {
    sfc_channel c = {.name = "x",
                     .width = 64,
                     .height = 64,
                     .dtype = SFC_F32,
                     .flags = SFC_COORDINATE,
                     .tolerance = .1 / sqrt(3)};
    uint8_t mask[4096], decoded[4096];
    for (unsigned i = 0; i < 4096; i++) {
      float x = (float)(i % 64), y = (float)(i / 64);
      mask[i] = (kind == 4) ? 0 : (kind == 3) ? i % 7 != 0 : 1;
      input[i] = kind == 0   ? 12345
                 : kind == 1 ? 50000 + x * .02f + y * .03f
                 : kind == 2 ? 100 + x * x * .01f + sinf(y * .1f)
                 : kind == 5 ? (float)(i * 7919 % 65536)
                 : kind == 6 ? 1e20f + x
                             : 10 + x * .1f - y * .2f;
    }
    if (kind == 7) {
      uint32_t nan = 0x7fc12345, inf = 0x7f800000;
      memcpy(input + 10, &nan, 4);
      memcpy(input + 11, &inf, 4);
    }
    uint8_t *enc = NULL;
    size_t n = 0;
    assert(!sfc_encode_block(&c, input, 4, 256, mask, &enc, &n));
    assert(n <= SFC_BLOCK_BOUND);
    if(kind==4)assert(n==32 && enc[5]==2 && (enc[6]&2));
    if (write) {
      uint32_t nn = (uint32_t)n;
      assert(fwrite(&nn, 4, 1, write) == 1);
      assert(fwrite(enc, 1, n, write) == n);
    }
    if (read) {
      uint32_t nn;
      assert(fread(&nn, 4, 1, read) == 1 && nn <= SFC_BLOCK_BOUND);
      free(enc);
      n = nn;
      enc = malloc(n);
      assert(enc && fread(enc, 1, n, read) == n);
    }
    assert(!sfc_decode_block(enc, n, SFC_F32, output, 4, 256, decoded));
    double max = 0;
    for (unsigned i = 0; i < 4096; i++) {
      assert(decoded[i] == mask[i]);
      if (!mask[i]) {
        assert(output[i] == -1);
        continue;
      }
      if (!isfinite(input[i])) {
        assert(!memcmp(input + i, output + i, 4));
        continue;
      }
      double err = fabs((double)output[i] - input[i]);
      assert(err <= c.tolerance);
      if (err > max)
        max = err;
    }
    printf("case %d: %zu bytes, max %.9g, mode %u\n", kind, n, max, enc[5]);
    memset(output, 0x5a, sizeof output);
    float guard[4096];
    memcpy(guard, output, sizeof guard);
    assert(sfc_decode_block(enc, n - 1, SFC_F32, output, 4, 256, decoded));
    assert(!memcmp(guard, output, sizeof guard));
    free(enc);
  }
  for (int type = 1; type <= 2; type++)
    for (int exact = 0; exact < 2; exact++) {
      sfc_channel c = {.name = "image",
                       .width = 64,
                       .height = 64,
                       .dtype = (sfc_dtype)type,
                       .flags = exact ? SFC_EXACT : 0,
                       .tolerance = exact ? 0 : 1};
      uint8_t data[8192], out[8192];
      size_t z = sfc_sample_size(c.dtype);
      for (unsigned i = 0; i < 4096; i++) {
        uint16_t v = (uint16_t)(i % 256);
        memcpy(data + i * z, &v, z);
      }
      uint8_t *enc;
      size_t n;
      assert(!sfc_encode_block(&c, data, z, 64 * z, NULL, &enc, &n));
      assert(!sfc_decode_block(enc, n, c.dtype, out, z, 64 * z, NULL));
      for (unsigned i = 0; i < 4096; i++) {
        uint16_t a = 0, b = 0;
        memcpy(&a, data + i * z, z);
        memcpy(&b, out + i * z, z);
        assert(abs((int)a - (int)b) <= (exact ? 0 : 1));
      }
      free(enc);
    }
}
static void container(void) {
  char path[] = "/tmp/sfc-test-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  unlink(path);
  sfc_channel c[2] = {{.name = "x",
                       .width = 65,
                       .height = 67,
                       .dtype = SFC_F32,
                       .flags = SFC_COORDINATE,
                       .tolerance = .01},
                      {.name = "generations",
                       .width = 65,
                       .height = 67,
                       .dtype = SFC_U16,
                       .flags = SFC_EXACT}};
  const char meta[] = "{\"scale\":[0.05,0.05],\"custom\":123}";
  sfc_writer *w;
  FILE *existing = fopen(path, "wb");
  assert(existing);
  assert(fwrite("keep", 1, 4, existing) == 4);
  fclose(existing);
  assert(sfc_create(path, c, 2, meta, sizeof meta - 1, &w));
  assert(!w);
  existing = fopen(path, "rb");
  char keep[4];
  assert(existing);
  assert(fread(keep, 1, 4, existing) == 4);
  fclose(existing);
  assert(!memcmp(keep, "keep", 4));
  unlink(path);
  assert(!sfc_create(path, c, 2, meta, sizeof meta - 1, &w));
  for (unsigned ch = 0; ch < 2; ch++)
    for (unsigned by = 0; by < 2; by++)
      for (unsigned bx = 0; bx < 2; bx++) {
        float f[4096];
        uint16_t u[4096];
        for (unsigned i = 0; i < 4096; i++) {
          f[i] = (float)(by * 64 + i / 64) * .1f +
                 (float)(bx * 64 + i % 64) * .01f;
          u[i] = (uint16_t)(by * 64 + i / 64);
        }
        assert(!sfc_write_block(w, ch ? (void *)u : (void *)f, ch ? 2 : 4,
                                ch ? 128 : 256, NULL));
      }
  assert(!sfc_finish(w));
  sfc_reader *r;
  assert(!sfc_open_file(path, &r));
  assert(sfc_channel_count(r) == 2 && sfc_find_channel(r, "generations") == 1);
  char m[128] = {0};
  assert(!sfc_read_metadata(r, m, sizeof meta - 1));
  assert(!strcmp(m, meta));
  float roi[9];
  uint8_t mask[9];
  assert(!sfc_read_region(r, 0, 62, 62, 3, 3, roi, 4, 12, mask));
  for (unsigned i = 0; i < 9; i++) {
    assert(mask[i]);
    assert(fabs(roi[i] - ((float)(62 + i / 3) * .1f +
                          (float)(62 + i % 3) * .01f)) < .01);
  }
  float lo, hi;
  assert(!sfc_block_range(r, 0, 0, 0, &lo, &hi) && lo <= 0 && hi >= 6.93f);
  memset(roi, 0x55, sizeof roi);
  float guard[9];
  memcpy(guard, roi, sizeof roi);
  assert(sfc_read_region(r, 0, 64, 66, 3, 3, roi, 4, 12, mask));
  assert(!memcmp(guard, roi, sizeof roi));
  sfc_close(r);
  FILE *f = fopen(path, "r+b");
  assert(f);
  assert(!fseek(f, -1, SEEK_END));
  int v = fgetc(f);
  assert(!fseek(f, -1, SEEK_END));
  fputc(v ^ 255, f);
  fclose(f);
  assert(!sfc_open_file(path, &r));
  uint16_t block[4096];
  assert(sfc_read_block(r, 1, 1, 1, block, 2, 128, NULL));
  sfc_close(r);
  f=fopen(path,"r+b");assert(f);
  assert(!fseek(f,64+2*128+3,SEEK_SET));v=fgetc(f);
  assert(!fseek(f,64+2*128+3,SEEK_SET));fputc(v^1,f);fclose(f);
  assert(sfc_open_file(path,&r)==SFC_INVALID && !r);
  unlink(path);
  assert(!sfc_create(path,c,2,meta,sizeof meta-1,&w));
  assert(sfc_finish(w)==SFC_INVALID); /* Consumed: never cancel again. */
  assert(access(path,F_OK)!=0);
}
typedef struct {
  uint8_t header[192], entry[32], *payload;
  size_t payload_size, bytes, calls;
  uint64_t data;
} virtual_file;
static void put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v) {
  for (unsigned i = 0; i < 8; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}
static int virtual_read(void *user, uint64_t offset, void *dst, size_t n) {
  virtual_file *v = user;
  v->calls++;
  v->bytes += n;
  if (offset < 192 && n <= 192 - offset)
    memcpy(dst, v->header + (size_t)offset, n);
  else if (offset >= 192 && offset < v->data && (offset - 192) % 32 == 0 &&
           n == 32)
    memcpy(dst, v->entry, n);
  else if (offset == v->data && n == v->payload_size)
    memcpy(dst, v->payload, n);
  else
    return -1;
  return 0;
}
static void huge_index(void) {
  /* A virtual 2^40-wide image with a 512 GiB index: opening it and reading
   * its final block must use a few hundred bytes, not scan/materialize it. */
  virtual_file v = {0};
  sfc_channel c = {.name = "image",
                   .width = UINT64_C(1) << 40,
                   .height = 64,
                   .dtype = SFC_U8,
                   .flags = SFC_EXACT};
  uint8_t pixels[4096];
  memset(pixels, 42, sizeof pixels);
  assert(
      !sfc_encode_block(&c, pixels, 1, 64, NULL, &v.payload, &v.payload_size));
  v.data = 192 + (UINT64_C(1) << 34) * 32;
  memcpy(v.header, "SFC1", 4);
  put32(v.header + 4, 1);
  put32(v.header + 8, 1);
  put64(v.header + 16, 192);
  put64(v.header + 32, v.data);
  put64(v.header + 40, v.data + v.payload_size);
  put32(v.header + 48, sfc_crc32(v.header, 48));
  memcpy(v.header + 64, "image", 6);
  put64(v.header + 128, c.width);
  put64(v.header + 136, 64);
  put32(v.header + 144, SFC_U8);
  put32(v.header + 148, SFC_EXACT);
  put64(v.header + 160, 192);
  put64(v.header + 168, UINT64_C(1) << 34);
  put64(v.entry, v.data);
  put32(v.entry + 8, (uint32_t)v.payload_size);
  put32(v.entry + 12, sfc_crc32(v.payload, v.payload_size));
  sfc_reader *r = NULL;
  assert(!sfc_open(virtual_read, &v, v.data + v.payload_size, &r));
  assert(sfc_channel_info(r, 0)->width == c.width);
  memset(pixels, 0, sizeof pixels);
  assert(
      !sfc_read_block(r, 0, (UINT64_C(1) << 34) - 1, 0, pixels, 1, 64, NULL));
  for (unsigned i = 0; i < 4096; i++)
    assert(pixels[i] == 42);
  assert(v.bytes < 512 && v.calls <= 5);
  assert(sfc_read_block(r, 0, UINT64_C(1) << 34, 0, pixels, 1, 64, NULL));
  sfc_close(r);
  free(v.payload);
}
static void malformed_blocks(void) {
  float input_block[4096], decoded[4096];
  uint8_t valid[4096];
  for (unsigned i = 0; i < 4096; i++)
    input_block[i] = (float)(i % 64) * .125f;
  sfc_channel c = {.dtype = SFC_F32, .tolerance = .01};
  uint8_t *encoded = NULL;
  size_t n = 0;
  assert(!sfc_encode_block(&c, input_block, 4, 256, NULL, &encoded, &n));
  uint8_t *mutated = malloc(n);
  assert(mutated);
  for (size_t cut = 0; cut < n; cut++) {
    for (unsigned i = 0; i < 4096; i++)
      decoded[i] = 123;
    memset(valid, 77, sizeof valid);
    assert(sfc_decode_block(encoded, cut, SFC_F32, decoded, 4, 256, valid));
    for (unsigned i = 0; i < 4096; i++)
      assert(decoded[i] == 123 && valid[i] == 77);
  }
  uint32_t state = 12345;
  for (unsigned trial = 0; trial < 512; trial++) {
    memcpy(mutated, encoded, n);
    state = state * 1664525u + 1013904223u;
    size_t at = state % n;
    mutated[at] ^= (uint8_t)(1u << (trial % 8));
    /* Structurally valid mutations may decode; sanitizer checks both paths. */
    (void)sfc_decode_block(mutated, n, SFC_F32, decoded, 4, 256, valid);
  }
  free(mutated);
  free(encoded);
}
int main(int argc, char **argv) {
  FILE *w = NULL, *r = NULL;
  if (argc == 3) {
    if (!strcmp(argv[1], "write"))
      w = fopen(argv[2], "wb");
    else
      r = fopen(argv[2], "rb");
    assert(w || r);
  }
  assert(sfc_crc32("123456789", 9) == UINT32_C(0xcbf43926));
  cases(w, r);
  if (w)
    fclose(w);
  if (r)
    fclose(r);
  container();
  huge_index();
  malformed_blocks();
  puts("surface codec: OK");
  return 0;
}
