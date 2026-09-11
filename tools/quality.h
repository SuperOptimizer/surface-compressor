/* Bounded reference comparison. Included after TIFF source helpers. */
typedef struct {
  uint64_t n, nonfinite;
  double absolute, square, maximum, low, high, ssim;
  uint64_t windows;
} quality_stats;
static void quality_add(quality_stats *s, double ref, double got) {
  double e = fabs(ref - got);
  s->n++;
  s->absolute += e;
  s->square += e * e;
  s->maximum = fmax(s->maximum, e);
  s->low = fmin(s->low, ref);
  s->high = fmax(s->high, ref);
}
/* Gaussian weighted nonoverlapping 8x8 windows; invalid samples are omitted
 * and weights renormalized. Explicitly labeled to avoid claiming sliding SSIM.
 */
static void quality_ssim(quality_stats *s, const double ref[4096],
                         const double got[4096], const uint8_t valid[4096],
                         double range) {
  double weights[8];
  for (unsigned k = 0; k < 8; k++) {
    double d = (double)k - 3.5;
    weights[k] = exp(-d * d / (2 * 1.5 * 1.5));
  }
  double c1 = .0001 * range * range, c2 = .0009 * range * range;
  for (unsigned by = 0; by < 64; by += 8)
    for (unsigned bx = 0; bx < 64; bx += 8) {
      double total = 0, a = 0, b = 0, aa = 0, ab = 0, bb = 0;
      for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++) {
          unsigned i = (by + y) * 64 + bx + x;
          if (!valid[i])
            continue;
          double w = weights[y] * weights[x];
          total += w;
          a += w * ref[i];
          b += w * got[i];
        }
      if (!total)
        continue;
      a /= total;
      b /= total;
      for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++) {
          unsigned i = (by + y) * 64 + bx + x;
          if (!valid[i])
            continue;
          double w = weights[y] * weights[x], da = ref[i] - a, db = got[i] - b;
          aa += w * da * da;
          ab += w * da * db;
          bb += w * db * db;
        }
      aa /= total;
      ab /= total;
      bb /= total;
      s->ssim += ((2 * a * b + c1) * (2 * ab + c2)) /
                 ((a * a + b * b + c1) * (aa + bb + c2));
      s->windows++;
    }
}
static int quality_source(source *s, const char *reference, int directory,
                          const sfc_channel *ch) {
  char stem[64], path[2048];
  snprintf(stem, sizeof stem, "%s", ch->name);
  if (strchr(stem, '/') || strstr(stem, ".."))
    return -1;
  if (ch->components > 1 && !(ch->flags & SFC_XYZ)) {
    char *suffix = strrchr(stem, '.');
    if (!suffix)
      return -1;
    *suffix = 0;
  }
  if (directory) {
    if (snprintf(path, sizeof path, "%s/%s.tif", reference, stem) >=
        (int)sizeof path)
      return -1;
  } else if (snprintf(path, sizeof path, "%s", reference) >= (int)sizeof path)
    return -1;
  if (source_open(s, path, stem))
    return -1;
  return s->w == ch->width && s->h == ch->height &&
                 ((ch->flags & SFC_XYZ) ? 0 : ch->component) < s->spp &&
                 ((ch->flags & SFC_COORDINATE) || s->type == ch->dtype)
             ? 0
             : -1;
}
static int quality_report(const char *file, const char *reference) {
  struct stat st, fs;
  if (stat(reference, &st) || stat(file, &fs))
    return die("reference or compressed file missing");
  int directory = S_ISDIR(st.st_mode), rc = 1;
  sfc_reader *r = NULL;
  if (sfc_open_file(file, &r))
    return die("invalid compressed file");
  uint64_t samples = 0;
  double seconds = 0;
  for (uint32_t ch = 0; ch < sfc_channel_count(r); ch++) {
    const sfc_channel *c = sfc_channel_info(r, ch);
    source src = {0};
    if (quality_source(&src, reference, directory, c)) {
      source_close(&src);
      goto done;
    }
    quality_stats stats = {.low = DBL_MAX, .high = -DBL_MAX};
    /* Fixed range for SSIM across this whole channel. Floating channels use
     * source extent; integer channels use the nominal full-scale range. */
    double range = c->dtype == SFC_U8 ? 255 : c->dtype == SFC_U16 ? 65535 : 0;
    if (!range) {
      double low = DBL_MAX, high = -DBL_MAX;
      for (uint32_t y = 0; y < src.h; y++)
        for (uint32_t x = 0; x < src.w; x++) {
          uint8_t v[4];
          if (get(&src, x, y, (c->flags & SFC_XYZ) ? 0 : c->component, v)) {
            source_close(&src);
            goto done;
          }
          double a = numeric(v, src.type);
          if (isfinite(a)) {
            low = fmin(low, a);
            high = fmax(high, a);
          }
        }
      range = high > low ? high - low : 1;
    }
    for (uint64_t by = 0; by < (c->height + 63) / 64; by++)
      for (uint64_t bx = 0; bx < (c->width + 63) / 64; bx++) {
        uint8_t out[16384], valid[4096], finite[4096] = {0};
        double a[4096], b[4096];
        size_t z = sfc_sample_size(c->dtype);
        double t = (double)clock() / CLOCKS_PER_SEC;
        int err = sfc_read_block(r, ch, bx, by, out, z, 64 * z, valid);
        seconds += (double)clock() / CLOCKS_PER_SEC - t;
        if (err) {
          source_close(&src);
          goto done;
        }
        for (unsigned i = 0; i < 4096; i++) {
          uint64_t x = bx * 64 + i % 64, y = by * 64 + i / 64;
          if (x >= c->width || y >= c->height)
            continue;
          samples++;
          uint8_t v[4];
          if (get(&src, (uint32_t)x, (uint32_t)y,
                  (c->flags & SFC_XYZ) ? 0 : c->component, v)) {
            source_close(&src);
            goto done;
          }
          if (!valid[i]) {
            if (!(c->flags & SFC_COORDINATE)) {
              source_close(&src);
              goto done;
            }
            continue; /* XYZ validity is independently checked below. */
          }
          a[i] = numeric(v, src.type);
          b[i] = numeric(out + i * z, c->dtype);
          if (!isfinite(a[i]) || !isfinite(b[i])) {
            if (src.type != c->dtype || memcmp(v, out + i * z, z)) {
              source_close(&src);
              goto done;
            }
            stats.nonfinite++;
            continue;
          }
          if (fabs(a[i] - b[i]) > c->tolerance ||
              ((c->flags & SFC_EXACT) && memcmp(v, out + i * z, z))) {
            source_close(&src);
            die("reference error exceeds channel tolerance");
            goto done;
          }
          finite[i] = 1;
          quality_add(&stats, a[i], b[i]);
        }
        quality_ssim(&stats, a, b, finite, range);
      }
    double rms = stats.n ? sqrt(stats.square / stats.n) : 0;
    printf("quality %s samples=%llu max=%.9g mae=%.9g rms=%.9g psnr=%.9g "
           "ssim_8x8=%.9g range=%.9g\n",
           c->name, (unsigned long long)stats.n, stats.maximum,
           stats.n ? stats.absolute / stats.n : 0, rms,
           rms ? 20 * log10(range / rms) : INFINITY,
           stats.windows ? stats.ssim / stats.windows : 1, range);
    source_close(&src);
  }
  if (directory && sfc_find_channel(r, "x") >= 0 &&
      sfc_find_channel(r, "y") >= 0 && sfc_find_channel(r, "z") >= 0) {
    source src[3] = {{0}}, mask = {0};
    int ids[3], have_mask = 0;
    quality_stats stats = {0};
    char path[2048];
    snprintf(path, sizeof path, "%s/mask.tif", reference);
    if (!access(path, F_OK)) {
      if (source_open(&mask, path, "mask"))
        goto xyz_done;
      have_mask = 1;
    }
    for (unsigned k = 0; k < 3; k++) {
      const char *name = k == 0 ? "x" : k == 1 ? "y" : "z";
      ids[k] = sfc_find_channel(r, name);
      const sfc_channel *c = sfc_channel_info(r, (uint32_t)ids[k]);
      if (c->dtype != SFC_F32 || !(c->flags & SFC_COORDINATE) ||
          quality_source(src + k, reference, 1, c))
        goto xyz_done;
      if (k && (src[k].w != src[0].w || src[k].h != src[0].h))
        goto xyz_done;
    }
    for (uint64_t by = 0; by < (src[0].h + 63ull) / 64; by++)
      for (uint64_t bx = 0; bx < (src[0].w + 63ull) / 64; bx++) {
        float out[3][4096];
        uint8_t valid[3][4096];
        if (ids[1] == ids[0] + 1 && ids[2] == ids[0] + 2) {
          float joint[4096][3];
          if (sfc_read_xyz(r, (uint32_t)ids[0], bx, by, joint, 12, 768,
                           valid[0]))
            goto xyz_done;
          for (unsigned k = 0; k < 3; k++) {
            memcpy(valid[k], valid[0], 4096);
            for (unsigned i = 0; i < 4096; i++)
              out[k][i] = joint[i][k];
          }
        } else
          for (unsigned k = 0; k < 3; k++)
            if (sfc_read_block(r, (uint32_t)ids[k], bx, by, out[k], 4, 256,
                               valid[k]))
              goto xyz_done;
        for (unsigned i = 0; i < 4096; i++) {
          uint64_t x = bx * 64 + i % 64, y = by * 64 + i / 64;
          if (x >= src[0].w || y >= src[0].h)
            continue;
          double original[3], square = 0;
          int expected = 1;
          for (unsigned k = 0; k < 3; k++) {
            uint8_t v[4];
            if (get(src + k, (uint32_t)x, (uint32_t)y, 0, v))
              goto xyz_done;
            original[k] = numeric(v, src[k].type);
            expected &= isfinite(original[k]);
            double d = (double)out[k][i] - original[k];
            square += d * d;
          }
          expected &= original[2] > 0;
          if (expected && have_mask && mask.w >= src[0].w &&
              mask.h >= src[0].h && mask.w % src[0].w == 0 &&
              mask.h % src[0].h == 0) {
            unsigned sx = mask.w / src[0].w, sy = mask.h / src[0].h;
            for (unsigned yy = 0; expected && yy < sy; yy++)
              for (unsigned xx = 0; xx < sx; xx++) {
                uint8_t v[4];
                if (get(&mask, (uint32_t)x * sx + xx, (uint32_t)y * sy + yy, 0,
                        v))
                  goto xyz_done;
                if (!(numeric(v, mask.type) >= 255)) {
                  expected = 0;
                  break;
                }
              }
          }
          for (unsigned k = 0; k < 3; k++)
            if (valid[k][i] != expected || (!expected && out[k][i] != -1))
              goto xyz_done;
          if (expected &&
              (sfc_channel_info(r, (uint32_t)ids[0])->flags & SFC_XYZ) &&
              sqrt(square) > sfc_channel_info(r, (uint32_t)ids[0])->tolerance)
            goto xyz_done;
          if (expected)
            quality_add(&stats, 0, sqrt(square));
        }
      }
    printf("quality xyz samples=%llu max=%.9g mae=%.9g rms=%.9g\n",
           (unsigned long long)stats.n, stats.maximum,
           stats.n ? stats.absolute / stats.n : 0,
           stats.n ? sqrt(stats.square / stats.n) : 0);
    rc = 0;
  xyz_done:
    for (unsigned k = 0; k < 3; k++)
      source_close(src + k);
    source_close(&mask);
    if (rc)
      goto done;
  } else
    rc = 0;
  printf("storage bytes=%llu bytes/sample=%.6g decode_Msamples/s=%.6g\n",
         (unsigned long long)fs.st_size,
         samples ? (double)fs.st_size / samples : 0,
         seconds ? samples / seconds / 1e6 : 0);
done:
  sfc_close(r);
  return rc ? die("reference comparison failed") : 0;
}
