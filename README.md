# surface-compressor

Independent 64×64 DCT blocks for large 2D images and parametric XYZ surfaces.
The core C library has no TIFF dependency and reads only the requested blocks
and their small shared entropy tables.
Version 1.0.0 uses container version 4. The format and C API form the 1.x
compatibility baseline; [release validation](RELEASE.md) records the checks.
Joint XYZ is the default for tifxyz.

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
build/release/surface-compressor encode surface.tifxyz surface.sfc --error 0.1
build/release/surface-compressor decode surface.sfc restored.tifxyz
build/release/surface-compressor verify surface.sfc
```

The tools require libtiff. Set `SFC_TOOLS=OFF` for a standalone codec library.
`SFC_FAST_MATH=ON` is the default. `SFC_SANITIZE=ON` enables ASan/UBSan.
For converter integration tests, install numpy, tifffile and imagecodecs, then
run `python tests/test_cli.py build/release/surface-compressor`.

## Data and error contract

* uint8, uint16 and float32; scalar and multichannel images.
* Float32 forward DCT, quantization preparation and inverse accumulation for
  new blocks. Block bases and verification stay float64; legacy blocks retain
  float64 inverse accumulation. There is no integer DCT.
* Integer arithmetic is used for file offsets, packed quantized coefficients,
  entropy streams, exact auxiliary data, and integer image input/output.
* Different compilers, fast-math settings and CPUs may produce different bytes
  and reconstructed floating-point values. Correctness means staying inside
  the error budget, not reproducing bits.
* `--error` is maximum Euclidean XYZ displacement in the original coordinate
  units (full-resolution voxels for tifxyz). Default for tifxyz: 0.1. Joint XYZ packets
  verify the Euclidean error after prediction and rotation; independent fallback
  axes use error/sqrt(3). Generic images require an explicit per-channel
  absolute error in their native sample units.
* Scalar container blocks and joint residuals try three quantization steps.
  Selection measures actual entropy-coded bytes, including masks, escapes and
  table cost. Within 2% of minimum block size it prefers lower squared error.
  Standalone scalar blocks and the independent XYZ fallback prune candidates
  using their self-contained size. Raw/RLE remains an option.
* Joint XYZ tries independent axes and affine prediction plus a stored float32
  PCA rotation. The smaller complete patch packet wins. Outliers receive exact
  XYZ escapes after checking the final Euclidean error and numerical margin.
  Mixed validity masks use alternating runs when smaller than a bitmap.
* Encoder reconstruction checks add exact sample escapes when necessary;
  incompressible or numerically unsuitable blocks fall back to raw/RLE.
  Numerical margin includes inverse accumulation and float rounding boundaries.
* Tifxyz validity combines finite XYZ, z > 0 and mask channel zero >= 255.
  Integer higher-resolution masks invalidate a point if any mapped pixel fails.
  Invalid coordinates reconstruct as (-1,-1,-1).
* All auxiliary tifxyz channels, including masks and generations, are exact.
  Their original dimensions, channel count and sample type survive export.
  Metadata bytes, including unknown JSON keys, are preserved verbatim.
* TIFF import supports grayscale and RGB pixel layouts. Palette and subsampled
  YCbCr inputs must first be converted to ordinary sample planes. Generic RGB
  and grayscale photometric interpretation is retained. TIFF color profiles,
  alpha-association tags and original strip/compression layout are not archived.

## Partial access and memory

`surfcomp.h` exposes scalar block/region reads and `sfc_read_xyz`, which returns
all three coordinate components with one patch decode. XYZ samples occupy
48 KiB per tile, plus 4 KiB of validity and entry bookkeeping in `sfc_cache`. `sfc_encode_xyz_block`/`sfc_decode_xyz_block` also support standalone
self-contained packets. Use `sfc_write_xyz_block` with a grouped descriptor
triple to append each spatial patch once, then write auxiliary channels.

Scalar streams can share tables across up to 16 blocks. Joint packets share
inline tables across their three residual planes. Neither needs neighboring
patches. A cold scalar read fetches one index entry, its payload and at most
680 bytes of shared tables; a joint read validates three index entries and
fetches one payload. Region reads batch up to 128 adjacent entries. The complete
index is never loaded into memory.

Readers retain at most 16 borrowed immutable table sets and eight reusable
packet/decode workspaces, under 4 MiB total excluding caller-owned decoded
geometry. Excess simultaneous calls use temporary workspaces. Writers reuse
candidate/entropy scratch and stay under 7 MiB of heap storage, excluding
per-channel descriptors and index offsets. These bounds do not grow with image
dimensions. Transform locals also use bounded caller-stack space.
A read-at callback supports other backing stores, including range requests.
The callback must supply exactly the requested bytes; synchronization belongs
in the callback. The local file implementation uses thread-safe `pread`.

The converter streams TIFF strips/tiles through a shared 64 MiB scratch cache.
An individual decoded TIFF strip/tile larger than 64 MiB is rejected; retile
such an input before conversion. XYZ extraction and validity run once per patch;
export decodes each patch once for the three separate TIFFs.
Transactional `sfc_read_region` allocates scratch proportional to the requested
region. Cache region reads stream into caller output without region-sized scratch.

File creation in the C API refuses to overwrite an existing path. Cancel removes
only a newly created output. The CLI encodes to a temporary sibling file and
publishes it on success; export similarly publishes a completed directory and
refuses an existing destination. Failed block and region reads leave caller
outputs unchanged, except streaming cache region reads, which may have copied
earlier tiles before a failure. Payloads carry CRC32 checksums, which detect corruption;
they are not authentication. Shared tables have their own CRC32 checks.

New files use container version 4; versions 1 through 3 remain readable.
Older readers reject new files. V4 adds compact validity masks and self-contained
joint XYZ packets, retaining V3 checksums and float32 inverse arithmetic.
Standalone scalar packets are B641 modes 0/1/2; scalar mode 3 requires its
container table context. Standalone J641 packets include their own tables.

See [the container specification](spec/format.md) for byte layouts.

For an explicit 1.0-voxel XYZ limit (for example, 2.4 µm CT data), use
`--error 1.0`. For a 0.25-voxel limit (for example, 8–9 µm CT data), use
`--error 0.25`. These are Euclidean XYZ limits, not independent per-axis limits.
Resolution is not inferred from tifxyz grid scale; the default remains 0.1.
See [measured error/compression tradeoffs](BENCHMARKS.md#adaptive-quantization-and-error-budget-sweep).


All codec operations are synchronous: no pthread dependency, worker pool,
thread-count setting or parallel API. Callers own scheduling. Stateless block
calls and independent writers may run concurrently; a shared reader is safe
with a concurrent read_at callback. Serialize calls on each writer and never
close an object while another call uses it. `sfc_finish` always consumes its
writer, including on failure; do not subsequently cancel that pointer.

For source-based quality checks:

```sh
surface-compressor verify surface.sfc --reference original.tifxyz
surface-compressor verify image.sfc --reference original.tif
```

This streams source TIFFs and reports maximum error, MAE, RMS, PSNR and Gaussian
weighted nonoverlapping 8×8-window SSIM per channel, plus Euclidean XYZ errors.
The SSIM variant is labeled `ssim_8x8`; float-channel PSNR/SSIM use the finite
source extent as data range, while integer channels use their nominal range.
Nonfinite exact samples and XYZ validity are checked separately. Reference
comparison fails on error-budget or exactness violations. Decode-only timing
excludes reference comparison and SSIM work.

`SFC_NATIVE` and `SFC_IPO` are opt-in build optimizations. CTest automatically
exchanges fixtures with the opposite fast-math policy. Python CLI tests are
registered when numpy/tifffile/imagecodecs are available; a Python caller test
owns its own threads to exercise concurrent C reads. The codec itself remains
synchronous in every build.

## Installation and linking

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build/release -j
cmake --install build/release
```

Use `-DBUILD_SHARED_LIBS=ON` for a shared library, or `-DSFC_TOOLS=OFF`
for a library-only installation without libtiff. Downstream CMake projects use
`find_package(surfcomp 1 CONFIG REQUIRED)` and link `surfcomp::surfcomp`.
The `surfcomp.pc` file supports pkg-config users.
[Installed consumer examples](examples/installed) exercise C and C++ linkage.
`surface-compressor --version`, `sfc_version()` and `SFC_VERSION_STRING` identify
the library release; `sfc_format_version()` identifies the current writer format.

For interactive access to very large surfaces, wrap a reader in `sfc_cache`.
The cache lazily decodes 64×64 tiles and evicts the least recently used tiles
under a caller-selected byte budget. It never loads the complete surface or
index. Scalar channels, multichannel image planes and joint XYZ are supported;
reading X, Y and Z individually from a joint packet reuses the same tile.

```c
sfc_cache *cache = NULL;
int rc = sfc_cache_create(reader, 64u * 1024u * 1024u, &cache);
if (rc == SFC_OK) {
    float xyz[32 * 32 * 3];
    uint8_t valid[32 * 32];
    rc = sfc_cache_read_xyz_region(cache, first_xyz_channel,
                                   x, y, 32, 32, xyz, 12, 32 * 12, valid);
    /* Keep cache alive across requests; destroy before closing reader. */
    sfc_cache_destroy(cache);
}
```

Use `sfc_cache_read_region` for a scalar/image channel, and
`sfc_cache_get_stats` for hits, misses, evictions and resident bytes.
The byte budget includes tile bookkeeping, decoded samples and validity masks;
fixed cache control storage (roughly 2 KiB) and the reader's bounded decode
scratch are additional. Entries are allocated on demand. A joint tile needs
about 52 KiB, and a scalar tile 8–20 KiB depending on sample type.
An insufficient budget returns `SFC_LIMIT`; it does not silently exceed the cap.

Shared-cache calls are safe from caller-owned threads, with each tile lookup,
decode and copy serialized by C11 atomics. For concurrent decoding, use separate
caches over the same reader. There are no internal workers or thread controls.
Callbacks must not reenter the same cache. Destroy requires all calls to have
finished, and the reader and its immutable backing storage must outlive the cache.
Region reads copy into caller-owned output and allocate no region-sized scratch;
an I/O/decode failure may leave previously copied tiles in that output.

Fast builds use `-ffast-math -fno-finite-math-only`: reassociation, reciprocal
optimizations and fused operations remain enabled, while NaN/Inf classification
remains valid. This exception is required because scalar images preserve
nonfinite sample bits and every decoder validates untrusted numeric fields.
Compile `src/surfcomp.c` with `-fno-finite-math-only` **after** any parent-project
fast-math flags; CMake already does so. The source rejects finite-only builds
instead of silently losing validation. Pure `-ffast-math` is not a supported
codec build policy. This does not impose bit-exact reconstruction.
