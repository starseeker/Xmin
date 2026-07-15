include_guard(GLOBAL)
include(CMakeParseArguments)

# Define one architectural component. Empty components are INTERFACE targets so
# the final link graph exists before source imports begin. Once SOURCES are added,
# the component becomes an OBJECT library by default; TYPE STATIC is also valid.
function(xmin_add_component name)
  set(options UPSTREAM)
  set(one_value_args TYPE)
  set(multi_value_args
    COMPILE_FEATURES
    PRIVATE_INCLUDE_DIRS
    PUBLIC_INCLUDE_DIRS
    SOURCES
    PRIVATE_DEFINITIONS
    PRIVATE_LIBRARIES
    PUBLIC_LIBRARIES
  )
  cmake_parse_arguments(XAC
    "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
  if(XAC_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "Unknown arguments for Xmin component '${name}': ${XAC_UNPARSED_ARGUMENTS}"
    )
  endif()

  set(target "xmin_${name}")
  if(TARGET ${target})
    message(FATAL_ERROR "Xmin component '${name}' is already defined.")
  endif()

  if(XAC_SOURCES)
    if(NOT XAC_TYPE)
      set(XAC_TYPE OBJECT)
    endif()
    if(NOT XAC_TYPE MATCHES "^(OBJECT|STATIC)$")
      message(FATAL_ERROR
        "Xmin component '${name}' TYPE must be OBJECT or STATIC."
      )
    endif()

    add_library(${target} ${XAC_TYPE})
    target_sources(${target} PRIVATE ${XAC_SOURCES})
    if(XAC_COMPILE_FEATURES)
      target_compile_features(${target} PUBLIC ${XAC_COMPILE_FEATURES})
    else()
      target_compile_features(${target} PUBLIC c_std_11)
    endif()
    target_include_directories(${target}
      PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>"
        ${XAC_PUBLIC_INCLUDE_DIRS}
      PRIVATE
        ${XAC_PRIVATE_INCLUDE_DIRS}
    )
    target_compile_definitions(${target} PRIVATE ${XAC_PRIVATE_DEFINITIONS})
    target_link_libraries(${target}
      PRIVATE ${XAC_PRIVATE_LIBRARIES}
      PUBLIC ${XAC_PUBLIC_LIBRARIES}
    )
    if(NOT XAC_UPSTREAM)
      xmin_enable_warnings(${target})
    endif()
  else()
    add_library(${target} INTERFACE)
    target_include_directories(${target}
      INTERFACE
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>"
        ${XAC_PUBLIC_INCLUDE_DIRS}
    )
    if(XAC_COMPILE_FEATURES)
      target_compile_features(${target} INTERFACE ${XAC_COMPILE_FEATURES})
    endif()
    target_compile_definitions(${target} INTERFACE ${XAC_PRIVATE_DEFINITIONS})
    target_link_libraries(${target} INTERFACE
      ${XAC_PRIVATE_LIBRARIES}
      ${XAC_PUBLIC_LIBRARIES}
    )
  endif()

  add_library("Xmin::${name}" ALIAS ${target})
endfunction()
