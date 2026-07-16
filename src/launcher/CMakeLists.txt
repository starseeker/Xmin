# Private-cookie generation and Xauthority serialization are isolated so their
# invariants can be tested without starting a process tree.
xmin_add_component(launcher
  TYPE STATIC
  SOURCES xmin_authority.c
  PRIVATE_DEFINITIONS _POSIX_C_SOURCE=200809L
  PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
)

add_executable(xmin_run main.c)
add_executable(Xmin::run ALIAS xmin_run)
set_target_properties(xmin_run PROPERTIES
  OUTPUT_NAME xmin-run
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  C_EXTENSIONS NO
)
target_compile_definitions(xmin_run PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(xmin_run PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_BINARY_DIR}/generated"
)
target_link_libraries(xmin_run PRIVATE Xmin::launcher)
xmin_enable_warnings(xmin_run)

if(XMIN_ENABLE_INSTALL)
  install(
    TARGETS xmin_run
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
  )
endif()
