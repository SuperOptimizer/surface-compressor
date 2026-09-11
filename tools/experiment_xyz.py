"""Compare independent XYZ, PCA rotation, affine prediction, and their combination.

Research tool, not a file encoder: uses the actual block codec, reconstructs all
samples, and estimates a joint container's transform/exception overhead. Loads
reference TIFFs into memory. Requires numpy, tifffile and a shared codec library.

Example (macOS):
  clang -O3 -ffast-math -fno-finite-math-only -shared -fPIC -I. src/surfcomp.c -o build/experiment.dylib
  python tools/experiment_xyz.py surface.tifxyz --error 1 \
      --library build/experiment.dylib --json build/joint.json
"""
import argparse
import ctypes as C
import json
from pathlib import Path

import numpy as np
import tifffile


class Channel(C.Structure):
    _fields_ = [
        ("name", C.c_char * 64), ("width", C.c_uint64), ("height", C.c_uint64),
        ("dtype", C.c_int), ("flags", C.c_uint32),
        ("components", C.c_uint32), ("component", C.c_uint32),
        ("tolerance", C.c_double),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--error", type=float, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    if not np.isfinite(args.error) or args.error <= 0:
        parser.error("--error must be finite and positive")
    lib = C.CDLL(str(args.library.resolve()))
    free = C.CDLL(None).free
    free.argtypes = [C.c_void_p]
    lib.sfc_encode_block.argtypes = [
        C.POINTER(Channel), C.c_void_p, C.c_size_t, C.c_size_t, C.c_void_p,
        C.POINTER(C.c_void_p), C.POINTER(C.c_size_t),
    ]
    lib.sfc_decode_block.argtypes = [
        C.c_void_p, C.c_size_t, C.c_int, C.c_void_p,
        C.c_size_t, C.c_size_t, C.c_void_p,
    ]
    data = np.stack([tifffile.imread(args.input / f"{c}.tif") for c in "xyz"], -1)
    if data.ndim != 3 or data.dtype != np.float32:
        parser.error("expected three matching scalar float32 TIFFs")
    h, w, _ = data.shape
    validity = np.isfinite(data).all(-1) & (data[:, :, 2] > 0)
    mask_path = args.input / "mask.tif"
    if mask_path.exists():
        with tifffile.TiffFile(mask_path) as t:
            mask = t.asarray()
            if mask.ndim == 3:
                mask = mask[0] if t.pages[0].planarconfig == 2 else mask[:, :, 0]
        mh, mw = mask.shape
        if mh >= h and mw >= w and mh % h == 0 and mw % w == 0:
            validity &= (mask >= 255).reshape(h, mh // h, w, mw // w).all((1, 3))
    y, x = np.mgrid[:64, :64]
    design = np.stack([np.ones_like(x), x - 31.5, y - 31.5], -1)
    design = design.reshape(-1, 3).astype("float64")
    methods = ["independent", "pca", "affine", "affine_pca"]
    totals = {m: dict(bytes=0, max=0., sq=0., corrections=0) for m in methods}
    best = dict(bytes=0, max=0., sq=0., choices={m: 0 for m in methods})
    count = blocks = 0
    channel = Channel(b"residual", 64, 64, 3, 2, 0, 0, args.error / np.sqrt(3))
    for by in range(0, h, 64):
        for bx in range(0, w, 64):
            hh, ww = min(64, h - by), min(64, w - bx)
            original = np.zeros((64, 64, 3), dtype="float64")
            original[:hh, :ww] = data[by:by + hh, bx:bx + ww]
            original = original.reshape(-1, 3)
            mask = np.zeros((64, 64), dtype="uint8")
            mask[:hh, :ww] = validity[by:by + hh, bx:bx + ww]
            mask = mask.ravel()
            valid = mask.astype(bool)
            nv = int(mask.sum())
            count += nv
            outcomes = {}
            for method in methods:
                predictor = np.zeros((4096, 3))
                basis = np.eye(3)
                overhead = 0
                if nv and method in ("affine", "affine_pca"):
                    coefficients = np.linalg.lstsq(design[valid], original[valid], rcond=None)[0]
                    # Round parameters to the representation counted below BEFORE
                    # predicting, so the decoder can recreate the same predictor.
                    coefficients = coefficients.astype("float32").astype("float64")
                    predictor = design @ coefficients
                    overhead += 36
                elif nv and method == "pca":
                    predictor[:] = original[valid].mean(0)
                    overhead += 24  # Three float64 means.
                residual = original - predictor
                if nv and method in ("pca", "affine_pca"):
                    v = residual[valid]
                    _, basis = np.linalg.eigh(v.T @ v)
                    basis = basis.astype("float32").astype("float64")
                    overhead += 36
                transformed = np.ascontiguousarray((residual @ basis).astype("float32"))
                decoded = np.empty_like(transformed)
                size = overhead
                for axis in range(3):
                    encoded, n = C.c_void_p(), C.c_size_t()
                    rc = lib.sfc_encode_block(
                        C.byref(channel), transformed.ctypes.data + axis * 4, 12, 768,
                        mask.ctypes.data, C.byref(encoded), C.byref(n))
                    if rc:
                        raise RuntimeError(f"encode failed: {rc}")
                    try:
                        rc = lib.sfc_decode_block(encoded, n.value, 3,
                            decoded.ctypes.data + axis * 4, 12, 768, None)
                        if rc:
                            raise RuntimeError(f"decode failed: {rc}")
                        size += n.value
                    finally:
                        free(encoded)
                reconstructed = decoded.astype("float64") @ np.linalg.inv(basis) + predictor
                reconstructed = reconstructed.astype("float32").astype("float64")
                errors = np.linalg.norm(reconstructed[valid] - original[valid], axis=-1)
                violations = errors > args.error
                # Simulate exact XYZ escapes after the FINAL coordinate transform.
                # The format is not implemented: charge a conservative 16 bytes
                # per escaped sample (index plus three original float32 values).
                corrections = int(violations.sum())
                size += corrections * 16
                errors[violations] = 0
                maximum, square_sum = float(errors.max(initial=0)), float(errors @ errors)
                t = totals[method]
                t["bytes"] += size
                t["max"] = max(t["max"], maximum)
                t["sq"] += square_sum
                t["corrections"] += corrections
                outcomes[method] = size, maximum, square_sum
            winner = min(methods, key=lambda m: outcomes[m][0])
            size, maximum, square_sum = outcomes[winner]
            best["bytes"] += size + 4  # Per-group transform selector.
            best["max"] = max(best["max"], maximum)
            best["sq"] += square_sum
            best["choices"][winner] += 1
            blocks += 1
    # Conservatively keep the existing three-channel index. Auxiliary channel
    # payloads are excluded; this experiment compares XYZ storage only.
    overhead = 64 + 3 * 128 + (args.input / "meta.json").stat().st_size + 3 * blocks * 32
    for result in [*totals.values(), best]:
        result["bytes"] += overhead
        result["ratio"] = data.nbytes / result["bytes"]
        result["rms"] = float(np.sqrt(result.pop("sq") / count)) if count else 0.
    result = dict(input=str(args.input), error=args.error, raw_bytes=data.nbytes,
                  blocks=blocks, valid=count, methods=totals, adaptive_joint=best,
                  prototype=True, size_includes_estimated_joint_metadata=True)
    text = json.dumps(result, indent=2) + "\n"
    if args.json:
        args.json.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
