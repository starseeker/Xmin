add_executable(xmin_run main.cpp)
add_executable(Xmin::run ALIAS xmin_run)
set_target_properties(xmin_run PROPERTIES
  OUTPUT_NAME xmin-run
)
target_compile_definitions(xmin_run PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_features(xmin_run PRIVATE cxx_std_17)
target_link_libraries(xmin_run PRIVATE Xmin::platform)
xmin_enable_warnings(xmin_run)

if(XMIN_ENABLE_INSTALL)
  install(
    TARGETS xmin_run
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
  )
endif()
