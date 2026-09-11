# Local validation, 2026-09-10

Apple M4, macOS, Clang Release build, `SFC_FAST_MATH=ON`. Timings are individual
local runs with `/usr/bin/time -l`, not a cross-platform performance claim.

Input: PHercParis4 segment `20230702185753`, mesh
`20230702185753-on-20260411134726-2.4um.tifxyz`, downloaded from the
[Vesuvius open-data bucket](https://vesuvius-challenge-open-data.s3.amazonaws.com/PHercParis4/segments/20230702185753/mesh/20230702185753-on-20260411134726-2.4um.tifxyz/meta.json).
Three 1820×2530 float32 planes; 4,029,118 valid XYZ samples.

| Measurement | Result |
|---|---:|
| Original sample bytes | 55,255,200 |
| Compressed file bytes, including index/metadata | 15,870,830 |
| Compression ratio against sample bytes | 3.48× |
| Maximum Euclidean XYZ error | 0.0737548 voxels |
| RMS Euclidean XYZ error | 0.0127007 voxels |
| Requested maximum XYZ error | 0.1 voxels |
| Encode wall time | 1.25 s |
| Full decode + payload CRC verification | 0.43 s |
| Encoded blocks (all three channels) | 3,480 |
| Encoder maximum RSS reported by time | 57,622,528 bytes |
| Decoder maximum RSS reported by time | 2,048,000 bytes |

Invalid samples reconstructed exactly as (-1,-1,-1). Metadata bytes matched.
Comparison used float64 Euclidean differences against the original TIFF planes.
Timing commands:

```sh
surface-compressor encode input.tifxyz output.sfc --error 0.1
surface-compressor bench output.sfc
surface-compressor decode output.sfc restored.tifxyz
```

The initial float32-inverse prototype's conservative numerical guard fell back
to raw storage too often (1.13× on this sample). Widening inverse accumulation,
reserving per-sample rounding margin, and retaining exact escapes gave the
result above. A table-based CRC reduced the full decode/verify run from 0.52 s
to 0.43 s without changing the file format or checksum values.

## Renderer paging smoke test

A synthetic 1400×1300 surface over a local 512³ CT fixture was opened, then
paged to global grid point (1399,1299). The final resident window was
(376,276), 1024×1024. The 64 MiB cache reported 182 hits, 363 misses, and
17,842,176 resident bytes across the initial and requested windows.

Preparing the new window synchronously produced a 66.2 ms maximum navigation
stage. Moving decode and normal/geometry preparation to a worker reduced it
to 8.3 ms in this smoke test. GPU upload remains on the UI thread. This is a
single paging operation, not a sustained interaction benchmark.

## Checks

* Fast-math and strict-math builds read each other's encoded fixtures and satisfy
  error bounds; identical encoded bytes are not required.
* uint8/uint16 exact and lossy images, float32 surfaces, nonfinite values,
  validity, partial blocks and cross-block ROIs.
* Multichannel RGB and tifxyz auxiliary-channel round trips, integer-scaled
  masks, exact generations and metadata preservation.
* Truncated/mutated blocks, corrupt payloads, transactional output and exclusive
  creation/temporary-file ownership checks under ASan/UBSan.
* Virtual 2^40-wide image with a 512 GiB index: opening and reading the final
  block accesses under 512 bytes and never materializes the index.
* Bounded cache eviction, concurrent reads, asynchronous job cleanup and global
  grid positioning above 2^53.
* render3d full suite: 49 passed, one existing shard-fixture test skipped.

Remaining viewer limits: compressed surfaces are read-only; plane intersections
cover the resident window, and whole-surface overview LOD is not implemented.
The format is experimental. Linux and other CPU architectures have not been
validated in this session.


## Adaptive quantization and error-budget sweep

The encoder now tries quantization steps of 0.5, 1, and 2 times the per-component
error limit for each block. It retains the smallest payload **including exact
corrections**. All candidates undergo the same reconstruction-error checks.
This is an encoder change; existing SFC1 decoders can read the result.

Measured on the same 2.4 µm surface, plus its native 7.91 µm counterpart
`20230702185753-on-20230205180739-7.91um.tifxyz` (559×776 XYZ samples):

| Data | XYZ limit (voxels) | File bytes | Ratio vs raw XYZ | Maximum error | RMS error |
|---|---:|---:|---:|---:|---:|
| 2.4 µm | 0.1 | 12,893,087 | 4.29× | 0.090799 | 0.034289 |
| 2.4 µm | 0.25 | 9,195,931 | 6.01× | 0.227855 | 0.086229 |
| 2.4 µm | 0.5 | 6,942,802 | 7.96× | 0.460175 | 0.158267 |
| 2.4 µm | 1 | 5,218,926 | 10.59× | 0.927115 | 0.281058 |
| 7.91 µm | 0.1 | 2,669,053 | 1.95× | 0.083295 | 0.028389 |
| 7.91 µm | 0.25 | 2,330,145 | 2.23× | 0.224574 | 0.075183 |
| 7.91 µm | 0.5 | 1,932,896 | 2.69× | 0.448139 | 0.170073 |
| 7.91 µm | 1 | 1,441,356 | 3.61× | 0.922051 | 0.376990 |

Validity is exact in every case. These are measured Euclidean XYZ errors over
valid samples, with differences computed in float64. The 2.4 µm source contains
55,255,200 sample bytes; the 7.91 µm source contains 5,205,408 sample bytes.
No masks or categorical fields receive a larger tolerance.

At 1.0 voxel on the 2.4 µm surface, the old single-step encoder produced
8,137,933 bytes (6.79×). Adaptive selection produces 5,218,926 bytes (10.59×),
an additional 35.9% size reduction at the same specified error budget.
At the original 0.1-voxel budget it improves 3.48× to 4.29×.

In the paired local sweep, encoding the 2.4 µm surface at 1.0 voxel increased
from 2.45 s to 4.27 s; full decode/verification remained approximately 0.86 s.
Do not compare these timings directly against the earlier isolated run: system
load differed. Neither file sizes nor measured error depend on that timing.

A 1.0-voxel limit at 2.4 µm permits 2.4 µm displacement. A 0.25-voxel limit at
8–9 µm permits 2–2.25 µm displacement. The 7.91 µm measurement is not a benchmark
of every 8–9 µm dataset, and its considerably lower ratio should not be hidden
by the better 2.4 µm result. The CLI keeps tolerance explicit (`--error`); tifxyz
`scale` describes grid sampling and is not a reliable CT voxel-size field.


## Joint XYZ experiment (not yet a container mode)

`tools/experiment_xyz.py` compares the real block codec on four representations:

1. Independent XYZ, using the new adaptive quantization.
2. A float32 PCA rotation of centered XYZ into a local coordinate frame.
3. Subtracting a fitted affine XYZ prediction from grid coordinates (u,v).
4. Affine prediction followed by PCA rotation of the remaining XYZ residuals.

The final option removes both predictable grid variation and correlation
between coordinate channels. Each 64×64 XYZ group remains independent.
The three residual channels still use floating-point DCTs. Decoding reverses
the rotation and adds the predictor, then checks displacement in original XYZ.

| Data and error budget | Independent XYZ | Rotation | Affine predictor | Predictor + rotation | Best per block |
|---|---:|---:|---:|---:|---:|
| 2.4 µm, 1 voxels | 10.59× | 11.26× | 12.73× | 14.17× | 14.20× |
| 7.91 µm, 0.25 voxels | 2.23× | 2.26× | 2.35× | 2.39× | 2.40× |
| 2.4 µm, 0.1 voxels | 4.29× | 4.66× | 4.58× | 4.99× | 5.01× |

For 2.4 µm / 1.0 voxel, selecting the best representation per block gives an
estimated 3,891,251-byte file versus 5,218,926 bytes with independent XYZ:
25.4% smaller. Maximum final XYZ error is 0.919427 voxel; RMS is 0.267142.
For 7.91 µm / 0.25 voxel, the estimate is 2,171,444 bytes versus 2,330,145 bytes,
6.8% smaller. Maximum final XYZ error is 0.219730 voxel.

**These are prototype size estimates, not files in an implemented joint format.**
The experiment actually encodes and decodes every residual block. Its byte
counts include the existing three-channel index, metadata, float32 predictor
and rotation parameters (36 bytes each), and a four-byte group mode selector.
It also simulates exact XYZ escapes for any violation after the final inverse
transform, charging 16 bytes per correction; none were needed in these runs.
The prototype rounds stored parameters before encoding and evaluates errors
again after reconstructing float32 world coordinates.

A production implementation needs a shared XYZ group packet, format/version
handling, one-decode XYZ read API, robust C fitting/rotation code, and tests of
malformed group payloads and cross-build numerical margins. The current
production codec still encodes coordinate channels independently. General
images and exact auxiliary channels need not use the XYZ-specific transform.


## Sparse inverse and vectorized transforms

The floating-point inverse now skips zero coefficients and inactive frequency
rows, and accumulates contiguous output samples to allow SIMD. Accumulation
remains float64 with float32 inputs, coefficients, and output. The forward
transform also uses contiguous accumulation. The block format is unchanged.

On the Apple M4, a paired local run on the 2.4 µm / 1.0-voxel surface measured
warm full decode/verification at 0.455–0.498 s before and 0.092–0.094 s after
(about 5× faster). Encoding measured 2.196 s before and 0.984 s after (2.2×).
These are local timings, sensitive to system load, not throughput guarantees.
The new encoded file is 5,218,993 bytes; maximum XYZ error is 0.927115 voxel
and RMS is 0.281058. All 4,029,118 valid samples were checked; invalid samples
remain exactly -1. Strict/fast-math cross-decoding, ASAN/UBSAN codec tests,
and TIFF CLI integration tests pass. A recursive Lee forward-DCT experiment
did not show a consistent additional end-to-end benefit and was not retained.

## Shared entropy models experiment (historical prototype)

`tools/experiment_entropy.c` applies volume-compressor's run/level tokens,
frequency contexts, HybridUint coding, and two-lane tANS to existing surface
DCT coefficients. It shares tables across a bounded group while restarting
entropy coding for every 64×64 block. A cache miss would require that block's
payload and shared tables, without decoding neighboring blocks.

| Surface / error | Existing bytes | Shared tables, 16 blocks | Shared tables, 64 blocks |
|---|---:|---:|---:|
| 2.4 µm / 1 voxel | 5,218,926 | 2,866,700 | 2,862,224 |
| 7.91 µm / 0.25 voxel | 2,330,145 | 1,074,263 | 1,079,205 |

The 16-block estimates represent 45.1% and 53.9% smaller files respectively,
without changing coefficients or reconstruction quality. The harness builds
and serializes actual entropy tables, encodes and decodes every token, and
checks token equality and final states. Estimates retain current headers,
masks, exceptions and indexes, and budget 16 additional bytes per block and
four bytes per group for stream lengths and references. They are not new SFC
files: production table addressing, reader caching and malformed-stream tests
were not implemented in this prototype; the implementation below supersedes
these estimates. These results are separate from the joint XYZ
experiment; their gains must not simply be multiplied.

Volume-compressor predicts DC from the preceding block within a substream;
it does not share all coefficient values. In this surface experiment, adding
DC prediction to the 64-block model saves only another 129 bytes on the
2.4 µm input. Independent block streams are preferable for this workload.

Build the research harness with an adjacent volume-compressor checkout:

```sh
clang -O3 -ffast-math -I. -I../volume-compressor tools/experiment_entropy.c -lm -o /tmp/sfc-entropy
/tmp/sfc-entropy surface.sfc 16 0
```


## Implemented shared entropy mode (container version 2)

The writer now emits real shared-table files, with independently restarted
64×64 streams, groups of up to 16 blocks, per-block and per-group size fallback,
CRC-protected table references and a four-entry reader table cache. No DC
prediction or joint XYZ transform is used. The core has no volume-compressor
build dependency; the MIT entropy primitives are included locally.

Paired Apple M4 runs (three runs each; timings below are medians):

| Surface / error | Varint bytes | Shared bytes | Reduction | Raw/shared ratio | Encode before → after | Full verify before → after |
|---|---:|---:|---:|---:|---:|---:|
| 2.4 µm / 1 voxel | 5,218,993 | 2,890,813 | 44.6% | 19.11× | 1.957 → 2.006 s | 0.187 → 0.210 s |
| 7.91 µm / 0.25 voxel | 2,330,230 | 1,074,422 | 53.9% | 4.84× | 0.355 → 0.355 s | 0.053 → 0.054 s |

The baseline already includes the sparse/vectorized transform optimization.
Do not compare absolute times with earlier runs under different system load.
The larger surface takes about 13% more full-decode time for the reduced size;
the smaller sample's decode timings are close. These measurements use local
files, not network transfer. The 24-byte production stream descriptor and
actual grouping/fallback decisions account for differences from estimates.

Maximum XYZ errors are 0.927115 and 0.224426 voxel respectively, with RMS
0.281058 and 0.075177. Every valid sample was compared to the original TIFFs;
invalid coordinates remain exactly -1. The entropy test additionally compares
new and old reconstructed blocks exactly within one build, isolating transport
correctness from permitted differences between floating-point builds.

Validation includes ASAN/UBSAN, strict/fast-math builds, TIFF CLI integration,
version 1 read compatibility, decoding the last block of a group first while
forbidding neighboring payload reads, table cache reuse across random reads,
a partial final group, explicit malformed references/tables/stream lengths,
and 1024 checksum-repaired mutations reaching the entropy parser. New files
require a version-2-capable reader; the updated render3d snapshot includes it.


## Synchronous codec improvements and review fixes (container v3)

All numbers below are **single-caller, synchronous** operation. The codec starts
no threads, has no pthread dependency and exposes no parallel API. An external
caller may safely share a reader across its own threads. The worker-pool
experiment was removed in accordance with the user's constraint.

The retained changes combine symmetric transform products, explicitly marked
float32 inverse accumulation, smooth hole/edge extension, mean-centered blocks,
coarse-first candidate pruning, distortion preference within 2% of candidate
size, slicing-by-eight CRC, borrowed table-cache pointers and reusable read
packets. V3 additionally protects descriptors, metadata and index positions.
Old files continue using their original inverse arithmetic and CRC rules.

Paired Apple M4 local runs, three trials each (medians; baseline already includes
shared entropy tables):

| Surface / error | Previous bytes | New bytes | Reduction | Encode before → after | Full verify before → after |
|---|---:|---:|---:|---:|---:|
| 2.4 µm / 1 voxel | 2,890,813 | 1,950,217 | 32.5% | 2.156 → 1.375 s | 0.212 → 0.141 s |
| 7.91 µm / 0.25 voxel | 1,074,422 | 862,481 | 19.7% | 0.363 → 0.296 s | 0.054 → 0.035 s |

The larger source is now 28.33× smaller than its raw XYZ samples; the smaller
source is 6.04×. Timings vary with machine load. Encoding probes coarser steps
first, reducing unnecessary inverse transforms; this does not relax the error
limit. Distortion preference adds 1,091 / 2,863 bytes versus size-only selection
on these inputs, so it is not described as free quality.

Using the built-in `verify --reference` against every valid source sample:

| Surface | Max XYZ error | Mean XYZ distance | RMS XYZ error |
|---|---:|---:|---:|
| 2.4 µm | 0.924697 | 0.256019 | 0.280687 |
| 7.91 µm | 0.183608 | 0.064594 | 0.070052 |

The previous shared-entropy RMS values were 0.281058 and 0.075177. Thus this pass
reduces size and decode time on both samples, with a small RMS improvement on
the larger surface and about 6.8% on the smaller one. XYZ validity is unchanged.
The metrics command also reports per-channel MAE/PSNR and the explicitly labeled
Gaussian weighted nonoverlapping `ssim_8x8` metric. Those values depend on the
coordinate range; XYZ distance is the primary surface-quality measure here.

Validation: codec/entropy/transform tests, opposite-fast-math fixture exchange,
ASAN/UBSAN, TIFF/reference/4- and 5-channel integration tests, external caller
stress (16 caller-owned threads, 2048 reads), index/descriptor/metadata corruption,
and render3d cache/UI tests plus a headless load of the real v3 file. The source
review follow-up distinguishes completed fixes from remaining speculative
compression experiments; no claim is made that every proposed algorithm shipped.


## V4: compact masks and joint XYZ (2026-09-10)

This pass implements compressed mixed masks, entropy-size-aware quantization,
affine/PCA joint XYZ with final Euclidean verification, reusable encoder scratch,
and one TIFF XYZ/validity extraction per patch. The joint decoder and renderer
read a patch once for all three axes. No internal threads or parallel API.

A is segment **20230702185753**, B is **20230929220926**. Each is tested at
2.4 µm (E=1 voxel) and 7.91 µm (E=0.25 voxel). B adds a different real surface;
these four inputs are two surfaces at two resolutions, not four independent
shapes. B sources are the x/y/z TIFFs and metadata beneath:

- [B, 2.4 µm metadata](https://vesuvius-challenge-open-data.s3.amazonaws.com/PHercParis4/segments/20230929220926/mesh/20230929220926-on-20260411134726-2.4um.tifxyz/meta.json)
- [B, 7.91 µm metadata](https://vesuvius-challenge-open-data.s3.amazonaws.com/PHercParis4/segments/20230929220926/mesh/20230929220926-on-20230205180739-7.91um.tifxyz/meta.json)

Apple M4, Release, fast-math, single synchronous caller; three trials, medians.
Times include CLI startup and local file I/O. Full verify decodes all channels
without reference metrics; the V4 CLI uses the joint API. Raw ratio includes
12 bytes per grid point, including invalid points, divided by complete file size.

| Input | Grid | V3 → V4 bytes | Reduction | Raw ratio | Encode seconds | Full verify seconds |
|---|---|---:|---:|---:|---:|---:|
| A / 2.4 µm | 1820×2530 | 1,950,217 → 1,600,107 | 18.0% | 34.53× | 1.376 → 2.712 | 0.141 → 0.178 |
| A / 7.91 µm | 559×776 | 862,481 → 755,516 | 12.4% | 6.89× | 0.297 → 0.627 | 0.035 → 0.040 |
| B / 2.4 µm | 3790×1604 | 2,667,670 → 2,254,240 | 15.5% | 32.36× | 2.039 → 3.674 | 0.183 → 0.240 |
| B / 7.91 µm | 1164×492 | 1,236,210 → 1,018,873 | 17.6% | 6.74× | 1.453 → 1.188 | 0.045 → 0.052 |

These are ratio improvements with a CPU tradeoff. Encoding is slower on three
inputs and faster on B at 7.91 µm, where one-pass XYZ extraction avoids repeated
compressed-TIFF work. Full decode is 15–31% slower. An initial joint version
encoded A in 3.65 / 0.84 seconds; an intermediate measurement after making the
independent fallback cheaper was 2.77 / 0.62 seconds for only 135 / 314 extra
bytes. The table above includes the final numeric-margin and TIFF-order changes.
The mask/entropy-only intermediate was 1,746,152 / 777,509 bytes for A, with
approximately unchanged decode speed; joint prediction provides the remaining
reduction. These gains are measured together, not multiplied from older probes.

Full source comparisons (all valid samples):

| Input | Maximum XYZ error | Mean XYZ distance | RMS XYZ error |
|---|---:|---:|---:|
| A / 2.4 µm | 0.988486 | 0.353601 | 0.392544 |
| A / 7.91 µm | 0.247093 | 0.094132 | 0.104611 |
| B / 2.4 µm | 0.988492 | 0.358187 | 0.397182 |
| B / 7.91 µm | 0.246580 | 0.099846 | 0.110283 |

Both strict and fast-math decoders pass the same budgets and validity checks.
Joint coding uses more of the permitted Euclidean budget: A RMS increases from
0.280687 / 0.070052 in V3 to the values above. This is not a quality improvement
at equal error; it is a smaller representation within the same maximum-error
contract. Lower-RMS selection remains active for near-equal candidate sizes.

Validation adds folds, sparse/degenerate masks, partial edges, 100k/1M/4M
coordinates, malformed mask/XYZ packets with transactional-output checks,
strict/fast-math XYZ fixture exchange, and 512 joint reads from 16 external
caller-owned threads alongside the existing 2048 scalar reads. Renderer cache
coverage exercises both scalar and joint files; a real V4 surface also passes
the headless renderer smoke test. ASAN/UBSAN and reference TIFF export remain
part of the checks. Source-code snapshots are synchronized in render3d.
