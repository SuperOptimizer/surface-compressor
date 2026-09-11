#include <cstring>
#include <surfcomp.h>
int main() {
  return std::strcmp(sfc_version(), SFC_VERSION_STRING) ||
         sfc_format_version() != SFC_FORMAT_VERSION;
}
