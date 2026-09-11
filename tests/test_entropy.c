/* Exercise the real container path, including independent reads, table cache,
 * malformed entropy streams and unchanged coefficient reconstruction. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/surfcomp.c"
#include <assert.h>

typedef struct {
  uint8_t *bytes;
  size_t size;
  int restrict_reads;
  uint64_t index, block, table;
  size_t block_size, table_size, table_reads, bytes_read;
} memory_file;
static int memory_read(void *user, uint64_t off, void *dst, size_t n) {
  memory_file *m = user;
  if (off > m->size || n > m->size - off)
    return -1;
  if (m->restrict_reads && !((off == m->index && n == 32) ||
                             (off == m->block && n == m->block_size) ||
                             (off == m->table && n == m->table_size)))
    return -1;
  if (off == m->table && n == m->table_size)
    m->table_reads++;
  m->bytes_read += n;
  memcpy(dst, m->bytes + off, n);
  return 0;
}
static void select_block(memory_file *m, unsigned i) {
  m->index = 192 + i * 32;
  const uint8_t *e = m->bytes + m->index;
  m->block = u64(e);
  m->block_size = u32(e + 8);
  const uint8_t *b = m->bytes + m->block;
  assert(b[5] == 3);
  size_t at;
  assert(!block_prefix(b, m->block_size, &at, NULL));
  m->table = u64(b + at);
  m->table_size = u32(b + at + 8);
}
static void checksum_block(memory_file *m) {
  p32(m->bytes + m->index + 12, sfc_crc32(m->bytes + m->block, m->block_size));
}
static void reject(memory_file *m, unsigned i) {
  sfc_reader *r = NULL;
  m->restrict_reads = 0;
  assert(!sfc_open(memory_read, m, m->size, &r));
  float out[4096];
  uint8_t valid[4096];
  for (unsigned k = 0; k < 4096; k++) {
    out[k] = 987;
    valid[k] = 77;
  }
  assert(sfc_read_block(r, 0, i, 0, out, 4, 256, valid) == SFC_INVALID);
  for (unsigned k = 0; k < 4096; k++)
    assert(out[k] == 987 && valid[k] == 77);
  sfc_close(r);
}
int main(void) {
  enum { BLOCKS = 17 };
  char path[] = "/tmp/sfc-entropy-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  unlink(path);
  sfc_channel c = {.name = "x",
                   .width = BLOCKS * 64,
                   .height = 64,
                   .dtype = SFC_F32,
                   .flags = SFC_COORDINATE,
                   .tolerance = .1};
  sfc_writer *w;
  assert(!sfc_create(path, &c, 1, NULL, 0, &w));
  float (*expected)[4096] = malloc(BLOCKS * 4096 * sizeof(float));
  assert(expected);
  float in[4096], out[4096];
  uint8_t masks[BLOCKS][4096], valid[4096];
  uint32_t rng = 42;
  size_t legacy_size = 192 + BLOCKS * 32;
  for (unsigned b = 0; b < BLOCKS; b++) {
    for (unsigned i = 0; i < 4096; i++) {
      rng = rng * 1664525u + 1013904223u;
      in[i] = 1000 + b * 10 + sinf(i % 64 * .4f) * 8 + cosf(i / 64 * .3f) * 9 +
              (rng >> 24) * .01f;
      masks[b][i] = i % 31 != b % 31;
    }
    uint8_t *packet;
    size_t n;
    assert(!sfc_encode_block(&c, in, 4, 256, masks[b], &packet, &n));
    assert(packet[5] == 1);
    legacy_size += n;
    assert(!sfc_decode_block(packet, n, c.dtype, expected[b], 4, 256, NULL));
    free(packet);
    memcpy(expected[b], in, sizeof in);
    assert(!sfc_write_block(w, in, 4, 256, masks[b]));
  }
  assert(!sfc_finish(w));
  FILE *f = fopen(path, "rb");
  assert(f && !fseeko(f, 0, SEEK_END));
  memory_file m = {.size = (size_t)ftello(f)};
  assert(m.size < legacy_size);
  m.bytes = malloc(m.size);
  uint8_t *original = malloc(m.size);
  assert(m.bytes && original);
  rewind(f);
  assert(fread(m.bytes, 1, m.size, f) == m.size);
  fclose(f);
  memcpy(original, m.bytes, m.size);
  assert(u32(m.bytes + 4) == 4);
  sfc_reader *r;
  assert(!sfc_open(memory_read, &m, m.size, &r));
  assert(m.bytes_read == 192);
  /* Read the end of a group first. Only its own index, payload and table may
   * be accessed. Then every block in a permutation, including the tail group.
   */
  for (unsigned k = 0; k < BLOCKS; k++) {
    unsigned b = (15 + k * 7) % BLOCKS;
    select_block(&m, b);
    m.restrict_reads = 1;
    assert(!sfc_read_block(r, 0, b, 0, out, 4, 256, valid));
    for (unsigned i = 0; i < 4096; i++)
      assert(masks[b][i] ? fabs((double)out[i] - expected[b][i]) <= c.tolerance
                         : out[i] == -1);
    assert(!memcmp(valid, masks[b], sizeof valid));
  }
  assert(m.table_reads == 2);
  sfc_close(r);
  select_block(&m, 15);
  size_t at;
  assert(!block_prefix(m.bytes + m.block, m.block_size, &at, NULL));
  for (unsigned kind = 0; kind < 10; kind++) {
    memcpy(m.bytes, original, m.size);
    uint8_t *b = m.bytes + m.block, *p = b + at;
    switch (kind) {
    case 0:
      p64(p, u64(m.bytes + 32) - 1);
      break;
    case 1:
      p32(p + 8, SE_TABLES_MAX_BYTES + 1);
      break;
    case 2:
      p32(p + 12, u32(p + 12) ^ 1);
      break;
    case 3:
      p32(p + 16, 0);
      break;
    case 4:
      p32(p + 20, u32(p + 20) + 1);
      break;
    case 5:
      p32(b + 24, 23);
      break;
    case 6:
      p32(m.bytes + m.table, 0);
      p32(p + 12, sfc_crc32(m.bytes + m.table, m.table_size));
      break;
    case 7:
      p32(m.bytes + 4, 1);
      p32(m.bytes + 52, 0);
      p32(m.bytes + 48, header_crc(m.bytes));
      break;
    case 8:
      p64(p, m.block);
      break;
    case 9:
      memset(m.bytes + m.table, 255, m.table_size);
      p32(p + 12, sfc_crc32(m.bytes + m.table, m.table_size));
      break;
    }
    checksum_block(&m);
    reject(&m, 15);
  }
  /* Pass checksums so mutations reach table and entropy parsers. */
  for (unsigned trial = 0; trial < 1024; trial++) {
    memcpy(m.bytes, original, m.size);
    rng = rng * 1664525u + 1013904223u;
    uint8_t *p = m.bytes + m.block + at;
    if (trial & 1) {
      m.bytes[m.table + rng % m.table_size] ^= 1u << (trial % 8);
      p32(p + 12, sfc_crc32(m.bytes + m.table, m.table_size));
    } else {
      m.bytes[m.block + at + rng % (m.block_size - at)] ^= 1u << (trial % 8);
    }
    checksum_block(&m);
    m.restrict_reads = 0;
    assert(!sfc_open(memory_read, &m, m.size, &r));
    int rc = sfc_read_block(r, 0, 15, 0, out, 4, 256, valid);
    assert(rc == SFC_OK || rc == SFC_INVALID);
    sfc_close(r);
  }
  /* Descriptor corruption and index relocation are now covered in v3. */
  memcpy(m.bytes, original, m.size);
  m.bytes[64 + 88] ^= 1; /* A still-finite, structurally valid tolerance bit. */
  m.restrict_reads = 0;
  assert(sfc_open(memory_read, &m, m.size, &r) == SFC_INVALID && !r);
  memcpy(m.bytes, original, m.size);
  uint8_t swap[32];
  memcpy(swap, m.bytes + 192, 32);
  memcpy(m.bytes + 192, m.bytes + 192 + 15 * 32, 32);
  memcpy(m.bytes + 192 + 15 * 32, swap, 32);
  reject(&m, 15);
  memcpy(m.bytes, original, m.size);
  m.bytes[192 + 15 * 32 + 16] ^=
      1; /* Range checks do not need to read the payload. */
  assert(!sfc_open(memory_read, &m, m.size, &r));
  float lo, hi;
  assert(sfc_block_range(r, 0, 15, 0, &lo, &hi) == SFC_INVALID);
  sfc_close(r);
  assert(sfc_open(memory_read, &m, m.size - 1, &r) == SFC_INVALID && !r);

  /* Dense AC signs are bypass bits; the exact token maximum is 8192. */
  uint8_t dense[32 + 8192] = {0};
  memcpy(dense, "B641", 4);
  dense[5] = 1;
  p32(dense + 24, 8192);
  for (unsigned i = 0; i < 4096; i++) {
    dense[32 + i * 2] = 0;
    dense[33 + i * 2] = (i & 1) ? 1 : 2;
  }
  sfc_tokens *tokens = malloc(sizeof *tokens);
  assert(tokens);
  uint32_t counts[SE_NMODELS][SE_NTOK] = {{0}};
  assert(!entropy_tokenize(dense, sizeof dense, tokens, counts));
  assert(tokens->ns == SFC_TOKENS);
  for (size_t n = 0; n < 32; n++)
    assert(entropy_tokenize(dense, n, tokens, counts) == SFC_INVALID);
  free(tokens);
  free(original);
  free(m.bytes);
  free(expected);
  unlink(path);
  printf("shared entropy: %zu -> %zu bytes, independent reads and malformed "
         "streams OK\n",
         legacy_size, m.size);
  return 0;
}
