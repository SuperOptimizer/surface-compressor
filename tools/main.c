#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "surfcomp.h"
#include <dirent.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <tiffio.h>
#include <time.h>
#include <unistd.h>
#define LIMIT 4096
#define SCRATCH_LIMIT (64u << 20)
typedef struct {
  TIFF *t;
  char path[2048], stem[64];
  uint32_t w, h, tw, th;
  uint16_t spp, planar, photometric;
  sfc_dtype type;
  size_t bytes, decoded_bytes;
  uint8_t *scratch;
  uint32_t cached;
  uint64_t stamp;
  int tiled;
} source;
static source *scratch_owner[4];
static size_t scratch_bytes;
static uint64_t scratch_clock;
static int source_scratch(source *s) {
  s->stamp = ++scratch_clock;
  if (s->scratch)
    return 0;
  for (;;) {
    int empty = -1, victim = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < 4; i++) {
      if (!scratch_owner[i])
        empty = i;
      else if (scratch_owner[i]->stamp < oldest) {
        oldest = scratch_owner[i]->stamp;
        victim = i;
      }
    }
    if (empty >= 0 && scratch_bytes + s->bytes <= SCRATCH_LIMIT) {
      s->scratch = malloc(s->bytes);
      if (!s->scratch)
        return -1;
      scratch_bytes += s->bytes;
      scratch_owner[empty] = s;
      s->cached = UINT32_MAX;
      return 0;
    }
    if (victim < 0)
      return -1;
    source *v = scratch_owner[victim];
    scratch_bytes -= v->bytes;
    free(v->scratch);
    v->scratch = NULL;
    v->cached = UINT32_MAX;
    scratch_owner[victim] = NULL;
  }
}
typedef struct {
  unsigned src, sample;
  int coordinate;
  sfc_channel c;
} channel;
static int die(const char *what) {
  fprintf(stderr, "surface-compressor: %s\n", what);
  return 1;
}
static int source_open(source *s, const char *path, const char *stem) {
  memset(s, 0, sizeof *s);
  snprintf(s->path, sizeof s->path, "%s", path);
  snprintf(s->stem, sizeof s->stem, "%s", stem);
  s->t = TIFFOpen(path, "r");
  if (!s->t)
    return -1;
  uint16_t bits = 0, fmt = 0;
  TIFFGetField(s->t, TIFFTAG_IMAGEWIDTH, &s->w);
  TIFFGetField(s->t, TIFFTAG_IMAGELENGTH, &s->h);
  TIFFGetFieldDefaulted(s->t, TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(s->t, TIFFTAG_SAMPLEFORMAT, &fmt);
  TIFFGetFieldDefaulted(s->t, TIFFTAG_SAMPLESPERPIXEL, &s->spp);
  TIFFGetFieldDefaulted(s->t, TIFFTAG_PLANARCONFIG, &s->planar);
  TIFFGetFieldDefaulted(s->t, TIFFTAG_PHOTOMETRIC, &s->photometric);
  if (s->photometric != PHOTOMETRIC_MINISBLACK &&
      s->photometric != PHOTOMETRIC_MINISWHITE &&
      s->photometric != PHOTOMETRIC_RGB)
    return -1;
  if (s->planar != PLANARCONFIG_CONTIG && s->planar != PLANARCONFIG_SEPARATE)
    return -1;
  s->type = fmt == SAMPLEFORMAT_UINT
                ? (bits == 8    ? SFC_U8
                   : bits == 16 ? SFC_U16
                                : 0)
                : (fmt == SAMPLEFORMAT_IEEEFP && bits == 32 ? SFC_F32 : 0);
  if (!s->type || !s->w || !s->h || !s->spp)
    return -1;
  s->tiled = TIFFIsTiled(s->t);
  if (s->tiled) {
    TIFFGetField(s->t, TIFFTAG_TILEWIDTH, &s->tw);
    TIFFGetField(s->t, TIFFTAG_TILELENGTH, &s->th);
    s->bytes = (size_t)TIFFTileSize(s->t);
  } else {
    s->tw = s->w;
    TIFFGetFieldDefaulted(s->t, TIFFTAG_ROWSPERSTRIP, &s->th);
    if (s->th > s->h)
      s->th = s->h;
    s->bytes = (size_t)TIFFStripSize(s->t);
  }
  if (!s->tw || !s->th || !s->bytes || s->bytes > SCRATCH_LIMIT)
    return -1;
  s->cached = UINT32_MAX;
  return 0;
}
static void source_close(source *s) {
  if (s->t)
    TIFFClose(s->t);
  for (int i = 0; i < 4; i++)
    if (scratch_owner[i] == s) {
      scratch_owner[i] = NULL;
      scratch_bytes -= s->bytes;
    }
  free(s->scratch);
}
static int get(source *s, uint32_t x, uint32_t y, unsigned sample, void *out) {
  if (x >= s->w || y >= s->h || sample >= s->spp || source_scratch(s))
    return -1;
  uint16_t plane = s->planar == PLANARCONFIG_SEPARATE ? (uint16_t)sample : 0;
  uint32_t id = s->tiled ? TIFFComputeTile(s->t, x, y, 0, plane)
                         : TIFFComputeStrip(s->t, y, plane);
  if (id != s->cached) {
    tmsize_t n =
        s->tiled
            ? TIFFReadEncodedTile(s->t, id, s->scratch, (tmsize_t)s->bytes)
            : TIFFReadEncodedStrip(s->t, id, s->scratch, (tmsize_t)s->bytes);
    if (n <= 0)
      return -1;
    s->decoded_bytes = (size_t)n;
    s->cached = id;
  }
  size_t z = sfc_sample_size(s->type),
         np = s->planar == PLANARCONFIG_SEPARATE ? 1 : s->spp;
  size_t i = ((size_t)(y % s->th) * s->tw + (s->tiled ? x % s->tw : x)) * np +
             (s->planar == PLANARCONFIG_SEPARATE ? 0 : sample);
  if (s->decoded_bytes < z || i > (s->decoded_bytes - z) / z)
    return -1;
  memcpy(out, s->scratch + i * z, z);
  return 0;
}
static double numeric(const void *p, sfc_dtype type) {
  if (type == SFC_U8)
    return *(const uint8_t *)p;
  if (type == SFC_U16) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
  }
  float v;
  memcpy(&v, p, 4);
  return v;
}
static int read_text(const char *path, uint8_t **p, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  if (fseek(f, 0, SEEK_END)) {
    fclose(f);
    return -1;
  }
  long size = ftell(f);
  if (size < 0 || size > 64 * 1024 * 1024) {
    fclose(f);
    return -1;
  }
  rewind(f);
  *p = malloc((size_t)size + 1);
  if (!*p) {
    fclose(f);
    return -1;
  }
  *n = (size_t)size;
  int ok = fread(*p, 1, *n, f) == *n;
  (*p)[*n] = 0;
  fclose(f);
  return ok ? 0 : -1;
}
static int cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}
static int encode(const char *input, const char *output, double error) {
  struct stat st;
  if (stat(input, &st))
    return die("input missing");
  int profile = S_ISDIR(st.st_mode);
  source *src = calloc(LIMIT, sizeof *src);
  channel *channels = calloc(LIMIT, sizeof *channels);
  if (!src || !channels) {
    free(src);
    free(channels);
    return die("allocation failed");
  }
  unsigned ns = 0, nc = 0;
  int rc = 1;
  uint8_t *meta = NULL;
  size_t ml = 0;
  char temp[2048];
  snprintf(temp, sizeof temp, "%s.tmp.%ld", output, (long)getpid());
  sfc_writer *writer = NULL;
  int temp_created = 0;
  int xyz[3] = {-1, -1, -1}, mask_source = -1;
  if (profile) {
    char path[2048];
    snprintf(path, sizeof path, "%s/meta.json", input);
    if (read_text(path, &meta, &ml)) {
      die("missing meta.json");
      goto done;
    }
    DIR *dir = opendir(input);
    if (!dir)
      goto done;
    char *names[LIMIT];
    unsigned nn = 0;
    int enumerate_failed = 0;
    struct dirent *de;
    while ((de = readdir(dir))) {
      size_t len = strlen(de->d_name);
      if (len > 4 && !strcmp(de->d_name + len - 4, ".tif")) {
        if (nn == LIMIT) {
          enumerate_failed = 1;
          break;
        }
        names[nn] = strdup(de->d_name);
        if (!names[nn]) {
          enumerate_failed = 1;
          break;
        }
        nn++;
      }
    }
    closedir(dir);
    if (enumerate_failed) {
      for (unsigned i = 0; i < nn; i++)
        free(names[i]);
      die("too many input channels or allocation failure");
      goto done;
    }
    qsort(names, nn, sizeof *names, cmp);
    int failed = 0;
    for (unsigned i = 0; i < nn; i++) {
      char stem[64];
      size_t len = strlen(names[i]) - 4;
      if (len >= sizeof stem) {
        failed = 1;
        free(names[i]);
        continue;
      }
      memcpy(stem, names[i], len);
      stem[len] = 0;
      snprintf(path, sizeof path, "%s/%s", input, names[i]);
      free(names[i]);
      if (failed)
        continue;
      if (source_open(src + ns, path, stem)) {
        source_close(src + ns);
        failed = 1;
        continue;
      }
      for (int a = 0; a < 3; a++)
        if (stem[0] == "xyz"[a] && !stem[1])
          xyz[a] = (int)ns;
      if (!strcmp(stem, "mask"))
        mask_source = (int)ns;
      ns++;
    }
    if (failed) {
      die("unsupported TIFF (expected u8/u16/float32, strips/tiles <=64 MiB)");
      goto done;
    }
    for (int a = 0; a < 3; a++)
      if (xyz[a] < 0 || src[xyz[a]].spp != 1) {
        die("tifxyz needs single-channel x/y/z");
        goto done;
      }
    for (int a = 1; a < 3; a++)
      if (src[xyz[a]].w != src[xyz[0]].w || src[xyz[a]].h != src[xyz[0]].h) {
        die("XYZ dimensions differ");
        goto done;
      }
  } else {
    if (error <= 0) {
      die("image encoding requires --error in sample units");
      goto done;
    }
    if (source_open(src, input, "image")) {
      source_close(src);
      goto done;
    }
    ns = 1;
    char image_meta[80];
    snprintf(image_meta, sizeof image_meta,
             "{\"format\":\"sfc.image\",\"photometric\":%u}", src->photometric);
    meta = (uint8_t *)strdup(image_meta);
    ml = strlen(image_meta);
    if (!meta)
      goto done;
  }
  for (unsigned i = 0; i < ns; i++)
    for (unsigned ch = 0; ch < src[i].spp; ch++) {
      if (nc == LIMIT) {
        die("too many channels");
        goto done;
      }
      channel *c = channels + nc++;
      c->src = i;
      c->sample = ch;
      c->coordinate =
          profile && ((int)i == xyz[0] || (int)i == xyz[1] || (int)i == xyz[2]);
      if (src[i].spp == 1)
        snprintf(c->c.name, 64, "%s", src[i].stem);
      else if (snprintf(c->c.name, 64, "%s.%u", src[i].stem, ch) >= 64) {
        die("channel name too long");
        goto done;
      }
      c->c.components = src[i].spp;
      c->c.component = ch;
      c->c.width = src[i].w;
      c->c.height = src[i].h;
      c->c.dtype = c->coordinate ? SFC_F32 : src[i].type;
      int exact = profile && !c->coordinate;
      c->c.flags = c->coordinate ? SFC_COORDINATE : exact ? SFC_EXACT : 0;
      c->c.tolerance = c->coordinate ? (error > 0 ? error : .1) / sqrt(3.0)
                       : exact       ? 0
                                     : error;
    }
  if (profile) {
    /* Coordinate sources may be separated by auxiliary files in directory
     * order. */
    for (unsigned k = 0; k < 3; k++) {
      unsigned j = k;
      while (j < nc && channels[j].src != (unsigned)xyz[k])
        j++;
      if (j == nc)
        goto done;
      channel tmp = channels[j];
      memmove(channels + k + 1, channels + k, (j - k) * sizeof *channels);
      channels[k] = tmp;
      channels[k].c.flags = SFC_COORDINATE | SFC_XYZ;
      channels[k].c.components = 3;
      channels[k].c.component = k;
      channels[k].c.tolerance = error > 0 ? error : .1;
    }
  }
  sfc_channel *descs = malloc(nc * sizeof *descs);
  if (!descs)
    goto done;
  for (unsigned i = 0; i < nc; i++)
    descs[i] = channels[i].c;
  int created = sfc_create(temp, descs, nc, meta, ml, &writer);
  free(descs);
  if (created)
    goto done;
  temp_created = 1;
  for (unsigned i = 0; i < nc; i++) {
    channel *c = channels + i;
    source *s = src + c->src;
    size_t z = sfc_sample_size(c->c.dtype);
    uint8_t data[16384], valid[4096];
    float joint[4096][3];
    for (uint64_t by = 0; by < (s->h + 63ull) / 64; by++)
      for (uint64_t bx = 0; bx < (s->w + 63ull) / 64; bx++) {
        memset(data, 0, sizeof data);
        memset(joint, 0, sizeof joint);
        memset(valid, 0, sizeof valid);
        /* Finish one source plane before touching the next. Large TIFF strips
         * can exceed the combined cache budget; interleaving x/y/z per pixel
         * would repeatedly evict and inflate them. */
        if (c->coordinate)
          for (unsigned a = 0; a < 3; a++) {
            source *axis = src + xyz[a];
            for (unsigned k = 0; k < 4096; k++) {
              uint64_t x = bx * 64 + k % 64, y = by * 64 + k / 64;
              if (x >= s->w || y >= s->h)
                continue;
              uint8_t sample[4];
              if (get(axis, (uint32_t)x, (uint32_t)y, 0, sample))
                goto done;
              joint[k][a] = (float)numeric(sample, axis->type);
            }
          }
        for (unsigned k = 0; k < 4096; k++) {
          uint64_t x = bx * 64 + k % 64, y = by * 64 + k / 64;
          if (x >= s->w || y >= s->h)
            continue;
          uint8_t sample[4];
          if (c->coordinate) {
            valid[k] = 1;
            for (unsigned a = 0; a < 3; a++) {
              if (!isfinite(joint[k][a]) || (a == 2 && joint[k][a] <= 0))
                valid[k] = 0;
            }
            if (mask_source >= 0 && valid[k]) {
              source *m = src + mask_source;
              if (m->w % s->w == 0 && m->h % s->h == 0 && m->w >= s->w &&
                  m->h >= s->h) {
                unsigned sx = m->w / s->w, sy = m->h / s->h;
                for (unsigned yy = 0; valid[k] && yy < sy; yy++)
                  for (unsigned xx = 0; xx < sx; xx++) {
                    uint8_t vmask[4];
                    if (get(m, (uint32_t)x * sx + xx, (uint32_t)y * sy + yy, 0,
                            vmask))
                      goto done;
                    if (!(numeric(vmask, m->type) >= 255)) {
                      valid[k] = 0;
                      break;
                    }
                  }
              }
            }
          } else {
            if (get(s, (uint32_t)x, (uint32_t)y, c->sample, sample))
              goto done;
            memcpy(data + k * z, sample, z);
            valid[k] = 1;
          }
        }
        if (c->coordinate ? sfc_write_xyz_block(writer, joint, 12, 768, valid)
                          : sfc_write_block(writer, data, z, 64 * z, valid))
          goto done;
      }
    fprintf(stderr, "encoded %s (%ux%u)\n", c->coordinate ? "xyz" : c->c.name,
            s->w, s->h);
    if (c->coordinate)
      i += 2;
  }
  rc = sfc_finish(writer);
  writer = NULL;
  if (!rc && rename(temp, output))
    rc = 1;
done:
  if (writer)
    sfc_cancel(writer);
  if (rc && temp_created)
    unlink(temp);
  for (unsigned i = 0; i < ns; i++)
    source_close(src + i);
  free(src);
  free(channels);
  free(meta);
  return rc;
}
static int export_xyz(sfc_reader *r, uint32_t first, const char *dir) {
  TIFF *t[3] = {0};
  int rc = 1;
  float xyz[4096][3], plane[4096];
  char path[2048];
  const sfc_channel *c = sfc_channel_info(r, first);
  if (c->width > UINT32_MAX || c->height > UINT32_MAX)
    return 1;
  for (unsigned k = 0; k < 3; k++) {
    const sfc_channel *a = sfc_channel_info(r, first + k);
    if (strchr(a->name, '/') || strstr(a->name, "..") ||
        snprintf(path, sizeof path, "%s/%s.tif", dir, a->name) >=
            (int)sizeof path)
      goto done;
    if (!access(path, F_OK) || (t[k] = TIFFOpen(path, "w8")) == NULL)
      goto done;
    TIFFSetField(t[k], TIFFTAG_IMAGEWIDTH, (uint32_t)c->width);
    TIFFSetField(t[k], TIFFTAG_IMAGELENGTH, (uint32_t)c->height);
    TIFFSetField(t[k], TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(t[k], TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(t[k], TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(t[k], TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t[k], TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(t[k], TIFFTAG_TILEWIDTH, 64);
    TIFFSetField(t[k], TIFFTAG_TILELENGTH, 64);
    TIFFSetField(t[k], TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(t[k], TIFFTAG_PREDICTOR, 3);
  }
  for (uint64_t y = 0; y < c->height; y += 64)
    for (uint64_t x = 0; x < c->width; x += 64) {
      if (sfc_read_xyz(r, first, x / 64, y / 64, xyz, 12, 768, NULL))
        goto done;
      for (unsigned k = 0; k < 3; k++) {
        for (unsigned i = 0; i < 4096; i++)
          plane[i] = xyz[i][k];
        if (TIFFWriteTile(t[k], plane, (uint32_t)x, (uint32_t)y, 0, 0) < 0)
          goto done;
      }
    }
  rc = 0;
done:
  for (unsigned k = 0; k < 3; k++)
    if (t[k])
      TIFFClose(t[k]);
  return rc;
}
static int decode_into(const char *input, const char *dir) {
  sfc_reader *r = NULL;
  if (sfc_open_file(input, &r))
    return die("invalid compressed file");
  if (mkdir(dir, 0755) && access(dir, W_OK)) {
    sfc_close(r);
    return die("cannot create destination");
  }
  size_t ml = (size_t)sfc_metadata_size(r);
  uint8_t *meta = malloc(ml + 1);
  int rc = 1;
  char path[2048];
  if (!meta || sfc_read_metadata(r, meta, ml))
    goto done;
  meta[ml] = 0;
  snprintf(path, sizeof path, "%s/meta.json", dir);
  FILE *f = fopen(path, "wb");
  if (!f)
    goto done;
  int written = fwrite(meta, 1, ml, f) == ml;
  if (fclose(f) || !written)
    goto done;
  for (uint32_t c = 0; c < sfc_channel_count(r);) {
    const sfc_channel *ch = sfc_channel_info(r, c);
    if (ch->flags & SFC_XYZ) {
      if (export_xyz(r, c, dir))
        goto done;
      c += 3;
      continue;
    }
    unsigned planes = (ch->flags & SFC_XYZ) ? 1
                      : ch->components      ? ch->components
                                            : 1;
    if (strchr(ch->name, '/') || strstr(ch->name, "..") ||
        ch->width > UINT32_MAX || ch->height > UINT32_MAX)
      goto done;
    char stem[64];
    snprintf(stem, sizeof stem, "%s", ch->name);
    if (planes > 1) {
      char *suffix = strrchr(stem, '.');
      if (!suffix || strcmp(suffix, ".0"))
        goto done;
      *suffix = 0;
    }
    snprintf(path, sizeof path, "%s/%s.tif", dir, stem);
    if (!access(path, F_OK))
      goto done;
    TIFF *t = TIFFOpen(path, "w8");
    if (!t)
      goto done;
    size_t z = sfc_sample_size(ch->dtype);
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, (uint32_t)ch->width);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, (uint32_t)ch->height);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)planes);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, (uint16_t)(z * 8));
    TIFFSetField(t, TIFFTAG_SAMPLEFORMAT,
                 ch->dtype == SFC_F32 ? SAMPLEFORMAT_IEEEFP
                                      : SAMPLEFORMAT_UINT);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_SEPARATE);
    unsigned photo = PHOTOMETRIC_MINISBLACK;
    if (strstr((char *)meta, "\"format\":\"sfc.image\"")) {
      const char *field = strstr((char *)meta, "\"photometric\":");
      if (field && sscanf(field, "\"photometric\":%u", &photo) != 1)
        photo = PHOTOMETRIC_MINISBLACK;
    }
    if (photo > PHOTOMETRIC_RGB || (photo == PHOTOMETRIC_RGB && planes < 3)) {
      TIFFClose(t);
      goto done;
    }
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, photo);
    unsigned base_planes = photo == PHOTOMETRIC_RGB ? 3 : 1;
    if (planes > base_planes) {
      uint16_t extras[SFC_MAX_CHANNELS] = {0};
      TIFFSetField(t, TIFFTAG_EXTRASAMPLES, (uint16_t)(planes - base_planes),
                   extras);
    }
    TIFFSetField(t, TIFFTAG_TILEWIDTH, 64);
    TIFFSetField(t, TIFFTAG_TILELENGTH, 64);
    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(t, TIFFTAG_PREDICTOR, ch->dtype == SFC_F32 ? 3 : 2);
    uint8_t block[16384];
    int bad = 0;
    for (unsigned plane = 0; !bad && plane < planes; plane++)
      for (uint64_t y = 0; !bad && y < ch->height; y += 64)
        for (uint64_t x = 0; !bad && x < ch->width; x += 64) {
          if (sfc_read_block(r, c + plane, x / 64, y / 64, block, z, 64 * z,
                             NULL) ||
              TIFFWriteTile(t, block, (uint32_t)x, (uint32_t)y, 0,
                            (uint16_t)plane) < 0)
            bad = 1;
        }
    TIFFClose(t);
    if (bad)
      goto done;
    c += planes;
  }
  rc = 0;
done:
  free(meta);
  sfc_close(r);
  return rc;
}
/* Export into a private sibling directory; failed reads never publish a
 * partially reconstructed surface or overwrite an existing destination. */
static int decode(const char *input, const char *dir) {
  struct stat st;
  if (!lstat(dir, &st))
    return die("destination already exists");
  char temp[2048];
  if (snprintf(temp, sizeof temp, "%s.tmp.XXXXXX", dir) >= (int)sizeof temp ||
      !mkdtemp(temp))
    return die("cannot create temporary export directory");
  int rc = decode_into(input, temp);
  if (!rc && !rename(temp, dir))
    return 0;
  DIR *d = opendir(temp);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
        continue;
      char path[4096];
      if (snprintf(path, sizeof path, "%s/%s", temp, e->d_name) <
          (int)sizeof path)
        unlink(path);
    }
    closedir(d);
  }
  rmdir(temp);
  return die("export failed; destination was not published");
}
static int inspect(const char *path, int verify) {
  sfc_reader *r;
  if (sfc_open_file(path, &r))
    return die("invalid compressed file");
  int rc = 0;
  double start = (double)clock() / CLOCKS_PER_SEC;
  uint64_t count = 0;
  for (uint32_t c = 0; c < sfc_channel_count(r); c++) {
    const sfc_channel *ch = sfc_channel_info(r, c);
    printf("%s %llu x %llu type=%u error=%.9g %s\n", ch->name,
           (unsigned long long)ch->width, (unsigned long long)ch->height,
           ch->dtype, ch->tolerance,
           ch->flags & SFC_EXACT ? "exact" : "bounded lossy");
    if (verify && (!(ch->flags & SFC_XYZ) || !ch->component)) {
      uint8_t block[49152];
      size_t z = sfc_sample_size(ch->dtype);
      for (uint64_t y = 0; y < (ch->height + 63) / 64; y++)
        for (uint64_t x = 0; x < (ch->width + 63) / 64; x++) {
          if ((ch->flags & SFC_XYZ)
                  ? sfc_read_xyz(r, c, x, y, block, 12, 768, NULL)
                  : sfc_read_block(r, c, x, y, block, z, 64 * z, NULL)) {
            rc = 1;
            goto done;
          }
          count++;
        }
    }
  }
  if (verify)
    printf("verified %llu blocks in %.3f CPU seconds\n",
           (unsigned long long)count, (double)clock() / CLOCKS_PER_SEC - start);
done:
  sfc_close(r);
  return rc;
}
#include "quality.h"

int main(int argc, char **argv) {
  const char *usage =
      "encode INPUT OUT.sfc [--error E] | decode INPUT.sfc DIR | "
      "info|verify|bench INPUT.sfc [--reference TIFF_OR_DIRECTORY] | --version";
  if (argc == 2 && !strcmp(argv[1], "--version")) {
    printf("surface-compressor %s (container %u)\n", sfc_version(),
           sfc_format_version());
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts(usage);
    return 0;
  }
  TIFFSetWarningHandler(NULL);
  if (argc < 3)
    return die(usage);
  if (!strcmp(argv[1], "encode")) {
    if (argc != 4 && argc != 6)
      return die(usage);
    double error = 0;
    if (argc == 6) {
      if (strcmp(argv[4], "--error"))
        return die("unknown encode option");
      char *end;
      error = strtod(argv[5], &end);
      if (end == argv[5] || *end || !isfinite(error) || error <= 0)
        return die("invalid error tolerance");
    }
    return encode(argv[2], argv[3], error);
  }
  if (!strcmp(argv[1], "decode"))
    return argc == 4 ? decode(argv[2], argv[3]) : die(usage);
  if (!strcmp(argv[1], "info"))
    return argc == 3 ? inspect(argv[2], 0) : die(usage);
  if (!strcmp(argv[1], "verify") || !strcmp(argv[1], "bench")) {
    if (argc == 3)
      return inspect(argv[2], 1);
    if (argc == 5 && !strcmp(argv[3], "--reference"))
      return quality_report(argv[2], argv[4]);
    return die(usage);
  }
  return die(usage);
}
