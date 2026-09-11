# Changelog

## 1.0.0-rc.1 — unreleased

First release candidate; library version 1.0 and container version 4 are separate.
The release is pending cross-platform compatibility, fuzzing and release gates.

- Independent 64×64 blocks for U8, U16 and F32 single/multichannel images.
- Joint float32 XYZ coding is the default for tifxyz, with bounded Euclidean error.
- Affine/PCA prediction, compact validity masks, entropy-aware quantization,
  and exact escapes; readers also support experimental container versions 1–3.
- Synchronous C API, caller-owned scheduling, concurrent shared-reader support.
- Bounded partial reads, metadata preservation and exact auxiliary TIFF channels.
- CMake and pkg-config installation, static/shared libraries and CLI version reporting.

Performance and distortion tradeoffs are recorded in BENCHMARKS.md. The default
joint codec prioritizes smaller files within the selected maximum-error budget;
it generally uses more CPU than independent coordinate planes.

- Added a synchronous, byte-bounded LRU decode cache with scalar and XYZ region
  access, shared joint-channel tiles, cache statistics and caller-owned output.
  Shared-cache access is synchronized without internal worker threads. Tests
  cover eviction, failed reads, strided edges and caller-driven concurrency.
- Fixed LLVM fast-math removal of NaN/Inf checks by disabling finite-only
  assumptions in codec builds. Added strict-oracle nonfinite regression tests
  and a compile-time guard against unsafe downstream flags.
