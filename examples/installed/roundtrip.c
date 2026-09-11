#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <surfcomp.h>
int main(void) {
  float source[SFC_SAMPLES][3], restored[SFC_SAMPLES][3];
  for (unsigned i = 0; i < SFC_SAMPLES; i++) {
    source[i][0] = 10000 + (float)(i % 64) * .5f;
    source[i][1] = 20000 + (float)(i / 64) * .75f;
    source[i][2] = 30000 + sinf((float)i * .01f);
  }
  uint8_t *encoded = NULL;
  size_t size = 0;
  int rc = sfc_encode_xyz_block(source, sizeof source[0], sizeof source[0] * 64,
                                NULL, .25, &encoded, &size);
  if (!rc)
    rc = sfc_decode_xyz_block(encoded, size, restored, sizeof restored[0],
                              sizeof restored[0] * 64, NULL);
  free(encoded);
  if (rc)
    return 1;
  for (unsigned i = 0; i < SFC_SAMPLES; i++) {
    double error = 0;
    for (unsigned k = 0; k < 3; k++) {
      double d = (double)source[i][k] - restored[i][k];
      error += d * d;
    }
    if (error > .25 * .25)
      return 2;
  }
  printf("surfcomp %s: %zu-byte XYZ patch within 0.25 voxel\n", sfc_version(),
         size);
  return strcmp(sfc_version(), SFC_VERSION_STRING) != 0;
}
