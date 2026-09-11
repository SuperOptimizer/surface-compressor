# Experimental SFC format (container versions 1 through 4)

All integers and IEEE754 values are little endian. Dimensions and file offsets
are uint64. Files are limited to INT64_MAX bytes, 4096 channels, and 64 MiB of
opaque metadata. Every channel has ceil(width/64) × ceil(height/64) blocks.
Blocks have independent coefficient streams; version 2 may reference shared
entropy tables. Final blocks are padded to 64×64 and carry validity.
Reserved bytes must be zero. Byte offsets below are relative to each structure.

## Header (64 bytes)

| Offset | Type | Meaning |
|---|---|---|
| 0 | char[4] | SFC1 |
| 4 | u32 | Version 1 through 4; new writers emit 4 |
| 8 | u32 | Channel count |
| 12 | u32 | Reserved |
| 16 | u64 | Metadata offset, 64 + 128 × channel count |
| 24 | u64 | Metadata size |
| 32 | u64 | Payload area start |
| 40 | u64 | Exact file size |
| 48 | u32 | IEEE CRC32 of bytes 0–47; v3+ additionally includes bytes 52–63 |
| 52 | u32 | v3+: CRC32 of descriptors followed by metadata; v1/v2: zero |
| 56 | byte[8] | Reserved |

Channel descriptors follow the header, then metadata, then each channel's index
in descriptor order. There are no gaps in those sections. Payloads follow.

## Channel descriptor (128 bytes)

| Offset | Type | Meaning |
|---|---|---|
| 0 | char[64] | Unique, nonempty, NUL-terminated channel name |
| 64,72 | u64 | Width, height |
| 80 | u32 | Type: 1=u8, 2=u16, 3=float32 |
| 84 | u32 | Flags: 1=exact, 2=coordinate, 4=joint XYZ (v4) |
| 88 | float64 | Absolute component error; joint XYZ uses Euclidean error; exact permits zero |
| 96 | u64 | Index offset |
| 104 | u64 | Number of blocks |
| 112 | u32 | Image component count; 0 or 1 means scalar |
| 116 | u32 | Component index |
| 120 | byte[8] | Reserved |

A multichannel image occupies contiguous descriptors with matching dimensions,
type and component count, numbered from zero. The TIFF converter names them
`stem.0`, `stem.1`, etc. Explicit grouping distinguishes these from scalar
channels whose names happen to contain numeric suffixes.

## Index entry (32 bytes)

Entries are in block-row order. Offset 0 is payload offset (u64), 8 is payload
size (u32, at most 40000 for B641 or 200000 for J641), 12 is payload CRC32. Offsets 16 and 20 are float32
min/max over finite valid source samples, expanded by component tolerance and
outward-rounded. Offset 24 is a u32 empty marker. In v3+, offset 28 is CRC32
of LE channel index (u32), LE linear block index (u64), then entry bytes 0–27.
In v1/v2, offset 28 is reserved and zero. This binds offsets, bounds and payload
checksums to the intended block, so swapping otherwise-valid entries is detected.
An empty range is not usable as a geometry bound. Readers validate payload
ranges before reading them. Metadata and descriptors are structurally checked; v3+ also verifies their
combined checksum while opening, streaming metadata in bounded pieces.

## Block header (32 bytes)

| Offset | Type | Meaning |
|---|---|---|
| 0 | char[4] | B641 |
| 4 | u8 | Sample type |
| 5 | u8 | Mode: 0=raw, 1=DCT varints, 2=RLE, 3=DCT shared entropy (v2+) |
| 6 | u8 | Flags: 1=mixed validity, 2=all invalid, 4=coordinate, 8=run-coded mask (v4) |
| 7 | u8 | Reserved |
| 8 | float64 | Local base for DCT |
| 16 | float32 | Positive coefficient quantization step |
| 20 | u32 | Inverse arithmetic: 0=legacy float64 accumulation; 1=float32 (v3+) |
| 24 | u32 | DCT coefficient stream length |
| 28 | u32 | Exact exception count |

Mixed-validity blocks append a 512-byte bitset, least significant bit first,
row-major. All-valid and all-invalid blocks omit it. Invalid coordinates are -1;
other invalid values are zero. All-invalid blocks have no sample payload.
With flag 8 (requires flag 1), replace the bitmap with a u16 byte length followed
by one initial bit byte (0 or 1) and alternating positive base-128 varint runs.
The length includes the initial bit and is 3..509 bytes. Runs must total exactly
4096, consume the declared bytes, and never be zero. The encoder retains the
bitmap unless the length prefix plus runs is smaller than 512 bytes.

Raw payloads contain 4096 native-width little-endian sample bit patterns.
RLE uses unsigned base-128 varint run lengths followed by one sample bit pattern.

Mode 1 DCT coefficients are row-major. Repeatedly read an unsigned varint zero-run,
then (unless all 4096 positions are covered) a zigzag signed varint coefficient.
Multiply quantized coefficients by the float32 step, apply the separable
orthonormal inverse DCT-II using the float32 basis in `src/dct64.h`, and add the
float64 local base. The 4096 basis entries are fixed by this format: their
row-major little-endian float32 bytes have IEEE CRC32 `0x0da929d3`. This identifies
the normative constants; transform operations and reconstructed results need
not be bit identical. All modes produce
float32 reconstructed residuals; block offset 20 selects the inverse contract.
Let L1 be the sum of absolute float32 dequantized coefficients. Relative to the
exact two-pass sum using the stored float32 basis, inverse error before final
float32 residual rounding must be at most:

- Arithmetic 0: 1e-13 × L1 (legacy float64 accumulation).
- Arithmetic 1: 4e-7 × L1 + 1e-34 (float32 accumulation, container v3+).

The encoder reserves twice the corresponding accumulation allowance for
encoder/decoder differences, plus residual and final-output rounding. It checks
against the requested tolerance without an additional 0.95 multiplier. There
is no integer DCT and fast-math (with finite-only assumptions disabled) is allowed. Raw/RLE must keep offset 20 zero.

For the float32 symmetric kernel, even/odd basis symmetry halves the products.
A coefficient-to-output path has no more than 128 rounding steps over two
passes, and each pair of basis factors has magnitude at most 1/32. With unit
roundoff u=2^-24, gamma(128)/32 is below 2.4e-7; the 4e-7 bound leaves room for
reassociation and the stated tiny absolute term covers underflow. Kernels must
still reject nonfinite reconstruction. Tests compare sparse/dense inputs over
multiple magnitudes against a widened reference; legacy files retain their
original arithmetic contract.

Exceptions follow the coefficient stream: unsigned varint delta from previous
sample index (initially zero), then exact native-width sample bits. Entries
must be ordered, without duplicates; only the first delta may be zero.
Nonfinite generic float samples are preserved by raw/RLE or exact exceptions.


## Shared entropy DCT (mode 3, container version 2)

The local base, float32 step, validity and exceptions are identical to mode 1.
Only the coefficient stream changes. Its length at header offset 24 includes
this 24-byte descriptor, followed by an entropy stream and bypass stream:

| Offset | Type | Meaning |
|---|---|---|
| 0 | u64 | Absolute shared-table offset |
| 8 | u32 | Shared-table byte count, 60 through 680 |
| 12 | u32 | CRC32 of shared-table bytes |
| 16 | u32 | Entropy stream byte count, 3 through 11000 |
| 20 | u32 | Bypass stream byte count, at most 20000 |

The stream counts plus 24 must equal the coefficient stream length exactly.
Exceptions immediately follow it. Tables must lie entirely in the payload area
and precede the referring block. Block payload size remains at most 40000 bytes.
The regular index CRC covers the entire block, including this descriptor.
Tables are shared auxiliary payloads, not entries in the block index.

There are ten context models, each with 32 symbol frequencies summing to 1024.
Each model stores a little-endian uint32 presence bitmap followed by positive
frequencies for set bits in ascending symbol order. Frequencies use canonical
unsigned LEB128, each at most 1024. Empty bitmaps, zero frequencies, incorrect
sums and trailing bytes are invalid. Readers adopt frequencies verbatim.

Coefficient positions use the diagonal scan in `src/scan64.h`: increasing x+y,
then increasing y within each diagonal. DC (position zero) is encoded directly,
with no prediction from another block. Remaining coefficients use run/level
coding, followed by an explicit end-of-block symbol, including when the last
coefficient is nonzero.

1. Encode zigzag(DC) using context 9.
2. Set previous nonzero scan position to zero.
3. For each subsequent nonzero coefficient at position k, encode
   run = k - previous - 1, then magnitude - 1, then a bypass sign bit (1 negative).
4. Set previous = k and repeat; finish with run symbol 31 (EOB).

Run context is band(previous + 1). Level context is
3 + 2*band(k) + (run == 0). Band(p) is 0 for p < 128, 1 for p < 1024, else 2.
Unsigned values 0..3 are literal tokens. Larger values have token
2 + floor(log2(value)), with the remaining low floor(log2(value)) bits placed
in the bypass stream. DC tokens must be at most 25 and zigzag values at most
8388608. Non-EOB run tokens must be at most 13 and must land within the block.
Level tokens must be at most 23, with magnitude - 1 below 4194304.
All dequantization and floating-point reconstruction use the mode 1 contract.

Entropy uses two-lane, 10-bit tANS as defined in `src/entropy.h`, adapted from
volume-compressor. Spread symbols in increasing symbol order over 1024 slots
using step 643 modulo 1024. For each slot, take nx from a per-symbol counter
initialized to that symbol's frequency, then increment it; the slot decodes
that symbol and reads nb = 10 - floor(log2(nx)) bits, with next-state base
(nx << nb) - 1024. Tokens alternate lanes, starting at lane zero. The stream
begins with two 10-bit decoder states. Each token advances its lane to the
slot's base plus the bits read. Both entropy and bypass streams are forward,
LSB-first, byte-padded with zeros. At EOB both states must be zero, all bytes
must be consumed and fewer than eight zero padding bits may remain per stream.

Each block restarts both entropy states and bypass bits. Table sharing creates
no preceding-block dependency. The writer currently considers groups of up to
16 consecutive blocks per channel, but group size is an encoder choice and is
not needed by readers. It uses sharing only if selected smaller block packets
save more bytes than the shared tables cost. Standalone B641 encode/decode APIs
retain modes 0/1/2; use the container reader for mode 3.


## Version policy

Readers reject unsupported container versions at open. New writers use v4;
v1/v2/v3 files remain readable with their original CRC and inverse-arithmetic
rules. The payload CRC covers the complete block, including the 32-byte header,
validity, coefficient descriptor/streams and exceptions. V3 adds protection
for metadata, descriptors and index positions without changing block addressing.
CRC32 detects accidental corruption and is not authentication.

## Execution and memory

All library calls are synchronous. The library starts no threads and has no
thread-control API. Stateless block operations and independent writer instances
can be called concurrently. Concurrent reads on one reader are supported when
its read_at callback is safe for concurrent use. Calls on one writer remain
ordered and serialized by its caller; close/cancel must not race with calls.

Readers borrow reference-counted immutable table entries under a short atomic
lock. The lock is never held during I/O, table construction or decoding. Up to
16 table sets and eight reusable packet/decode buffers are retained (under 4 MiB once
populated); excess simultaneous reads use temporary buffers. Region reads batch
up to 128 adjacent index entries and retain transactional output staging.


## Joint XYZ packet (J641, version 4)

A joint group consists of three contiguous F32 descriptors, flags exactly 6,
components=3, component indices 0,1,2, with equal dimensions and positive
Euclidean tolerances. Each spatial patch has one shared payload. All three
index entries carry the same offset/size/payload CRC, but their own coordinate
ranges and position-bound index CRC. A joint read validates all three entries.
No packet depends on another patch; channel-major scalar access remains valid,
but decodes all three planes. `sfc_read_xyz` avoids that repeated work.

| Offset | Type | Meaning |
|---|---|---|
| 0 | char[4] | J641 |
| 4 | u8 | 0=independent axes, 1=affine predictor plus rotation |
| 5 | byte[3] | Reserved |
| 8 | f64 | Positive Euclidean tolerance, equal to descriptors |
| 16 | byte[4] | Reserved |
| 20 | u32 | Inline entropy table length, 0 or at most 680 |
| 24 | u32 | Exact XYZ exception count, at most 4096 |
| 28 | u32 | CRC32 of inline table bytes (zero when absent) |
| 32 | f32[9] | Per-axis constant, x slope, y slope |
| 68 | f32[9] | Row-major 3×3 reconstruction rotation |
| 104 | byte[8] | Reserved |
| 112,116,120 | u32 | Three B641 child packet lengths, each 32..40000 |
| 124 | byte[4] | Reserved |

Inline tables follow the 128-byte header, then the three child packets, then
exceptions. Tables use the scalar shared-table structure. Mode-3 children use
these inline models: their coefficient descriptor's first 16 bytes are zero,
with the ANS/bypass lengths and data unchanged. Tables must be present iff at
least one child uses them. All child validity masks must agree. Each exception
is a delta-index varint followed by three exact little-endian float32 values.
Indices strictly increase after the first; exceptions must identify valid
samples. The enclosing payload CRC covers all predictor, table, mask, child
and exception bytes.

Mode 0 requires zero predictor/rotation fields and copies child output values.
Mode 1 requires finite parameters and rotation columns orthonormal within 1e-5.
For pixel i, let u=(i mod 64)-31.5 and v=floor(i/64)-31.5. For axis k, evaluate
`p[k,0] + p[k,1]*u + p[k,2]*v + sum_j R[k,j]*child[j,i]` in float64, then round
to float32. Predictor fitting/rotation selection are encoder choices; the
stored float32 parameters are normative. Apply exact XYZ exceptions afterward.
Valid outputs must be finite with positive Z; invalid points are (-1,-1,-1).

The encoder verifies Euclidean distance after reconstruction, with additional
cross-decoder drift allowance. For each transformed child it reserves twice
the inverse accumulation allowance, twice the float32 ULP at coefficient L1,
and twice the largest decoded-output ULP; raw/RLE children have zero drift.
For mode 1 each axis reserves the absolute rotation-weighted child allowances,
two final-output ULPs, and 1e-12 times (one plus the sum of absolute predictor
and rotation terms, using 31.5 as the slope multiplier bound). This last term
covers reassociation even under cancellation. The Euclidean norm of axis
allowances is added to observed Euclidean error. Points exceeding the budget
are exact XYZ exceptions. Arbitrarily relaxed or approximate decoder math is
not permitted: the specified numeric allowances still apply with fast-math.

The encoder compares complete independent and affine/rotated packets, including
tables and XYZ exceptions. Independent fallback uses per-axis E/sqrt(3); joint
residuals use E and are subsequently checked in world XYZ space. Coordinate
magnitude may force exact storage; per-block prediction cannot restore bits
already absent from float32 input.

Implementation build policy: numeric input validation must honor NaN and Inf.
The reference CMake build pairs `-ffast-math` with `-fno-finite-math-only`;
finite-only compiler assumptions can erase validation after FP conversion and
are unsupported. This is an implementation requirement, not a change to the
container's transform or numerical allowances.
