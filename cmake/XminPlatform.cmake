include_guard(GLOBAL)

include(CheckIncludeFiles)
include(CheckSymbolExists)
include(CheckTypeSize)
include(TestBigEndian)

# Probe only facilities used by Xmin itself or by its retained pixman build.
# Every result exported from this function feeds a generated configuration
# header; there are deliberately no compatibility variables for Xorg/xtrans.
function(xmin_configure_platform)
  set(CMAKE_REQUIRED_QUIET TRUE)

  check_symbol_exists(AF_UNIX
    "sys/types.h;sys/socket.h" XMIN_HAVE_UNIX_SOCKETS)
  if(NOT XMIN_HAVE_UNIX_SOCKETS)
    message(FATAL_ERROR "Xmin requires Unix-domain sockets.")
  endif()

  check_include_files(
    "sys/types.h;sys/ipc.h;sys/shm.h" XMIN_HAVE_SYSV_SHM_HEADERS)
  if(XMIN_HAVE_SYSV_SHM_HEADERS)
    check_symbol_exists(shmat
      "sys/types.h;sys/ipc.h;sys/shm.h" XMIN_HAVE_SHMAT)
  else()
    set(XMIN_HAVE_SHMAT FALSE)
  endif()

  set(xmin_platform_has_mitshm FALSE)
  if(XMIN_HAVE_SYSV_SHM_HEADERS AND XMIN_HAVE_SHMAT)
    set(xmin_platform_has_mitshm TRUE)
  endif()
  if(XMIN_ENABLE_MITSHM STREQUAL "ON" AND NOT xmin_platform_has_mitshm)
    message(FATAL_ERROR
      "XMIN_ENABLE_MITSHM=ON, but SysV shared memory is unavailable.")
  elseif(XMIN_ENABLE_MITSHM STREQUAL "OFF")
    set(XMIN_HAVE_MITSHM FALSE)
  else()
    set(XMIN_HAVE_MITSHM ${xmin_platform_has_mitshm})
  endif()

  check_symbol_exists(getrandom "sys/random.h" XMIN_HAVE_GETRANDOM)
  check_symbol_exists(getentropy "unistd.h" XMIN_HAVE_GETENTROPY)
  check_symbol_exists(arc4random_buf "stdlib.h" XMIN_HAVE_ARC4RANDOM_BUF)
  check_symbol_exists(SCM_RIGHTS
    "sys/types.h;sys/socket.h" XMIN_HAVE_SCM_RIGHTS)

  # Configuration consumed by the retained portable pixman source set.
  check_symbol_exists(__builtin_clz "" HAVE_BUILTIN_CLZ)
  check_type_size("long" SIZEOF_LONG LANGUAGE C)
  test_big_endian(XMIN_IS_BIG_ENDIAN)
  set(WORDS_BIGENDIAN ${XMIN_IS_BIG_ENDIAN})

  foreach(variable IN ITEMS
      XMIN_HAVE_UNIX_SOCKETS
      XMIN_HAVE_MITSHM
      XMIN_HAVE_GETRANDOM
      XMIN_HAVE_GETENTROPY
      XMIN_HAVE_ARC4RANDOM_BUF
      XMIN_HAVE_SCM_RIGHTS
      XMIN_IS_BIG_ENDIAN
      HAVE_BUILTIN_CLZ
      SIZEOF_LONG
      WORDS_BIGENDIAN)
    set(${variable} "${${variable}}" PARENT_SCOPE)
  endforeach()
endfunction()
