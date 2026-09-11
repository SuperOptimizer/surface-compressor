# Surface-compressor 1.0.0 validation and compatibility

Library **1.0.0** writes **container version 4**. Joint XYZ is the tifxyz default;
its CPU/distortion tradeoff is recorded in BENCHMARKS.md. All codec operations
are synchronous. Callers own scheduling; the codec creates no threads and has
no thread-count controls or parallel API.

## Compatibility policy for 1.x

- Version-4 files written by 1.x remain readable by conforming 1.x decoders.
  Encoder bytes and reconstructed float bits may change; the format, numerical
  allowances, validity rules and requested error limits may not.
- Continue reading experimental versions 1–3. Permanent fixtures cover legacy
  exact storage, float64 inverse/shared entropy, float32 inverse, compact masks
  and joint XYZ. Never regenerate fixtures merely to make a decoder pass.
- Preserve C function signatures, enum values, public struct layout, ownership
  and synchronous execution rules throughout 1.x. Incompatible format or ABI
  changes require a major release. Library and container versions are separate.
- Supported release targets: macOS ARM64 and Linux x86-64, C11, Clang and GCC
  as exercised by CI. C++ consumers use the C ABI. Windows and 32-bit targets
  are not certified for 1.0.
- Fast builds use `-ffast-math -fno-finite-math-only`, keeping reassociation and
  FMA while preserving NaN/Inf validation. Source rejects finite-only builds.
  The specified numerical allowances still apply; bit-exact reconstruction
  is not required.
- Readers borrow immutable backing bytes; caches borrow readers. Destroy/close
  must not race with use. Writer finish always consumes its writer, including
  on failure. Cache region reads stream and can leave earlier tiles written on
  a later failure; ordinary block/region reads remain transactional.

## Verified release gates

- [x] Joint XYZ retained as the explicitly selected default.
- [x] Versioned header/library/CLI and relocated static/shared installations,
  including independent C/C++ CMake consumers and pkg-config linking.
- [x] Permanent v1–v4 fixtures on all six compiler/math configurations.
- [x] Each of six cross-platform consumers decodes all 18 newly encoded files
  from macOS ARM64 Clang and Linux x86-64 GCC/Clang, under both math policies.
- [x] All 26 local tests and 24 local ASAN/UBSAN tests pass. Remote CI also runs
  sanitizers and libFuzzer. The fixed codec completed a separate 601-second
  local fuzz run with 3,017,241 inputs and no findings; an earlier candidate
  completed 2,803,646 inputs. Clang static analysis reports no diagnostics.
- [x] 111 allocation/I/O failure cases, including cache failures, plus short
  `pread` handling pass with output/cleanup checks.
- [x] Seven real inputs from five surfaces pass source-reference validation at
  the requested 1.0/.25-voxel limits; generic image checks include nonfinite
  F32 payloads, grayscale, RGB/RGBA and additional channels.
- [x] Clean extracted 1.0.0 source archive builds and passes all 26 tests.
- [x] Cache tests verify LRU reuse/eviction, byte caps, strided edges, masks,
  caller-owned concurrency and a virtual 100-million-square image. Far-corner
  access requires fewer than 9 KiB of reads with a 16 KiB tile-cache budget.
- [x] Final API/format/numerical review has no unresolved confirmed defects.
  Remaining compression experiments are documented separately, not blockers.
- [x] Renderer snapshot matches the tested upstream revision byte-for-byte.
  macOS cache/UI/stream tests and a real-file headless smoke pass. Renderer
  integration is committed on its existing `master` branch.

The candidate at `e20b451f4638d0abac46b2baea2081b7fa48ee3e` passed all 13
[remote jobs](https://github.com/SuperOptimizer/surface-compressor/actions/runs/34559212066).
The final 1.0.0 implementation at `aba1682e61be20e4e547e5aeb6c185326faf486f`
also passed all 13 [release jobs](https://github.com/SuperOptimizer/surface-compressor/actions/runs/34559469290).
Subsequent release-record edits do not change the tested implementation.
Renderer integration `4e5194f078ff0208b53952054e2972c44aaa194b` pins that final
implementation; per-file hashes are in its `tools/surface-compressor/snapshot.json`.

## Reproducing the checks

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
python tests/test_install.py
python tests/test_archive.py
python tools/validate_real.py --cache /path/to/cache --output build/real-check --exe build/release/surface-compressor
cmake -S . -B build/fuzz -DSFC_TOOLS=OFF -DSFC_TESTS=OFF -DSFC_FUZZ=ON -DCMAKE_C_COMPILER=clang
cmake --build build/fuzz -j
python tests/seed_fuzz.py build/corpus
build/fuzz/fuzz_decode build/corpus -max_total_time=600 -max_len=1048576 -timeout=10 -rss_limit_mb=512
```

Use Python with numpy/tifffile/imagecodecs for all optional tests, and Clang
with libFuzzer for fuzzing. The harness exercises raw, joint and container
paths, with original and repaired outer checksums. The real corpus is pinned
by URLs, sizes and SHA256 in `tests/corpus/real.json`; existing matching cached
inputs need no download. These source checks are not isolated timing runs.

## Distribution

The [1.0.0 release](https://github.com/SuperOptimizer/surface-compressor/releases/tag/v1.0.0)
is the publication record for the tagged source archive and SHA256SUMS.
Use `tools/source_archive.py` to normalize CPack output before distribution;
raw CPack archives on macOS can include AppleDouble metadata sidecars.
