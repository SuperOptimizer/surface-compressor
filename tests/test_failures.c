/* Faults belong to the test caller; production has no fault API or global
 * state. */
#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static long alloc_budget = -1, io_budget = -1, read_budget = -1;
static unsigned faults = 0;
static int short_reads = 0;
static int trip(long *budget) {
  if (*budget < 0)
    return 0;
  if ((*budget)-- == 0) {
    faults++;
    return 1;
  }
  return 0;
}
static char *fault_strdup(const char *p) {
  return trip(&alloc_budget) ? NULL : strdup(p);
}
static off_t fault_ftello(FILE *f) {
  if (trip(&io_budget)) {
    errno = EIO;
    return -1;
  }
  return ftello(f);
}
static FILE *fault_fopen(const char *path, const char *mode) {
  if (trip(&io_budget)) {
    errno = EIO;
    return NULL;
  }
  return fopen(path, mode);
}
static void *fault_malloc(size_t n) {
  return trip(&alloc_budget) ? NULL : malloc(n);
}
static void *fault_calloc(size_t n, size_t z) {
  return trip(&alloc_budget) ? NULL : calloc(n, z);
}
static void *fault_realloc(void *p, size_t n) {
  return trip(&alloc_budget) ? NULL : realloc(p, n);
}
static size_t fault_fwrite(const void *p, size_t z, size_t n, FILE *f) {
  return fwrite(p, z, trip(&io_budget) && n ? n - 1 : n, f);
}
static int fault_fseeko(FILE *f, off_t n, int whence) {
  if (trip(&io_budget)) {
    errno = EIO;
    return -1;
  }
  return fseeko(f, n, whence);
}
static int fault_fflush(FILE *f) {
  if (trip(&io_budget)) {
    errno = EIO;
    return EOF;
  }
  return fflush(f);
}
static int fault_fclose(FILE *f) {
  int rc = fclose(f);
  return trip(&io_budget) ? EOF : rc;
}
static ssize_t fault_pread(int fd, void *p, size_t n, off_t off) {
  if (trip(&read_budget)) {
    errno = EIO;
    return -1;
  }
  return pread(fd, p, short_reads && n > 7 ? 7 : n, off);
}
#define strdup fault_strdup
#define ftello fault_ftello
#define fopen fault_fopen
#define malloc fault_malloc
#define calloc fault_calloc
#define realloc fault_realloc
#define fwrite fault_fwrite
#define fseeko fault_fseeko
#define fflush fault_fflush
#define fclose fault_fclose
#define pread fault_pread
#include "../src/surfcomp.c"
#undef strdup
#undef ftello
#undef fopen
#undef malloc
#undef calloc
#undef realloc
#undef fwrite
#undef fseeko
#undef fflush
#undef fclose
#undef pread
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static float source[4096][3];
static int write_file(const char *path, int joint) {
  sfc_channel c[3] = {{0}};
  for (unsigned k = 0; k < (joint ? 3u : 1u); k++) {
    c[k].name[0] = "xyz"[k];
    c[k].width = c[k].height = 64;
    c[k].dtype = SFC_F32;
    c[k].flags = SFC_COORDINATE | (joint ? SFC_XYZ : 0);
    c[k].components = joint ? 3 : 0;
    c[k].component = joint ? k : 0;
    c[k].tolerance = .25;
  }
  sfc_writer *w = NULL;
  int rc = sfc_create(path, c, joint ? 3 : 1, NULL, 0, &w);
  if (rc)
    return rc;
  rc = joint ? sfc_write_xyz_block(w, source, 12, 768, NULL)
             : sfc_write_block(w, source, 12, 768, NULL);
  if (rc) {
    sfc_cancel(w);
    return rc;
  }
  return sfc_finish(w); /* Consumed even on injected failure. */
}
static int read_file_test(const char *path, int joint) {
  sfc_reader *r = NULL;
  int rc = sfc_open_file(path, &r);
  if (rc)
    return rc;
  float out[4096][3];
  uint8_t mask[4096];
  memset(out, 0x43, sizeof out);
  memset(mask, 0x43, sizeof mask);
  rc = joint ? sfc_read_xyz(r, 0, 0, 0, out, 12, 768, mask)
             : sfc_read_block(r, 0, 0, 0, out, 12, 768, mask);
  if (rc) {
    for (size_t i = 0; i < sizeof out; i++)
      CHECK(((uint8_t *)out)[i] == 0x43);
    for (unsigned i = 0; i < 4096; i++)
      CHECK(mask[i] == 0x43);
  } else
    for (unsigned i = 0; i < 4096; i++) {
      CHECK(mask[i]);
      double d2 = 0;
      for (unsigned k = 0; k < (joint ? 3u : 1u); k++) {
        double d = (double)source[i][k] - out[i][k];
        d2 += d * d;
      }
      CHECK(d2 <= .25 * .25);
    }
  if (!rc) {
    float roi[16];
    uint8_t vm[16];
    memset(roi, 0x43, sizeof roi);
    memset(vm, 0x43, sizeof vm);
    rc = sfc_read_region(r, 0, 0, 0, 4, 4, roi, 4, 16, vm);
    if (rc) {
      for (size_t j = 0; j < sizeof roi; j++)
        CHECK(((uint8_t *)roi)[j] == 0x43);
      for (unsigned j = 0; j < 16; j++)
        CHECK(vm[j] == 0x43);
    } else
      for (unsigned i = 0; i < 16; i++)
        CHECK(vm[i] &&
              fabs((double)roi[i] - source[(i / 4) * 64 + i % 4][0]) <= .25);
  }
  if (!rc) {
    sfc_cache *cache = NULL;
    rc = sfc_cache_create(r, 65536, &cache);
    if (!rc) {
      float pixel[3];
      uint8_t vm = 0x43;
      memset(pixel, 0x43, sizeof(pixel));
      rc = joint
               ? sfc_cache_read_xyz_region(cache, 0, 0, 0, 1, 1, pixel, 12, 12,
                                           &vm)
               : sfc_cache_read_region(cache, 0, 0, 0, 1, 1, pixel, 4, 4, &vm);
      if (rc) {
        CHECK(vm == 0x43);
        for (size_t j = 0; j < sizeof(pixel); j++)
          CHECK(((uint8_t *)pixel)[j] == 0x43);
        sfc_cache_stats stats;
        CHECK(!sfc_cache_get_stats(cache, &stats) && !stats.resident_tiles);
      } else
        CHECK(vm == 1);
      sfc_cache_destroy(cache);
    } else
      CHECK(!cache);
  }
  sfc_close(r);
  return rc;
}
int main(void) {
  char dir[] = "/tmp/sfc-failures-XXXXXX", path[160];
  CHECK(mkdtemp(dir));
  snprintf(path, sizeof path, "%s/file.sfc", dir);
  for (unsigned i = 0; i < 4096; i++)
    for (unsigned k = 0; k < 3; k++)
      source[i][k] = (float)(10000 + k * 1000 + (i % 64) * .13 +
                             (i / 64) * .21 + sin(i * .1));
  unsigned checked = 0;
  for (int joint = 0; joint < 2; joint++) {
    for (unsigned mode = 0; mode < 2; mode++)
      for (long fail = 0; fail < 256; fail++) {
        faults = 0;
        alloc_budget = mode == 0 ? fail : -1;
        io_budget = mode == 1 ? fail : -1;
        int rc = write_file(path, joint);
        alloc_budget = io_budget = -1;
        if (rc)
          CHECK(access(path, F_OK) != 0);
        else {
          CHECK(!read_file_test(path, joint));
          CHECK(!unlink(path));
        }
        checked++;
        if (!faults)
          break;
        CHECK(fail < 255);
      }
    CHECK(!write_file(path, joint));
    for (unsigned mode = 0; mode < 2; mode++)
      for (long fail = 0; fail < 256; fail++) {
        faults = 0;
        alloc_budget = mode == 0 ? fail : -1;
        read_budget = mode == 1 ? fail : -1;
        read_file_test(path, joint);
        alloc_budget = read_budget = -1;
        CHECK(!access(path, F_OK));
        checked++;
        if (!faults)
          break;
        CHECK(fail < 255);
      }
    short_reads = 1;
    CHECK(!read_file_test(path, joint));
    short_reads = 0;
    CHECK(!unlink(path));
  }
  CHECK(!rmdir(dir));
  printf("%u allocation/I/O failure cases and short preads passed\n", checked);
  return 0;
}
