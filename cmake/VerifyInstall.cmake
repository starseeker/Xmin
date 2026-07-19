foreach(required IN ITEMS
    BINARY_DIR STAGE_DIR BINDIR LIBDIR MANDIR DOCDIR VERSION
    HAVE_CLIENT_FONTS HAVE_SHELL FONTDIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${STAGE_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BINARY_DIR}" --prefix "${STAGE_DIR}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR
    "Staged installation failed:\n${install_output}${install_error}")
endif()

if(HAVE_CLIENT_FONTS)
  foreach(face IN ITEMS
      Go-Regular.ttf
      Go-Bold.ttf
      Go-Italic.ttf
      Go-Bold-Italic.ttf
      Go-Mono.ttf
      Go-Mono-Bold.ttf
      Go-Mono-Italic.ttf
      Go-Mono-Bold-Italic.ttf)
    if(NOT EXISTS "${STAGE_DIR}/${FONTDIR}/${face}")
      message(FATAL_ERROR "Installed client font is missing: ${face}")
    endif()
  endforeach()
endif()

set(server "${STAGE_DIR}/${BINDIR}/Xmin")
set(controller "${STAGE_DIR}/${BINDIR}/xminctl")
foreach(program IN ITEMS "${server}" "${controller}")
  if(NOT EXISTS "${program}")
    message(FATAL_ERROR "Installed program is missing: ${program}")
  endif()
endforeach()

foreach(program IN ITEMS server controller)
  execute_process(
    COMMAND "${${program}}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
  )
  if(NOT version_result EQUAL 0 OR
     NOT "${version_output}${version_error}" MATCHES "${VERSION}")
    message(FATAL_ERROR
      "Installed ${program} failed its version check: "
      "${version_output}${version_error}")
  endif()
endforeach()

set(audit_files "${server}" "${controller}")
if(HAVE_SHELL)
  set(shell "${STAGE_DIR}/${BINDIR}/xmin-sh")
  if(NOT EXISTS "${shell}" OR
      NOT EXISTS "${STAGE_DIR}/${MANDIR}/man1/xmin-sh.1" OR
      NOT EXISTS "${STAGE_DIR}/${DOCDIR}/licenses/dash/COPYING")
    message(FATAL_ERROR "Installed bundled shell or its notices are missing")
  endif()
  execute_process(
    COMMAND "${shell}" -c
      "test \"$(kill -l 15)\" = TERM && trap 'exit 0' TERM && kill -s TERM $$; exit 1"
    RESULT_VARIABLE shell_result
    OUTPUT_VARIABLE shell_output
    ERROR_VARIABLE shell_error
  )
  if(NOT shell_result EQUAL 0)
    message(FATAL_ERROR
      "Installed bundled shell failed (${shell_result}): "
      "${shell_output}${shell_error}")
  endif()
  list(APPEND audit_files "${shell}")
endif()
if(HAVE_LAUNCHER)
  set(launcher "${STAGE_DIR}/${BINDIR}/xmin-run")
  if(NOT EXISTS "${launcher}")
    message(FATAL_ERROR "Installed launcher is missing: ${launcher}")
  endif()
  execute_process(
    COMMAND "${launcher}" --server "${server}" -- "${TEST_CHILD}"
    RESULT_VARIABLE launch_result
    OUTPUT_VARIABLE launch_output
    ERROR_VARIABLE launch_error
    TIMEOUT 20
  )
  if(NOT launch_result EQUAL 0)
    message(FATAL_ERROR
      "Installed relocation workflow failed (${launch_result}):\n"
      "${launch_output}${launch_error}")
  endif()
  list(APPEND audit_files "${launcher}")
endif()

if(HAVE_GL)
  file(GLOB gl_libraries
    "${STAGE_DIR}/${LIBDIR}/xmin/*GL*.so*"
    "${STAGE_DIR}/${LIBDIR}/xmin/*GL*.dylib*"
  )
  if(NOT gl_libraries)
    message(FATAL_ERROR "Installed client GL library is missing")
  endif()
  if(NOT EXISTS "${STAGE_DIR}/include/GL/glx.h" OR
     NOT EXISTS "${STAGE_DIR}/include/OSMesa/osmesa.h")
    message(FATAL_ERROR "Installed client GL development headers are missing")
  endif()
  list(APPEND audit_files ${gl_libraries})
endif()
list(REMOVE_DUPLICATES audit_files)

find_program(dependency_tool NAMES ldd otool)
if(NOT dependency_tool)
  message(FATAL_ERROR "Neither ldd nor otool is available for dependency audit")
endif()
get_filename_component(dependency_tool_name "${dependency_tool}" NAME)
foreach(binary IN LISTS audit_files)
  if(dependency_tool_name STREQUAL "otool")
    set(dependency_args -L "${binary}")
  else()
    set(dependency_args "${binary}")
  endif()
  execute_process(
    COMMAND "${dependency_tool}" ${dependency_args}
    RESULT_VARIABLE dependency_result
    OUTPUT_VARIABLE dependencies
    ERROR_VARIABLE dependency_error
  )
  if(NOT dependency_result EQUAL 0)
    message(FATAL_ERROR
      "Dependency inspection failed for ${binary}: ${dependency_error}")
  endif()
  # FreeBSD ldd and otool print the inspected file as the first line. Do not
  # mistake libGL's own install name for a dependency on host GL.
  string(REGEX REPLACE "^[^\n]*:\n" "" dependencies "${dependencies}")
  string(TOLOWER "${dependencies}" dependencies_lower)
  if(dependencies_lower MATCHES
      "lib(x11|xcb|xau|xfont|gl|egl|fontconfig|freetype|crypto|ssl|drm)")
    message(FATAL_ERROR
      "Forbidden runtime dependency in ${binary}:\n${dependencies}")
  endif()
endforeach()

message(STATUS "Installed relocation and dependency audit passed: ${STAGE_DIR}")
