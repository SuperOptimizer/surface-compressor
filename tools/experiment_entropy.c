/* Research only: reuse volume-compressor's tANS with independently restarted
 * surface-block streams and tables shared by a bounded group. Does not write
 * a new surface container. Compile from the surface-compressor root:
 * clang -O3 -ffast-math -fno-finite-math-only -I. -I../volume-compressor tools/experiment_entropy.c
 *       -lm -o build/experiment-entropy
 * Usage: experiment-entropy file.sfc [group_blocks=64] [delta_dc=0]
 * Size estimates include tables and 16 extra bytes per DCT block for stream
 * lengths / shared-table addressing. Every encoded token stream is decoded
 * and compared with its original tokens. Existing bypass bits are unchanged.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/surfcomp.c"
#include "volcomp.h"
#include <assert.h>
#define GROUP_MAX 64
static uint16_t symbols[GROUP_MAX][8192];
static uint8_t bypass[GROUP_MAX][20000];
static size_t nsymbols[GROUP_MAX], nbypass[GROUP_MAX], retained[GROUP_MAX];
static vf_model models[VF_NMODELS];
static vf_etab etabs[VF_NMODELS];
static uint16_t scan[4096];
static uint32_t counts[VF_NMODELS][VF_NTOK];
static size_t emit(uint16_t *syms, size_t at, vf_bitw *bw, uint32_t ctx,
                   uint32_t u) {
  uint32_t tok;
  assert(vf_hyb_emit(bw, u, &tok));
  assert(tok < VF_NTOK);
  syms[at++] = (uint16_t)((ctx << 8) | tok);
  counts[ctx][tok]++;
  return at;
}
static uint64_t flush_group(unsigned count) {
  if (!count)
    return 0;
  uint8_t tables[VF_TABLES_MAX_BYTES];
  for (unsigned m = 0; m < VF_NMODELS; m++) {
    unsigned used = 0;
    for (unsigned t = 0; t < VF_NTOK; t++)
      used += counts[m][t];
    if (!used)
      counts[m][0] = 1;
    assert(vf_model_build(models + m, counts[m]));
  }
  uint64_t bytes = 4 + vf_tables_write(models, tables);
  vf_etabs_init(models, etabs);
  for (unsigned b = 0; b < count; b++) {
    uint32_t fields[8194];
    uint8_t encoded[20000];
    size_t n = vf_tans_encode2(etabs, symbols[b], nsymbols[b], fields, encoded,
                               sizeof encoded);
    assert(n);
    vf_rdec decoder;
    assert(vf_rdec_init(&decoder, encoded, n));
    for (size_t k = 0; k < nsymbols[b]; k++) {
      unsigned ctx = symbols[b][k] >> 8;
      assert(vf_rdec_get(&decoder, models + ctx) == (symbols[b][k] & 255));
    }
    assert(vf_rdec_finished(&decoder));
    bytes += retained[b] + n + nbypass[b] + 16;
  }
  memset(counts, 0, sizeof counts);
  return bytes;
}
int main(int argc, char **argv) {
  if (argc < 2)
    return 1;
  unsigned group = argc > 2 ? (unsigned)atoi(argv[2]) : 64;
  int delta = argc > 3 ? atoi(argv[3]) : 0;
  assert(group > 0 && group <= GROUP_MAX);
  unsigned p = 0;
  for (unsigned sum = 0; sum <= 126; sum++)
    for (unsigned y = 0; y < 64; y++) {
      if (sum < y || sum - y >= 64)
        continue;
      scan[p++] = (uint16_t)(y * 64 + sum - y);
    }
  assert(p == 4096 && scan[0] == 0);
  sfc_reader *r = NULL;
  assert(!sfc_open_file(argv[1], &r));
  uint64_t bytes = r->data_off, dc_bytes = 0, coefficient_bytes = 0, blocks = 0;
  for (uint32_t ch = 0; ch < r->count; ch++) {
    const sfc_channel *c = r->channels + ch;
    unsigned pending = 0;
    int32_t previous_dc = 0;
    for (uint64_t by = 0; by < tiles(c->height); by++)
      for (uint64_t bx = 0; bx < tiles(c->width); bx++) {
        uint8_t e[32], packet[SFC_BLOCK_BOUND];
        assert(!entry(r, ch, bx, by, e));
        size_t n = u32(e + 8);
        assert(!read_bytes(r, u64(e), packet, n));
        assert(sfc_crc32(packet, n) == u32(e + 12));
        if (packet[5] != 1) {
          bytes += n;
          continue;
        }
        int32_t coeff[4096] = {0};
        size_t begin = 32 + ((packet[6] & 1) ? 512 : 0), cn = u32(packet + 24),
               at = begin, end = begin + cn;
        unsigned i = 0;
        assert(end <= n);
        while (i < 4096) {
          uint64_t run, v;
          assert(!varget(packet, end, &at, &run) && run <= 4096 - i);
          i += (unsigned)run;
          if (i == 4096)
            break;
          size_t start = at;
          assert(!varget(packet, end, &at, &v) && v <= 8388608);
          coeff[i] = vf_unzigzag((uint32_t)v);
          if (i == 0)
            dc_bytes += at - start;
          i++;
        }
        assert(at == end);
        coefficient_bytes += cn;
        blocks++;
        vf_bitw bw;
        vf_bw_init(&bw, bypass[pending], sizeof bypass[pending]);
        size_t nt = emit(symbols[pending], 0, &bw, VF_DC_CTX,
                         vf_zigzag(coeff[0] - (delta ? previous_dc : 0)));
        previous_dc = coeff[0];
        unsigned prev = 0;
        for (unsigned k = 1; k < 4096; k++)
          if (coeff[scan[k]]) {
            int32_t v = coeff[scan[k]];
            unsigned run = k - prev - 1;
            nt = emit(symbols[pending], nt, &bw, vf_run_ctx(prev + 1), run);
            nt = emit(symbols[pending], nt, &bw, vf_level_ctx(k, run),
                      (uint32_t)(v < 0 ? -v : v) - 1);
            assert(vf_bw_put(&bw, v < 0, 1));
            prev = k;
          }
        unsigned ctx = vf_run_ctx(prev + 1);
        symbols[pending][nt++] = (uint16_t)((ctx << 8) | VF_TOK_EOB);
        counts[ctx][VF_TOK_EOB]++;
        assert(nt <= 8192);
        nsymbols[pending] = nt;
        assert(vf_bw_flush(&bw, nbypass + pending));
        retained[pending] = n - cn;
        if (++pending == group) {
          bytes += flush_group(pending);
          pending = 0;
          previous_dc = 0;
        }
      }
    bytes += flush_group(pending);
  }
  printf("{\"group\":%u,\"delta_dc\":%d,\"original_bytes\":%llu,\"estimated_"
         "bytes\":%llu,\"coefficient_stream_bytes\":%llu,\"dc_varint_bytes\":%"
         "llu,\"dct_blocks\":%llu}\n",
         group, delta, (unsigned long long)r->size, (unsigned long long)bytes,
         (unsigned long long)coefficient_bytes, (unsigned long long)dc_bytes,
         (unsigned long long)blocks);
  sfc_close(r);
  return 0;
}
