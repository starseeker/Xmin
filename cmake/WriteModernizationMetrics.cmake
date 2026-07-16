if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "SOURCE_DIR, BINARY_DIR, and OUTPUT are required")
endif()

function(xmin_count_sources directory prefix)
  file(GLOB_RECURSE source_files LIST_DIRECTORIES FALSE
    "${directory}/*.c"
    "${directory}/*.cc"
    "${directory}/*.cpp"
    "${directory}/*.h"
    "${directory}/*.hpp"
  )
  list(LENGTH source_files source_count)
  set(line_count 0)
  foreach(source IN LISTS source_files)
    file(READ "${source}" contents)
    string(REGEX MATCHALL "\n" line_breaks "${contents}")
    list(LENGTH line_breaks file_lines)
    math(EXPR line_count "${line_count} + ${file_lines}")
  endforeach()
  set(${prefix}_FILES "${source_count}" PARENT_SCOPE)
  set(${prefix}_LINES "${line_count}" PARENT_SCOPE)
endfunction()

xmin_count_sources("${SOURCE_DIR}/src" PROJECT)
xmin_count_sources("${SOURCE_DIR}/src/next" NEXT)
xmin_count_sources("${SOURCE_DIR}/third_party/xorg-server" XORG)
xmin_count_sources("${SOURCE_DIR}/third_party/osmesa" OSMESA)

set(translation_units "unavailable")
set(next_translation_units "unavailable")
if(EXISTS "${BINARY_DIR}/compile_commands.json")
  file(READ "${BINARY_DIR}/compile_commands.json" compile_commands)
  string(JSON translation_units LENGTH "${compile_commands}")
  set(next_translation_units 0)
  if(translation_units GREATER 0)
    math(EXPR command_last "${translation_units} - 1")
    foreach(index RANGE 0 ${command_last})
      string(JSON source_file GET "${compile_commands}" ${index} file)
      string(FIND "${source_file}" "/src/next/" next_position)
      if(NOT next_position EQUAL -1)
        math(EXPR next_translation_units "${next_translation_units} + 1")
      endif()
    endforeach()
  endif()
endif()

set(legacy_bytes "unavailable")
if(DEFINED LEGACY_BINARY AND EXISTS "${LEGACY_BINARY}")
  file(SIZE "${LEGACY_BINARY}" legacy_bytes)
endif()
set(next_bytes "unavailable")
if(DEFINED NEXT_BINARY AND EXISTS "${NEXT_BINARY}")
  file(SIZE "${NEXT_BINARY}" next_bytes)
endif()

file(READ "${SOURCE_DIR}/profiles/protocol-coverage.json" protocol_profile)
string(JSON core_slots GET "${protocol_profile}" summary core_opcode_slots)
string(JSON extensions GET "${protocol_profile}" summary extension_count)
string(JSON extension_requests GET "${protocol_profile}" summary extension_requests)

string(CONCAT report
  "Xmin modernization metrics\n"
  "project_source_files=${PROJECT_FILES}\n"
  "project_source_lines=${PROJECT_LINES}\n"
  "next_source_files=${NEXT_FILES}\n"
  "next_source_lines=${NEXT_LINES}\n"
  "xorg_source_files=${XORG_FILES}\n"
  "xorg_source_lines=${XORG_LINES}\n"
  "osmesa_source_files=${OSMESA_FILES}\n"
  "osmesa_source_lines=${OSMESA_LINES}\n"
  "translation_units=${translation_units}\n"
  "next_translation_units=${next_translation_units}\n"
  "legacy_binary_bytes=${legacy_bytes}\n"
  "next_binary_bytes=${next_bytes}\n"
  "core_opcode_slots=${core_slots}\n"
  "profile_extensions=${extensions}\n"
  "profile_extension_requests=${extension_requests}\n"
)
file(WRITE "${OUTPUT}" "${report}")
message("${report}")
