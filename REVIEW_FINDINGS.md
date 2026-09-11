# surface-compressor: source review findings

Scope: read-only review of `src/surfcomp.c`, `surfcomp.h`, `src/dct64.h`, `src/entropy.h`,
`src/block_entropy.h`, `src/scan64.h`, `tools/main.c`, `tools/experiment_*`, `tests/`,
`spec/format.md`, `README.md`, `BENCHMARKS.md`, `CMakeLists.txt`. Nothing was compiled or run.
Line numbers refer to the working tree as of 2026-09-10. All impact estimates are unmeasured;
treat them as priorities to benchmark, not results.

Axes: **speed** (encode/decode wall time), **ratio** (bytes), **quality** (MAE/RMS/PSNR/SSIM at
equal bytes; the max-error contract is never to be relaxed), **correctness/robustness**.

Context the implementing agent needs:
- Blocks are independent 64x64 DCT tiles. Encoder: one float32 forward DCT, three scalar quant
  step candidates (0.5x/1x/2x tolerance), full float64 inverse DCT + per-sample verification per
  candidate, exact sample escapes on violation, raw/RLE fallback, then per-16-block-group tANS
  entropy coding with shared tables. Decoder: entry lookup (direct-addressed, one 32-byte pread),
  payload read, CRC32, table decode, sparse inverse DCT.
- Hard contract: per-sample absolute error <= tolerance. XYZ Euclidean budget E is split as
  E/sqrt(3) per axis in `tools/main.c:335`.
- Benchmarks show the budget is underused: measured max 0.0908 vs 0.1 limit and 0.927 vs 1.0.

---

## 0. Ranked action list

| # | Finding | Axis | Est. impact | Conf | Format change? |
|---|---|---|---|---|---|
| 1 | Thread the encoder (and decoder) across blocks / groups | speed | ~Ncore | high | no |
| 2 | Replace O(64^3) matmul DCT with a fast separable DCT (both directions) | speed | 5-15x transform | high (gap) / med (e2e) | no, but re-derive spec bound |
| 3 | Avoid full inverse DCT + verify per candidate (analytic pre-screen, pruning) | speed | 2-3x encode | high | no |
| 4 | Pick candidates by distortion among near-equal sizes, not size alone | quality | large RMS/MAE win at ~equal bytes | high | no |
| 5 | Remove the extra `tolerance * .95` margin (drift term already covers it) | ratio | 3-6% bytes | high | no |
| 6 | Deadzone quantization (+ optional trailing +/-1 zeroing pass) | ratio | 5-12% | med-high | no |
| 7 | Smooth inpaint fill for masked / out-of-image samples instead of last-sample hold | ratio | 5-20% on masked data | med-high | no |
| 8 | Joint Euclidean XYZ verification instead of E/sqrt(3) per-axis split | ratio | 15-30% bytes on tifxyz | high (conservatism) / med (size) | API change |
| 9 | More / adaptive quant step candidates (4x, 8x; bisection; energy-derived start) | ratio | 5-15% | med | no |
| 10 | Frequency-dependent quant matrix candidates | ratio | 10-25% plausible | med | yes (header byte) |
| 11 | CRC32: slicing-by-8 or PCLMUL/PMULL IEEE implementation | speed | 5-15% decode | high | no |
| 12 | Eliminate per-block mallocs/callocs on encode and read paths; borrow cache pointers | speed | few % encode, large on random paging | high | no |
| 13 | Batch index-entry reads per block row in `sfc_read_region`; larger table cache | speed | large for network/read_at backends | high | no |
| 14 | `SFC_TOKENS` overflow aborts a whole 16-block group instead of per-block fallback | correctness | hard encode fail | med | no |
| 15 | `sfc_finish` consumes the writer on failure; a following `sfc_cancel` double-frees | correctness | UAF | high | doc/API |
| 16 | Tools: verify/bench never compute MAE/RMS/PSNR/SSIM against the source | tooling | enables all of the above | high | no |

---

## 1. SPEED

### 1.1 Naive O(64^3) DCT kernel — `src/surfcomp.c:151-202`
Both forward (float32) and inverse (float64 accumulate) do two 64x64x64 matrix multiplies against
the `sfc_basis[64][64]` table in `src/dct64.h`: ~524k MACs per transform per direction. A
separable radix-2 / Lee / Hou / AAN 64-point DCT-II/III is O(n log n) per 1-D pass.
- BENCHMARKS.md says a recursive Lee *forward* experiment showed no consistent end-to-end gain.
  Likely because the inverse dominates and the existing sparse zero-skip already prunes it.
  Re-measure with the inverse converted too, and after 1.2.
- Minimum viable step if a butterfly is rejected: block the matmul into 4x4 register tiles,
  keep `restrict`, ensure FMA contraction, and consider explicit SIMD (NEON/AVX2) intrinsics.
- The spec pins the decoder inverse accumulation bound at `1e-13 * sum|dequantized coeff|`
  (`spec/format.md:87`). Any new inverse kernel must be shown to satisfy that bound (or the
  spec revised, see 3.2).
- `src/dct64.h` layout (16 KB table) is fine for the current kernel; a butterfly replaces it
  with O(64) twiddles that stay in L1.

### 1.2 Full inverse DCT + full verification per quantization candidate — `src/surfcomp.c:335-418`
Only the forward DCT is shared (comment at 332-333). Each surviving candidate does 4096
divide+round, varint serialization, `dct(recon, input, 1)` at :372, then a 4096-sample verify
loop. The `n >= rn` early-out at :368 correctly rejects oversized candidates before the inverse,
but every survivor pays the full inverse. Worst case: 3 inverse DCTs and 3 verify passes per block.
- Bound the escape count analytically before running the inverse: for scalar step s, L-inf
  reconstruction error <= sum over coefficients of |dequant error| * basis peak magnitude; derive
  from the coefficient histogram. Skip candidates certain to need many escapes.
- Exploit linearity between candidates where possible: recon(step) = IDCT(step * round(T/step)).
- Order candidates so an incumbent prunes later ones on a tighter bar than size alone.

### 1.3 Hot-loop micro-costs in the candidate loop — `src/surfcomp.c:342-406`
- `roundf(transformed[i] / step)` at :342: 4096 float divides per candidate. Hoist
  `inv = 1.0f/step`. Fast-math may do this; strict builds pay.
- `finite_number(q)` and `fabsf(q) > 4194304` per sample at :343: decide once from the max
  magnitude of `transformed` divided by step.
- `finite_number(orig)` at :396 is candidate-invariant but re-evaluated 4096x per candidate.
  Precompute `uint8_t orig_finite[4096]` once before the loop at :335.
- `invalid_depth` at :394-395 re-evaluates `c->flags & SFC_COORDINATE && name[0]=='z' && !name[1]`
  per sample. Hoist to one bool outside both loops.
- `float_ulp` calls `nextafterf` (libm, not always inlined). Replace with exponent extraction:
  for a normal float, ulp(x) = 2^(exp(x)-23).
- `c->dtype` switch at :382 is per sample. Specialize the verify loop per dtype (three bodies,
  one switch outside).
Expected: 10-30% of the verification pass, few % of encode overall.

### 1.4 Per-block allocation and zeroing on encode — `src/surfcomp.c:260`
Two 40000-byte `calloc`s (80 KB zeroed) per `sfc_encode_block`; 3480 blocks on the benchmark
input means ~278 MB of zeroing and 6960 malloc/free pairs. Only the 32-byte header and the
optional 512-byte mask bitset need zeroing (mask built with `|=` at :288).
- Add an internal encode entry point taking caller scratch; have the writer hold one scratch pair.
  Keep the public `sfc_encode_block` allocating for compatibility, but `memset` only `begin` bytes.

### 1.5 Per-call mallocs and memcpy on the read path — `src/surfcomp.c:809, 844, 851`
`sfc_read_block` allocates the payload buffer (:844) and, for entropy blocks,
`malloc(sizeof(se_model) * SE_NMODELS)` (:851). Each `se_model` holds a 1024-entry decode table;
with 10 models that is ~40-80 KB malloc'd and freed per block read, and re-`memcpy`'d out of
the table cache at :809 on every hit.
- Reader-scratch struct (payload buffer + one model set), thread-local or via a new
  `sfc_read_block_ex`. Hand out a borrowed cache pointer under a refcount/seqlock instead of copying.
Expected: significant on random-access paging (the renderer path in BENCHMARKS.md).

### 1.6 `sfc_read_region` triple-copies every sample — `src/surfcomp.c:876-907`
Decode into `block[16384]`, per-sample `memcpy` of 1-4 bytes into `tmp` (:892-898), then a second
per-sample copy into `dst` (:901-904). The staging is justified by the transactional contract
(failed reads leave outputs unchanged) and should stay.
- When pixel stride == sample size, `memcpy` whole `(x1-x0)` runs; make the final pass per-row.

### 1.7 Index entries read one at a time — `src/surfcomp.c:754-769, 884-899`
`entry()` is direct-addressed (O(1)) but each block costs a separate 32-byte pread. A 16x16-block
region issues 256 tiny syscalls before any payload. Not materializing the index is correct for
2^40-wide images, but a contiguous run of entries for one block row can be one read.
- Internal `entries(r, ch, bx0, bx1, by, buf)` reading `(bx1-bx0+1)*32` bytes; optional small
  LRU of 4 KB index pages. Big win for network/`read_at` backends.

### 1.8 CRC32 is byte-at-a-time — `src/surfcomp.c:98-102`
Every decoded block CRCs its payload (:848); every written block CRCs twice (:1091, :1101).
BENCHMARKS.md already credits a table CRC with 0.52 -> 0.43 s, so CRC is a measurable share.
- Slicing-by-8 (8 KB tables, ~4-6x) or carry-less multiply (PCLMUL on x86, PMULL on ARM).
- **Caveat:** hardware CRC32 instructions on both x86 SSE4.2 and ARMv8 compute CRC-32C
  (Castagnoli), not the IEEE polynomial used here. Using them means a format-breaking polynomial
  change. PCLMUL/PMULL preserves existing checksums and is the right choice.
- Trivial: group table CRC at :1091 is loop-invariant; hoist above the `for` at :1081.

### 1.9 No threading, no batch API — whole library and `tools/main.c:349-407, 498-511`
Blocks are independent; the only coupling is the shared tANS table per 16-block group.
`sfc_write_block` is strictly sequential and `flush_blocks` serializes the group.
- Library: encode the 16 blocks of a group on worker threads, then tokenize/build tables/flush
  serially (single writer preserves on-disk order). Add `sfc_read_blocks(..., count)`.
- Tools: worker pool in `encode()` and `decode_into()`. Expected 4-8x on typical cores.
- Reader thread safety: concurrent `sfc_read_block` on one reader appears safe (`r->channels`,
  `r->indices` read-only after open; table cache uses `atomic_flag`), but `surfcomp.h:42-43`
  only documents `read_at` synchronization. State the reader guarantee explicitly.

### 1.10 Table cache: 4 entries, contention bypasses the cache — `src/surfcomp.c:788-836`
The never-block design is sound, but on a contended bypass the table bytes are re-read, re-CRC'd,
and ten 1024-entry decode tables are rebuilt. Under N threads on different blocks of one group the
steady state can be "always bypass". 4 entries thrash at exactly 4 concurrent groups.
- 16-32 entries; seqlock or per-entry refcount so a miss under contention still populates.

### 1.11 Redundant varint round trip before tANS — `src/block_entropy.h:24-45`, `src/surfcomp.c:356-366, 1016-1021`
Encoder serializes `int32_t coef[4096]` to varints; `flush_blocks` calls `entropy_tokenize`, which
immediately re-parses the varints back into `int32_t coeff[4096]` to tokenize. Pass the
coefficient array (or the token stream) directly. Few % of encode; removes a parse failure path.

### 1.12 Tools: per-sample `get()` for x/y/z/mask — `tools/main.c:129-153`, called at `:365-397`
Four function calls plus cache-identity checks per sample. Read each distinct source tile once
per block into local buffers and extract via pointer arithmetic. Double-digit % of the TIFF
conversion phase, though dominated by codec cost until 1.1/1.2 land.

### 1.13 Build flags — `CMakeLists.txt`
CMake Release already implies `-O3` for GCC/Clang, so no change there. Missing: no `-march`/
`-mcpu` option, no LTO (`CMAKE_INTERPROCEDURAL_OPTIMIZATION`), no explicit vectorization report
flag. Add an opt-in `SFC_NATIVE` option and enable IPO for Release. Note `-ffast-math` is the
default; keep verifying that strict-math builds decode fast-math fixtures inside the bound.

### 1.14 Verified fine, no action
- Bit I/O (`src/entropy.h:101-164`): 64-bit accumulator, bulk 8-byte refill, no div/mod.
- 2-lane interleaved tANS decode (`src/entropy.h:424-434`): branch-light, XOR lane swap.
- Per-group table rebuild (`src/entropy.h:224-247, 350-382`): proportional, not a hotspot.
- `float coef[4096] = {0}` at `src/surfcomp.c:482`: 16 KB memset per decode is required by the
  run-skip decoder unless written positions are tracked; keep.

---

## 2. COMPRESSION RATIO

### 2.1 Extra 0.95 safety factor — `src/surfcomp.c:397`
`error > c->tolerance * .95` sits on top of the explicit `drift` term (:380) and, for integers,
the both-ends interval check (:387-389), which already cover numerical uncertainty. BENCHMARKS.md
confirms underuse (0.0908/0.1, 0.927/1.0).
- Use `error > c->tolerance`, or an explicit tiny relative epsilon such as `tolerance*(1-1e-6)`.
- Re-run the fast-math vs strict-math cross-decode fixture check listed in BENCHMARKS.md.

### 2.2 Only three candidates at 0.5/1/2x — `src/surfcomp.c:334`
Factor-of-2 spacing is coarse and non-adaptive: smooth blocks could take 8x with zero escapes;
rough blocks waste a full inverse on 0.5x.
- Cheapest: add 4x and 8x (pair with the pre-screen in 1.2 so they are not 5 inverse DCTs).
- Better: bisect on step from a size estimate.
- Best: derive the starting step from coefficient energy so the first probe usually wins.

### 2.3 Flat scalar step for all 4096 coefficients — `src/surfcomp.c:337, 342`
Same step for DC and the highest diagonal. A pure worst-case L-inf analysis gives little room
to differentiate by frequency (all 64x64 basis functions share the same L-inf bound within ~2x),
but the encoder verifies by exact reconstruction and escapes violations, so a frequency-dependent
matrix can be used aggressively: high-frequency errors are small and partially cancel.
- Add a few candidate quant matrices (flat, mild rolloff, strong rolloff) reusing the existing
  verify+escape machinery. Needs a matrix id in the block header: byte 7 and the u32 at offset 20
  are reserved. Format version bump.

### 2.4 Round-to-nearest, no deadzone, no RDO — `src/surfcomp.c:342`
- Deadzone: `sign * floorf(fabsf(t)/step + theta)` with theta ~0.4 instead of 0.5. More zeros,
  longer runs, fewer tokens. Safe by construction because the verifier escapes any violation.
  Watch for escape-budget blowups on high-detail blocks.
- Trailing-level RDO: late-scan +/-1 coefficients each cost a run token, a level token and a sign
  bit (`src/block_entropy.h:49-60`). A greedy "zero trailing +/-1s, re-verify" pass reusing the
  verify loop is cheap and often wins near EOB.

### 2.5 Masked samples filled by last-valid-sample hold — `src/surfcomp.c:318-329`
`input[i] = last;` for invalid samples carries the previous raster-order valid residual, so at a
row boundary the far-right value of one row is teleported to the far-left of the next. That step
is broadband: it costs high-frequency bits and its ringing bleeds into neighbouring valid samples,
forcing escapes or a smaller step. Masked positions cost zero error budget (skipped at :375,
overwritten at decode :527), so any smooth fill is free.
- Laplace inpainting (a few Jacobi/Gauss-Seidel sweeps from valid boundary samples) or a
  push-pull pyramid fill. The tifxyz benchmark is ~12.5% masked (4,029,118 valid of 4,604,600).

### 2.6 Edge/partial blocks use the same hold-fill — `src/surfcomp.c:1129-1131` with 2.5
Out-of-image samples are marked invalid and get the last-sample hold. For a 64x10 block the
remaining rows all replicate one sample, with a discontinuity at row 10. Replicating the last
*row* is a one-line improvement; mirror padding is better; the inpaint fill from 2.5 subsumes both.

### 2.7 Per-block header 32 bytes + raw 512-byte mask bitset — `src/surfcomp.c:284-290`
A mixed-validity block carries 544 bytes before any payload (up to 1.9 MB across 3480 blocks).
Validity masks are large contiguous regions; RLE typically cuts them 5-20x. Header bytes 8-23
(base, step, reserved) are dead weight for raw/RLE blocks (decoder validates them zero at :434-437).
- RLE or entropy-code the bitset; compact header for raw/RLE. Format change.

### 2.8 Escapes stored uncompressed — `src/surfcomp.c:400-404`, `spec/format.md:90-93`
`varint(delta) + z` raw bytes per escape (5+ bytes for float32), copied verbatim past the tANS
stream at :1068. Options: store a finely quantized residual against the reconstruction (far fewer
significant bits), or feed escapes through tANS with their own context. Up to ~10% on rough blocks.

### 2.9 XYZ E/sqrt(3) per-axis split is worst-case-tight — `tools/main.c:335`
Tight only when all three axes hit max simultaneously with aligned signs. Measured: requested 1.0,
max 0.927, RMS 0.281. Roughly a third of the linear budget is unused at the maximum.
- Encode x/y/z as a group (format already has `components`/`component`; `groups_ok` at
  `src/surfcomp.c:591` enforces contiguity), quantize each axis with a larger step (E/sqrt(2) or E),
  verify Euclidean displacement per point, escape only the axes needed. Requires a joint encode
  path across channels: real API change. The `experiment_xyz.py` PCA gains in BENCHMARKS.md
  (14.20x vs 10.59x) did not relax the per-axis budget, so these should compound.

### 2.10 `base` is the first valid sample, not the block mean — `src/surfcomp.c:312-317`
Forces DC to carry `mean - first_sample`. Using the valid-sample mean is free (base is float64),
makes DC near zero, and adds headroom against the `fabs(d) > 1e30` check at :324. <1% but free.

### 2.11 Entropy-model context granularity — `src/entropy.h:197-206`
`se_band_of` uses three buckets (pos<128, <1024, else; SE_NMODELS=10). For smooth surfaces most
nonzero AC energy is inside the first 128 scan positions. A finer split at the very lowest
frequencies (e.g. pos<16) could sharpen level/run models at the cost of a few more small
per-group tables. Low-to-mid single-digit %. Needs measurement.
- Lower priority: fixed diagonal scan (`src/scan64.h`) is not data-adaptive (few % at most);
  HybridUint 2+msb split (`src/entropy.h:172-193`) is fixed (<1%). Not worth changing without data.

### 2.12 Verified fine
- `n >= rn` raw-size bar at `src/surfcomp.c:368, 408` guarantees DCT never enlarges a block and
  updates as candidates win.
- Run/EOB/DC contexts sound; delta-DC experiment correctly kept out of the format.

---

## 3. QUALITY (MAE / RMS / PSNR / SSIM at equal bytes)

### 3.1 Candidate selection ignores distortion — `src/surfcomp.c:410-416`
Winner is the fewest bytes. Two candidates within 1% on size can differ 2x in RMS. The verify
loop already computes per-sample `error` at :381, so accumulating sum of squares is nearly free.
- Pick the smallest candidate, but among candidates within a small size tolerance (say 2%) pick
  the lowest RMS. Optionally expose a quality-bias knob. Pure encoder change.
- This is the single biggest lever on MAE/PSNR/SSIM without spending bytes.

### 3.2 The `1e-13 * l1` drift term is very generous — `src/surfcomp.c:347, 380`, `spec/format.md:86-88`
`l1` sums all 4096 |dequantized coefficients|, so smooth blocks with low-level coefficient noise
are penalized disproportionately. float64 accumulation error over 64 terms is ~64*DBL_EPSILON*l1
(~1.4e-14); 1e-13 is ~7x that, and it is applied on top of the 0.95 factor. Consider 1e-14 to
1e-15 with the cross-build fast/strict check. Spec edit required (the 1e-13 is normative).

### 3.3 Float outputs reject rather than clamp — `src/surfcomp.c:505-511` (integers OK at :132-141)
Integer paths clamp with `floor(d + .5)`, fine. For F32 the decoder returns SFC_INVALID when
`fabs(v) > FLT_MAX`. A different FP build could reject a block the encoder accepted. Clamp to
+/-FLT_MAX and let the encoder-verified bound govern.

### 3.4 Budget underuse is compound
Measured 0.908 and 0.927 of budget = 0.95 factor (2.1) x drift (3.2) x sqrt(3) split (2.9) x
coarse candidate spacing (2.2). Fixing 2.1 and 2.2 alone should push measured max to ~0.98.

### 3.5 Dithering / noise shaping: not applicable
Under a hard L-inf bound dither consumes budget. Correctly absent.

---

## 4. CORRECTNESS / ROBUSTNESS

### 4.1 `sfc_finish` consumes the writer on failure — `src/surfcomp.c:1203-1231`, `surfcomp.h:65-67`
On incomplete/failed state it calls `sfc_cancel(w)` and returns SFC_INVALID, freeing the writer.
The header does not say a failed finish destroys the writer; a caller that then calls
`sfc_cancel(w)` double-frees. Document that `sfc_finish` always consumes the writer, or make
failure leave the writer alive for an explicit cancel.

### 4.2 `SFC_TOKENS` cap can fail a whole group — `src/block_entropy.h:4, 18`, `src/surfcomp.c:1021-1022`
SFC_TOKENS = 8192; a dense block needs up to ~12287 tokens (DC + run/level per nonzero + EOB), so
`entropy_emit` returns SFC_INVALID and `flush_blocks` fails the entire 16-block group via `goto done`.
Likely unreachable in practice (dense blocks lose the `n >= rn` size check and go raw, which is
skipped at :1017-1018), but not provably so. On per-block tokenize failure leave `candidate[i]=NULL`
and keep that block's non-entropy form. Add an adversarial noisy-input test.

### 4.3 Missing local length guards (currently safe by non-local invariant)
- `entropy_tokenize` (`src/block_entropy.h:26`) reads `b[6]` and `b[24..27]` with no `n >= 32` check.
- `read_tables` (`src/surfcomp.c:792`) same pattern; safe only because `entry()` rejects `n < 32`.
Add `if (n < 32) return SFC_INVALID;` at both entries.

### 4.4 `SFC_BLOCK_BOUND` (40000) margin is underived — `surfcomp.h:11`, `src/surfcomp.c:343, 356-366`
Coefficient serialization writes into `b` before the `n >= rn` check. The `fabsf(q) > 4194304` cap
limits zigzag values to 24 bits = 4 varint bytes + 1 run byte: worst case 4096*5 + 544 = 21024.
Safe with ~2x margin, but nothing in the code says so. Add a comment or `_Static_assert`.

### 4.5 Fast-math and `cast_value` — `src/surfcomp.c:132-141, 379, 387-388`
`finite_number()` correctly uses bit inspection. But `cast_value` uses `fmax`/`fmin`, whose NaN
behaviour is unspecified under `-ffinite-math-only`, and it runs at :387-388 on `estimate +/- drift`
before the finiteness checks at :396. `estimate` comes from the inverse DCT without a prior finite
check. NaN is unlikely given earlier magnitude checks, but hoisting `finite_number(estimate)`
above :379 closes it for free.

### 4.6 `w->pending_count++` has no local bound — `src/surfcomp.c:1137`
`pending[SFC_GROUP]` = 16; the invariant is maintained across three code sites (:1118, :1173, :1177).
An explicit `if (w->pending_count >= SFC_GROUP) return SFC_INVALID;` makes it locally obvious.

### 4.7 CRC coverage gaps — `src/surfcomp.c:642, 765-767`, `spec/format.md`
- Header CRC covers bytes 0-47 only; channel descriptors and metadata are unchecked. A flipped bit
  in `tolerance` or `dtype` silently changes semantics.
- Index entries are unchecked. A corrupt offset pointing at a *different valid block* returns wrong
  data because that block's own CRC passes.
- Fix: CRC over descriptors+metadata in the header's reserved bytes 52-63 (currently must be zero,
  :644-646); seed each block CRC with its block index so swapped entries are detected. Format change.

### 4.8 Deep stack frames — `src/surfcomp.c:153, 185, 266, 318, 440-441, 482, 876`
`sfc_encode_block` ~80 KB locals + `dct` 32 KB `double tmp` = >110 KB; `sfc_read_region` +
`sfc_read_block` + `decode_block` ~75 KB. macOS non-main thread stacks default to 512 KB. Fine
today, tight for small-stack worker threads, which matters once 1.9 lands. Move `dct`'s `tmp`
and the encoder buffers into the scratch struct from 1.4.

### 4.9 `spec/format.md` is stale relative to the implementation
- Header says Version 1 at offset 4; `sfc_create` writes 2 (`src/surfcomp.c:955`), `sfc_open`
  accepts 1 or 2 (:638). Version 2 is undocumented.
- Block mode table lists 0=raw, 1=DCT, 2=RLE; mode 3 (entropy-coded, decoder :484-487) is absent.
- Shared-table reference structure (written :1062-1068, parsed :796-798: u64 offset, u32 size,
  u32 crc, u32 ANS length, u32 bypass length) is undocumented.
- Whether the block CRC covers the 32-byte block header or only the payload is not stated.
- No forward-compatibility / version-range policy.

### 4.10 Verified OK, no action needed
`sfc_write_block` mask handling (:1122-1156); `entry()` vs `sfc_block_range` float validation
(:765-767, :784); all-invalid block flag interaction (:278, :294, :432, :451); `flush_blocks`
error path (:1109-1120); `varget` shift bounds (:218-231); `strides()` overflow arithmetic
(:244-247); `sfc_open` rc init (:666); stdio/fd mixing (:607-621, :705-727, worth a comment);
byte packing / LEB128 canonical checks / refill bounds / token-range checks / table-sum validation
in `src/entropy.h` and `src/block_entropy.h`; `src/scan64.h` diagonal scan consistent with
`se_band_of` and both tokenizer and decoder.

---

## 5. TOOLS AND TESTS

### 5.1 No quality metrics anywhere — `tools/main.c:547-579` (`inspect()`), `tests/test_codec.c`
`verify`/`bench` only exercise `sfc_read_block` for CRC/decode timing; they never diff against the
source. Tests check max error only (`tests/test_codec.c:54-68, 78-103`). BENCHMARKS.md reports max
and RMS Euclidean only.
- Add `--reference DIR` to verify/bench, reusing `source_open`/`get`/`numeric` (`tools/main.c:74-165`),
  reporting per-channel max / MAE / RMS / PSNR, Euclidean stats for XYZ groups, and SSIM (a
  gaussian-window SSIM over each plane is ~40 lines of C). Also report bytes/sample and MB/s.
- Add MAE/PSNR assertions with regression thresholds in `tests/test_codec.c` so 3.1/2.4/2.5 can be
  evaluated without ad-hoc scripts.

### 5.2 Test coverage gaps
- No test of an all-invalid block asserting payload omission.
- No corruption test for index entries, channel descriptors, or truncated files (only payload
  mutation at `tests/test_codec.c:259-288` and one late-block corruption in `tests/test_cli.py:65-68`).
- No mixed-dtype file (uint8 + float32 channels), no >3-channel image, no RGBA.
- No adversarial dense-noise block to probe 4.2.
- No test that a fast-math build's fixtures decode inside the bound on a strict-math build is run
  automatically (fixtures are manual per `tests/test_codec.c:292-304`).
- No timing/throughput assertion for region reads (1.7) or paging (1.5).

### 5.3 Minor tool issues
- `tools/experiment_entropy.c:22-30` `emit()` relies on `assert()` for buffer bounds; unsafe under
  `-DNDEBUG`. Research tool, low severity.
- `tests/test_cli.py` needs numpy/tifffile/imagecodecs; CMake does not check or register it as a test.
- Public API has no ratio/quality bias knob, no batch encode, no multi-block read, no prefetch
  hint. `sfc_block_range` gives float min/max only. See 1.9 and 3.1 for the knobs worth adding.

---

## 6. Suggested implementation order

1. **Measurement first (5.1):** add MAE/RMS/PSNR/SSIM + throughput to verify/bench and tests so every
   change below is quantified.
2. **Free ratio/quality wins, no format change:** 2.1 (drop 0.95), 3.1 (distortion-aware selection),
   2.4 (deadzone), 2.10 (mean base), 2.5/2.6 (inpaint fill).
3. **Encoder speed, no format change:** 1.2 (pre-screen), 1.3 (hot loop), 1.4/1.11 (scratch, no
   varint round trip), 1.9 (threads), then 1.1 (fast DCT) with the spec bound re-derived.
4. **Decoder speed:** 1.5 (read scratch, borrowed cache), 1.8 (CRC), 1.7 (batched entries),
   1.10 (bigger cache), 1.6 (region memcpy).
5. **Robustness:** 4.1, 4.2, 4.3, 4.5, 4.6, 4.9 spec sync.
6. **Format-changing ratio work (bump version):** 2.9 (joint XYZ), 2.2 (more candidates), 2.3
   (quant matrices), 2.7 (mask RLE, compact raw header), 2.8 (escape coding), 4.7 (CRC coverage),
   3.2 (drift constant).


## Implementation follow-up (2026-09-10)

User constraint supersedes the threading recommendations: **no internal
threads, pthread dependency, thread controls or parallel API**. Scheduling is
owned by callers. Shared-reader safety is exercised by an external Python
caller; individual writer calls remain ordered. Findings 1.9 and the parallel
parts of 0/5.3/6 are deliberately not implemented.

Implemented in this pass: symmetric DCT products and explicit float32 inverse
contract (1.1/3.2); coarse-first candidate pruning (1.2); reciprocal, depth flag
hoisting and bit-based ULP spacing (1.3); header-only zeroing (1.4); reusable
read packets and borrowed reference-counted table pointers (1.5); row copies
and batched region index reads (1.6/1.7); slicing-by-eight IEEE CRC and hoisted
table CRC (1.8); 16-entry table cache with short bookkeeping-only locking
(1.10); opt-in native/IPO builds (1.13); explicit drift allowance without 0.95
(2.1); smooth hole/edge extension and centered blocks (2.5/2.6/2.10); lower
squared-error preference near equal sizes (3.1); writer ownership docs (4.1);
local length/queue bounds, scratch proof and pre-cast finite checks
(4.3–4.6); version-3 descriptor/metadata and position-bound index CRCs (4.7);
updated format and version policy (4.9); reference quality metrics, more
corruption/dense/shape tests, automatic cross-math fixtures and CLI test
registration (5.1–5.3). Caching uses bounded heap storage, with transform locals
still on the calling thread's stack; no worker-stack contract is imposed.

Corrections to review assumptions:
- A dense block requires 1 DC + 2×4095 AC tokens + 1 EOB = **8192 tokens**.
  Signs are bypass bits. The alleged 12287-token overflow (4.2/1.14) is not
  present; a static bound and dense-token regression test now prove it.
- An upper error bound cannot prove that a candidate needs many escapes.
  Analytic rejection on that basis would be unsound; coarse-first byte pruning
  is used instead.
- The stored float32 basis is the normative reference. Its difference from
  ideal cosine values is not additional cross-build accumulation error.
- The original 1e-13 allowance is retained for old blocks. New float32 blocks
  have their own explicit allowance; it is not reduced speculatively.
- F32 reconstruction overflow is rejected, not silently clamped. The encoder
  now checks estimate finiteness/range before casting or choosing a candidate.
- Near-equal-size quality preference may cost up to 2% in block bytes; it is
  not literally free. Real source metrics are used to assess the tradeoff.

Remaining optimization experiments, not confirmed correctness defects: deadzone
and trailing-level RDO; extra/adaptive steps; joint XYZ prediction/Euclidean
verification; quant matrices; compact masks/escapes; finer entropy contexts;
direct coefficient handoff instead of the varint roundtrip; encoder scratch
reuse; TIFF extraction batching. These require measured gains and, for some,
additional format/API design. They are not claimed as completed.

## V4 follow-up: accepted five-item implementation

The later request to implement all five proposed improvements is complete:

- Mixed validity uses alternating varint runs when the encoded prefix beats the
  512-byte bitmap. V4 gates the new flag; malformed lengths/runs are tested.
- Scalar container and joint-residual quantization candidates are evaluated by
  actual tANS bytes, mask/exception overhead and shared-table cost. Models are
  tried from preferred/all candidates and refined winners. Varint-size pruning
  remains in the cheaper independent XYZ fallback and standalone scalar API.
- J641 stores one 64x64 XYZ patch with float32 affine/PCA parameters and three
  child streams. It checks final Euclidean reconstruction plus numeric drift,
  uses exact XYZ escapes for outliers, and retains an independent-axis option.
  It does not reference neighboring patches. No integer transform was added.
- Writer candidate/entropy scratch and reader packet/XYZ workspaces are reused.
  TIFF XYZ/validity extraction happens once per patch; export and renderer use
  one joint read. Retained heap storage is dimension-independent (<7 MiB writer,
  <4 MiB shared reader, excluding descriptors/caller geometry/ROI staging).
- Four real inputs (two distinct surfaces, each at two resolutions), source
  metrics under strict/fast math, folds, holes, degenerate masks, large
  coordinates, corruption, sanitizer and caller-owned concurrency coverage.

Results and limitations are in BENCHMARKS.md: 12–18% smaller than V3, with higher
RMS inside the same maximum-error budget and generally higher encode/decode CPU
cost. The changes do not claim to simultaneously improve all three measures.
This supersedes the prior remaining-items list for joint XYZ, compact masks,
encoder scratch reuse and TIFF extraction batching. Deadzone/trailing-level RDO,
quantization matrices, direct coefficient handoff, alternate DCT algorithms and
finer entropy contexts remain experiments, not outstanding confirmed defects.

## Release-candidate follow-up

The new bounded LRU cache supports scalar/XYZ region reads, lazy allocation,
shared-cache synchronization with C11 atomics, counters and streamed output.
It creates no threads. Tests cover exact/float/joint tiles, eviction, masks,
strides, failure recovery, shared caller-owned concurrency and a virtual
100-million-square source with far-corner index offsets above 4 GiB.

Correction to the original finite-number assessment: bit inspection after FP
conversion is not sufficient under LLVM finite-only assumptions. The build now
uses `-ffast-math -fno-finite-math-only` and rejects finite-only compilation.
NaN payloads and infinities survive generic image roundtrips; nonfinite scalar
and XYZ parameters/packet fields are rejected. This policy retains floating
transforms, reassociation and FMA, without requiring bit-exact results.

Permanent compatibility fixtures, relocated static/shared installed consumers,
C/C++ linking, clean source packaging, fault injection and a cross-platform
exchange workflow are in place. Remote CI remains a release gate until run.
