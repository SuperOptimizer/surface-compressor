# 1.0 release gates

Joint XYZ remains the tifxyz default, as explicitly selected by the user. Its
CPU/distortion tradeoff is recorded in BENCHMARKS.md. The codec stays synchronous;
callers own scheduling. No internal threads, thread controls or parallel API.

The release sources identify **1.0.0**, using **container version 4**.
Publication remains pending the final artifact gate below.

## Compatibility policy for 1.x

- Version-4 files written by 1.x must remain readable by conforming 1.x decoders.
  Encoder output bytes and reconstructed float bits may change, but the format,
  numerical allowances, validity rules and requested error limits may not.
- Continue reading experimental container versions 1–3; permanent fixtures cover
  legacy exact storage, legacy float64 inverse/shared entropy, float32 inverse,
  compact masks and joint XYZ. Never regenerate fixtures merely to make a
  changed decoder pass. Add new fixtures when extending test coverage.
- Preserve existing C function signatures, enum values, struct layout, ownership
  and synchronous execution rules throughout 1.x. Changes requiring a format or
  ABI break require a major release. The public struct is not extensible by
  silently adding fields. Library version and container version are separate.
- Primary supported release targets: macOS ARM64 and Linux x86-64, C11, Clang
  and GCC as tested by CI. C++ consumers use the C ABI. Windows and 32-bit
  targets are not certified for 1.0.
- `-ffast-math` is supported only while the specified numerical error allowances
  hold. It is not permission to use arbitrary approximate decoder arithmetic.

## Gates and evidence

- [x] User confirmed joint XYZ remains the default.
- [x] Versioned library/header/CLI identity and CMake installation/export targets.
- [x] Separate installed C and C++ consumer tests pass locally for static library.
- [x] Static/shared installed packages pass after moving the install prefix (CMake and pkg-config).
- [x] Permanent v1–v4 fixtures pass on the full compiler/architecture matrix.
- [x] CI exchanges newly encoded containers across machines and math policies.
- [ ] Sustained container/block fuzzing and allocation/I/O failure injection pass.
- [x] Seven real inputs from five surfaces pass source-reference checks; generic image coverage expanded.
- [x] Candidate source archive builds/tests from a clean extraction (26 tests, including cache and nonfinite cases).
- [x] Final numerical/API/format review has no unresolved confirmed correctness findings.
- [ ] Candidate committed, remote CI green, renderer pinned to tested revision.
- [ ] Final 1.0 version/changelog/tag/release artifacts published after gates pass.

Existing evidence: 14 local codec tests, 13 ASAN/UBSAN tests, external shared-reader
stress, four real inputs from two surfaces under strict/fast math, and renderer
cache/UI/stream checks. These are useful evidence, not substitutes for the
unrun release gates above. Record concrete commands and CI run URLs as gates
are completed. Do not label a missing remote or platform run as a pass.

## Reproducing the local gates

```sh
python tests/test_install.py
python tests/test_archive.py
python tools/validate_real.py --cache /path/to/cache --output build/real-check --exe build/dev/surface-compressor
cmake -S . -B build/fuzz -DSFC_TOOLS=OFF -DSFC_TESTS=OFF -DSFC_FUZZ=ON -DCMAKE_C_COMPILER=clang
cmake --build build/fuzz -j
python tests/seed_fuzz.py build/corpus
build/fuzz/fuzz_decode build/corpus -max_total_time=600 -max_len=1048576 -timeout=10 -rss_limit_mb=512
```

Use a Clang installation that includes libFuzzer. The harness exercises raw,
joint and container paths, both with original and repaired outer checksums.
Local smoke evidence: 592,748 inputs in 121 seconds without sanitizer findings;
a subsequent 601-second run completed 2,803,646 inputs without findings; the final fixed binary is being fuzzed again. The allocation/I/O gate exercises 111 failure points, including cache allocation/read failures
plus deliberately short `pread` results, and verifies transactional output and
cleanup. Clang static analysis completed without diagnostics. Generic TIFF
checks also reject unknown/missing CLI options and cover nonfinite F32 samples.

The real corpus is pinned by source URLs, byte lengths and SHA256 in
`tests/corpus/real.json`. Downloads are optional when the cache already matches.
These are validation runs, not isolated performance comparisons. Use
`tools/source_archive.py` to normalize CPack output before publishing; raw
CPack archives on macOS can include AppleDouble metadata sidecars.

Additional pre-1.0 API: `sfc_cache` provides bounded LRU tile retention and
streaming region reads for huge surfaces. Its output-on-error semantics differ
from transactional `sfc_read_region`; see the public header. The LLVM fast-math
first-sample NaN regression is fixed by disabling finite-only assumptions while
retaining other fast-math optimizations. All 26 local tests and 24 sanitizer
tests pass, including strict-oracle nonfinite tests. Renderer cache/UI/stream
tests and a headless real-file smoke pass with the updated codec snapshot.

Candidate commit `e20b451f4638d0abac46b2baea2081b7fa48ee3e` passed all
13 remote jobs: six compiler/math builds, six cross-platform exchange consumers
and Linux sanitizer/fuzz checks. Each exchange consumer decoded all 18 files
produced by the six builds. See [candidate CI run](https://github.com/SuperOptimizer/surface-compressor/actions/runs/34559212066).
The final version-only promotion and CLI build-policy guard are being checked
in a fresh run before publishing.
