# Changelog

## 1.0.0 — 2026-09-10

First stable library release, using container version 4.

- Independent 64×64 floating-point DCT blocks for U8, U16 and F32 single- and
  multichannel images. No integer transform or bit-exact reconstruction requirement.
- Joint float32 XYZ coding is the tifxyz default, with bounded Euclidean error,
  affine/PCA prediction, compact masks, entropy-aware quantization and exact escapes.
- Reads experimental container versions 1–3 in addition to version 4.
- Synchronous C API with concurrent shared-reader support and caller-owned scheduling.
  No internal threads or thread controls.
- Byte-bounded LRU decode cache with scalar/XYZ region reads and cache statistics.
- Bounded partial reads, preserved metadata and exact auxiliary TIFF channels.
- Static/shared CMake and pkg-config packages; C and C++ consumers.
- Nonfinite scalar sample preservation and numeric validation under fast math,
  with finite-only compiler assumptions disabled.
- Permanent compatibility fixtures, cross-platform file exchange, sanitizers,
  fuzzing, fault injection, installed-package and clean-source-archive checks.

Performance and distortion tradeoffs are recorded in BENCHMARKS.md. Joint XYZ
prioritizes smaller files within the chosen maximum-error budget and generally
uses more CPU than independent coordinate planes.
