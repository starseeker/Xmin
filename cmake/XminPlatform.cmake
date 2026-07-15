include_guard(GLOBAL)

include(CheckIncludeFiles)
include(CheckSymbolExists)
include(TestBigEndian)

function(xmin_configure_platform)
  set(CMAKE_REQUIRED_QUIET TRUE)

  check_symbol_exists(AF_UNIX
    "sys/types.h;sys/socket.h" XMIN_HAVE_UNIX_SOCKETS)
  check_include_files(
    "sys/types.h;sys/ipc.h;sys/shm.h" XMIN_HAVE_SYSV_SHM_HEADERS)
  if(XMIN_HAVE_SYSV_SHM_HEADERS)
    check_symbol_exists(shmat
      "sys/types.h;sys/ipc.h;sys/shm.h" XMIN_HAVE_SHMAT)
  else()
    set(XMIN_HAVE_SHMAT FALSE)
  endif()

  set(have_mitshm FALSE)
  if(XMIN_HAVE_SYSV_SHM_HEADERS AND XMIN_HAVE_SHMAT)
    set(have_mitshm TRUE)
  endif()

  if(XMIN_ENABLE_MITSHM STREQUAL "ON" AND NOT have_mitshm)
    message(FATAL_ERROR
      "XMIN_ENABLE_MITSHM=ON, but SysV shared memory was not detected."
    )
  elseif(XMIN_ENABLE_MITSHM STREQUAL "OFF")
    set(XMIN_HAVE_MITSHM FALSE)
  else()
    set(XMIN_HAVE_MITSHM ${have_mitshm})
  endif()

  check_symbol_exists(getrandom "sys/random.h" XMIN_HAVE_GETRANDOM)
  check_symbol_exists(getentropy "unistd.h" XMIN_HAVE_GETENTROPY)
  check_symbol_exists(arc4random_buf "stdlib.h" XMIN_HAVE_ARC4RANDOM_BUF)
  check_symbol_exists(accept4 "sys/socket.h" XMIN_HAVE_ACCEPT4)
  check_symbol_exists(pipe2 "unistd.h" XMIN_HAVE_PIPE2)

  test_big_endian(XMIN_IS_BIG_ENDIAN)
  find_package(Threads REQUIRED)

  foreach(variable IN ITEMS
      XMIN_HAVE_UNIX_SOCKETS
      XMIN_HAVE_SYSV_SHM_HEADERS
      XMIN_HAVE_SHMAT
      XMIN_HAVE_MITSHM
      XMIN_HAVE_GETRANDOM
      XMIN_HAVE_GETENTROPY
      XMIN_HAVE_ARC4RANDOM_BUF
      XMIN_HAVE_ACCEPT4
      XMIN_HAVE_PIPE2
      XMIN_IS_BIG_ENDIAN)
    set(${variable} "${${variable}}" PARENT_SCOPE)
  endforeach()
endfunction()
