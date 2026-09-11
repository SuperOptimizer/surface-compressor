#include "surfcomp.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
  const uint8_t *data;
  size_t size;
} memory;
static int read_at(void *user, uint64_t offset, void *out, size_t n) {
  memory *m = user;
  if (offset > m->size || n > m->size - (size_t)offset)
    return -1;
  memcpy(out, m->data + (size_t)offset, n);
  return 0;
}
static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static uint64_t u64(const uint8_t *p) {
  return u32(p) | (uint64_t)u32(p + 4) << 32;
}
static void p32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (i * 8));
}
static void p64(uint8_t *p, uint64_t v) {
  p32(p, (uint32_t)v);
  p32(p + 4, (uint32_t)(v >> 32));
}
static void container(const uint8_t *data, size_t n) {
  memory m = {data, n};
  sfc_reader *r = NULL;
  if (sfc_open(read_at, &m, n, &r))
    return;
  uint8_t out[4096 * 12], valid[4096];
  for (unsigned c = 0; c < sfc_channel_count(r) && c < 3; c++) {
    sfc_read_block(r, c, 0, 0, out, 4, 256, valid);
    sfc_read_region(r, c, 0, 0, 1, 1, out, 4, 4, valid);
    float lo, hi;
    sfc_block_range(r, c, 0, 0, &lo, &hi);
  }
  if (sfc_channel_count(r) >= 3)
    sfc_read_xyz(r, 0, 0, 0, out, 12, 768, valid);
  sfc_close(r);
}
/* Also reach structural/entropy parsing behind CRCs. Repair only checksums,
 * with independent bounds on every byte touched by this harness. */
static void repaired(const uint8_t *data, size_t n) {
  if (n < 64 || memcmp(data, "SFC1", 4) || u32(data + 8) > 16)
    return;
  uint32_t version = u32(data + 4), channels = u32(data + 8);
  if (version < 1 || version > 4)
    return;
  size_t desc_end = 64 + (size_t)channels * 128;
  if (desc_end > n)
    return;
  uint8_t *b = malloc(n);
  if (!b)
    return;
  memcpy(b, data, n);
  uint64_t ml = u64(b + 24);
  if (version >= 3 && u64(b + 16) == desc_end && ml <= n - desc_end)
    p32(b + 52, sfc_crc32(b + 64, desc_end - 64 + (size_t)ml));
  for (unsigned ch = 0; ch < channels; ch++) {
    const uint8_t *d = b + 64 + 128 * ch;
    uint64_t ix = u64(d + 96), count = u64(d + 104);
    if (ix > n || count > 64 || count > (n - ix) / 32)
      continue;
    for (uint64_t i = 0; i < count; i++) {
      uint8_t *e = b + (size_t)ix + (size_t)i * 32;
      uint64_t off = u64(e);
      uint32_t len = u32(e + 8);
      if (off <= n && len <= n - off)
        p32(e + 12, sfc_crc32(b + (size_t)off, len));
      if (version >= 3) {
        uint8_t salt[40];
        p32(salt, ch);
        p64(salt + 4, i);
        memcpy(salt + 12, e, 28);
        p32(e + 28, sfc_crc32(salt, 40));
      }
    }
  }
  uint8_t header[60];
  memcpy(header, b, 48);
  memcpy(header + 48, b + 52, 12);
  p32(b + 48, sfc_crc32(header, version >= 3 ? 60 : 48));
  container(b, n);
  free(b);
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t n) {
  if (n > 1048576)
    return 0;
  uint8_t out[4096 * 12], valid[4096];
  if (n >= 5 && data[4] >= 1 && data[4] <= 3)
    sfc_decode_block(data, n, (sfc_dtype)data[4], out, 4, 256, valid);
  if (n >= 4 && !memcmp(data, "J641", 4))
    sfc_decode_xyz_block(data, n, out, 12, 768, valid);
  container(data, n);
  repaired(data, n);
  return 0;
}
