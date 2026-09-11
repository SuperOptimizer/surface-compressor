# Permanent compatibility fixtures

These files are decoder inputs, not regenerated test outputs. Their SHA256 and
format versions are recorded in manifest.json. Expected samples and validity
are specified independently by tests/test_compat.c. Add fixtures; do not rewrite
these to accommodate decoder regressions. Reconstruction comparisons use error
bounds; only exact fields and fixture identities require byte equality.

- v1-exact: exact U16 raw payload produced by the RC writer, then the header and
  index rewritten to the v1 layout and checksums. No v4-only payload features.
- v3-scalar: produced by the pre-v4 source snapshot preserved during development;
  includes varint and shared-entropy blocks with float32 inverse arithmetic.
- v2-scalar: v3-scalar rewritten to v2 header/index checksums and arithmetic 0,
  exercising the legacy float64 inverse with the same quantized coefficients.
  It is a constructed conformance fixture, not a historical upstream release.
- v4-scalar: RC compact masks and entropy selection, including partial edges.
- v4-xyz: RC joint XYZ packets, grouped indices, predictor/rotation and partial
  edge/invalid-pixel handling.

The CI exchange job separately generates new containers with every tested
compiler/math policy and decodes all 18 on each consumer platform/policy.
Those transient files do not replace these permanent fixtures.
