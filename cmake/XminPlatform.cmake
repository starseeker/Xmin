include_guard(GLOBAL)

include(CheckIncludeFiles)
include(CheckCSourceCompiles)
include(CheckStructHasMember)
include(CheckSymbolExists)
include(CheckTypeSize)
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
  set(HAS_SHM ${XMIN_HAVE_MITSHM})
  set(MITSHM ${XMIN_HAVE_MITSHM})

  check_symbol_exists(getrandom "sys/random.h" XMIN_HAVE_GETRANDOM)
  check_symbol_exists(getentropy "unistd.h" XMIN_HAVE_GETENTROPY)
  check_symbol_exists(arc4random_buf "stdlib.h" XMIN_HAVE_ARC4RANDOM_BUF)
  check_symbol_exists(accept4 "sys/socket.h" XMIN_HAVE_ACCEPT4)
  check_symbol_exists(pipe2 "unistd.h" XMIN_HAVE_PIPE2)

  check_include_files("sys/un.h" HAVE_SYS_UN_H)
  check_symbol_exists(poll "poll.h" HAVE_POLL)
  check_symbol_exists(setitimer "sys/time.h" HAVE_SETITIMER)
  check_symbol_exists(sigprocmask "signal.h" HAVE_SIGPROCMASK)
  check_symbol_exists(getifaddrs "ifaddrs.h" HAVE_GETIFADDRS)
  check_symbol_exists(getpeereid "sys/types.h;unistd.h" HAVE_GETPEEREID)
  check_symbol_exists(getpeerucred "ucred.h" HAVE_GETPEERUCRED)
  check_symbol_exists(SO_PEERCRED "sys/types.h;sys/socket.h" HAVE_SO_PEERCRED)
  check_symbol_exists(SCM_RIGHTS "sys/types.h;sys/socket.h" HAVE_SCM_RIGHTS)
  check_symbol_exists(getaddrinfo "sys/types.h;sys/socket.h;netdb.h"
    HAVE_GETADDRINFO)
  check_symbol_exists(inet_ntop "arpa/inet.h" HAVE_INET_NTOP)
  check_symbol_exists(issetugid "unistd.h" HAVE_ISSETUGID)
  check_symbol_exists(getresuid "sys/types.h;unistd.h" HAVE_GETRESUID)
  check_c_source_compiles("#include <time.h>
    int main(void) {
      struct timespec value;
      return clock_gettime(CLOCK_MONOTONIC, &value);
    }" MONOTONIC_CLOCK)

  set(CMAKE_EXTRA_INCLUDE_FILES "sys/types.h;sys/socket.h")
  check_type_size("socklen_t" XMIN_SIZEOF_SOCKLEN_T LANGUAGE C)
  check_type_size("struct sockaddr_storage"
    XMIN_SIZEOF_SOCKADDR_STORAGE LANGUAGE C)
  unset(CMAKE_EXTRA_INCLUDE_FILES)
  if(XMIN_SIZEOF_SOCKLEN_T)
    set(HAVE_SOCKLEN_T TRUE)
  else()
    set(HAVE_SOCKLEN_T FALSE)
  endif()
  if(XMIN_SIZEOF_SOCKADDR_STORAGE)
    set(HAVE_STRUCT_SOCKADDR_STORAGE TRUE)
  else()
    set(HAVE_STRUCT_SOCKADDR_STORAGE FALSE)
  endif()

  if(HAVE_GETPEEREID OR HAVE_GETPEERUCRED OR HAVE_SO_PEERCRED)
    set(NO_LOCAL_CLIENT_CRED FALSE)
  else()
    set(NO_LOCAL_CLIENT_CRED TRUE)
  endif()

  # The retained xtrans implementation is Unix-domain only by default.  File
  # descriptor passing is intentionally deferred until an extension needs it.
  set(UNIXCONN ${XMIN_HAVE_UNIX_SOCKETS})
  set(TCPCONN ${XMIN_ENABLE_TCP})
  set(LISTEN_UNIX ${XMIN_HAVE_UNIX_SOCKETS})
  set(LISTEN_LOCAL ${XMIN_HAVE_UNIX_SOCKETS})
  set(LISTEN_TCP ${XMIN_ENABLE_TCP})
  if(XMIN_ENABLE_TCP)
    set(XMIN_LOCAL_ONLY_XTRANS 0)
  else()
    set(XMIN_LOCAL_ONLY_XTRANS 1)
  endif()
  set(XTRANS_SEND_FDS FALSE)

  check_symbol_exists(reallocarray "stdlib.h" HAVE_REALLOCARRAY)
  check_symbol_exists(strcasecmp "strings.h" HAVE_STRCASECMP)
  check_symbol_exists(strncasecmp "strings.h" HAVE_STRNCASECMP)
  check_symbol_exists(strcasestr "string.h" HAVE_STRCASESTR)
  check_symbol_exists(strlcat "string.h" HAVE_STRLCAT)
  check_symbol_exists(strlcpy "string.h" HAVE_STRLCPY)
  check_symbol_exists(strndup "string.h" HAVE_STRNDUP)
  check_symbol_exists(timingsafe_memcmp "string.h" HAVE_TIMINGSAFE_MEMCMP)
  check_symbol_exists(vasprintf "stdio.h" HAVE_VASPRINTF)
  check_symbol_exists(fmemopen "stdio.h" HAVE_FMEMOPEN)

  check_symbol_exists(alarm "unistd.h" HAVE_ALARM)
  check_symbol_exists(getpagesize "unistd.h" HAVE_GETPAGESIZE)
  check_symbol_exists(gettimeofday "sys/time.h" HAVE_GETTIMEOFDAY)
  check_symbol_exists(mmap "sys/mman.h" HAVE_MMAP)
  check_symbol_exists(mprotect "sys/mman.h" HAVE_MPROTECT)
  check_symbol_exists(posix_memalign "stdlib.h" HAVE_POSIX_MEMALIGN)
  check_symbol_exists(sigaction "signal.h" HAVE_SIGACTION)
  check_include_files("sys/mman.h" HAVE_SYS_MMAN_H)
  check_include_files("unistd.h" HAVE_UNISTD_H)
  check_include_files("pthread.h" HAVE_PTHREADS)
  check_symbol_exists(__builtin_clz "" HAVE_BUILTIN_CLZ)
  set(xmin_saved_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
  if(NOT WIN32)
    list(APPEND CMAKE_REQUIRED_LIBRARIES m)
  endif()
  check_symbol_exists(cbrt "math.h" HAVE_CBRT)
  set(CMAKE_REQUIRED_LIBRARIES "${xmin_saved_required_libraries}")
  check_type_size("long" SIZEOF_LONG LANGUAGE C)

  check_struct_has_member(fd_set fds_bits
    "sys/types.h;sys/select.h" XMIN_FD_SET_HAS_FDS_BITS LANGUAGE C)
  if(XMIN_FD_SET_HAS_FDS_BITS)
    set(USE_FDS_BITS "fds_bits")
  else()
    check_struct_has_member(fd_set __fds_bits
      "sys/types.h;sys/select.h" XMIN_FD_SET_HAS___FDS_BITS LANGUAGE C)
    if(XMIN_FD_SET_HAS___FDS_BITS)
      set(USE_FDS_BITS "__fds_bits")
    else()
      message(FATAL_ERROR "Xmin cannot determine the fd_set bit-array member.")
    endif()
  endif()

  test_big_endian(XMIN_IS_BIG_ENDIAN)
  set(WORDS_BIGENDIAN ${XMIN_IS_BIG_ENDIAN})
  find_package(Threads REQUIRED)

  foreach(variable IN ITEMS
      XMIN_HAVE_UNIX_SOCKETS
      XMIN_HAVE_SYSV_SHM_HEADERS
      XMIN_HAVE_SHMAT
      XMIN_HAVE_MITSHM
      HAS_SHM
      MITSHM
      XMIN_HAVE_GETRANDOM
      XMIN_HAVE_GETENTROPY
      XMIN_HAVE_ARC4RANDOM_BUF
      XMIN_HAVE_ACCEPT4
      XMIN_HAVE_PIPE2
      XMIN_IS_BIG_ENDIAN
      HAVE_SYS_UN_H
      HAVE_POLL
      HAVE_SETITIMER
      HAVE_SIGPROCMASK
      HAVE_GETIFADDRS
      HAVE_GETPEEREID
      HAVE_GETPEERUCRED
      HAVE_SO_PEERCRED
      HAVE_SCM_RIGHTS
      HAVE_GETADDRINFO
      HAVE_INET_NTOP
      HAVE_ISSETUGID
      HAVE_GETRESUID
      MONOTONIC_CLOCK
      HAVE_SOCKLEN_T
      HAVE_STRUCT_SOCKADDR_STORAGE
      NO_LOCAL_CLIENT_CRED
      UNIXCONN
      TCPCONN
      LISTEN_UNIX
      LISTEN_LOCAL
      LISTEN_TCP
      XMIN_LOCAL_ONLY_XTRANS
      XTRANS_SEND_FDS
      HAVE_REALLOCARRAY
      HAVE_STRCASECMP
      HAVE_STRNCASECMP
      HAVE_STRCASESTR
      HAVE_STRLCAT
      HAVE_STRLCPY
      HAVE_STRNDUP
      HAVE_TIMINGSAFE_MEMCMP
      HAVE_VASPRINTF
      HAVE_FMEMOPEN
      HAVE_ALARM
      HAVE_GETPAGESIZE
      HAVE_GETTIMEOFDAY
      HAVE_MMAP
      HAVE_MPROTECT
      HAVE_POSIX_MEMALIGN
      HAVE_SIGACTION
      HAVE_SYS_MMAN_H
      HAVE_UNISTD_H
      HAVE_PTHREADS
      HAVE_BUILTIN_CLZ
      HAVE_CBRT
      SIZEOF_LONG
      WORDS_BIGENDIAN
      USE_FDS_BITS)
    set(${variable} "${${variable}}" PARENT_SCOPE)
  endforeach()
endfunction()
