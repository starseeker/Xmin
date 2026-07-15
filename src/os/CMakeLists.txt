# Connections, I/O, authentication, logging, allocation, and portability glue.
set(xmin_libc_fallbacks)
if(NOT HAVE_REALLOCARRAY)
  list(APPEND xmin_libc_fallbacks
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/os/reallocarray.c")
endif()
if(NOT HAVE_STRCASECMP OR NOT HAVE_STRNCASECMP)
  list(APPEND xmin_libc_fallbacks
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/os/strcasecmp.c")
endif()
if(NOT HAVE_STRCASESTR)
  list(APPEND xmin_libc_fallbacks
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/os/strcasestr.c")
endif()
if(NOT HAVE_STRLCAT)
  list(APPEND xmin_libc_fallbacks
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/os/strlcat.c")
endif()
if(NOT HAVE_STRLCPY)
  list(APPEND xmin_libc_fallbacks
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/os/strlcpy.c")
endif()
if(NOT HAVE_STRNDUP)
  list(APPEND xmin_libc_fallbacks
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/os/strndup.c")
endif()
if(NOT HAVE_TIMINGSAFE_MEMCMP)
  list(APPEND xmin_libc_fallbacks
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/os/timingsafe_memcmp.c")
endif()

xmin_add_component(os
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_libc_fallbacks}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PUBLIC_LIBRARIES Xmin::xserver_headers
)
