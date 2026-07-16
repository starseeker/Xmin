# Project-owned embedded-data adapters, cryptographic primitives, and small
# portability helpers.  SHA-1 is required by the RENDER glyph cache; keeping it
# here avoids Xorg's usual runtime dependency on a host crypto library.
xmin_add_component(support
  TYPE STATIC
  SOURCES xmin_sha1.c
  PUBLIC_LIBRARIES Xmin::xserver_headers
)
