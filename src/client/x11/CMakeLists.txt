find_package(Threads REQUIRED)

add_library(xmin_qt_x11 SHARED
  client_connection.cpp
  xkb_minimal.cpp
  xcb_core.cpp
  xcb_helpers.cpp
  xcb_wire.cpp
)
add_library(Xmin::QtX11 ALIAS xmin_qt_x11)
set_target_properties(xmin_qt_x11 PROPERTIES
  EXPORT_NAME QtX11
  OUTPUT_NAME XminClient
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
)
target_compile_features(xmin_qt_x11 PRIVATE cxx_std_17)
target_compile_definitions(xmin_qt_x11 PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(xmin_qt_x11
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src/server/include"
)
target_link_libraries(xmin_qt_x11 PRIVATE Xmin::platform Threads::Threads)
xmin_enable_warnings(xmin_qt_x11)

if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD|NetBSD|OpenBSD)$")
  target_link_options(xmin_qt_x11 PRIVATE
    "LINKER:--version-script,${CMAKE_CURRENT_SOURCE_DIR}/libxminclient.map"
  )
  set_property(TARGET xmin_qt_x11 APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/libxminclient.map"
  )
endif()

if(XMIN_ENABLE_INSTALL)
  install(TARGETS xmin_qt_x11
    EXPORT XminTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      COMPONENT QtClientRuntime NAMELINK_COMPONENT QtClientDevelopment
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      COMPONENT QtClientDevelopment
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
      COMPONENT QtClientRuntime
  )
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/xcb"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT QtClientDevelopment
    FILES_MATCHING PATTERN "*.h"
  )
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/xkbcommon"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT QtClientDevelopment
    FILES_MATCHING PATTERN "*.h"
  )
endif()
