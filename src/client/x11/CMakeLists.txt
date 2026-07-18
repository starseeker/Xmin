find_package(Threads REQUIRED)

set(xmin_go_font_directory "${PROJECT_SOURCE_DIR}/third_party/fonts/go")
set(xmin_go_font_files
  "${xmin_go_font_directory}/Go-Regular.ttf"
  "${xmin_go_font_directory}/Go-Bold.ttf"
  "${xmin_go_font_directory}/Go-Italic.ttf"
  "${xmin_go_font_directory}/Go-Bold-Italic.ttf"
  "${xmin_go_font_directory}/Go-Mono.ttf"
  "${xmin_go_font_directory}/Go-Mono-Bold.ttf"
  "${xmin_go_font_directory}/Go-Mono-Italic.ttf"
  "${xmin_go_font_directory}/Go-Mono-Bold-Italic.ttf"
)

if(XMIN_BUILD_TOOLKIT_CLIENT)
  set(xmin_embedded_fonts
    "${PROJECT_BINARY_DIR}/generated/client/x11/embedded_go_fonts.cpp"
  )
  add_custom_command(
    OUTPUT "${xmin_embedded_fonts}"
    COMMAND "${CMAKE_COMMAND}"
      "-DXMIN_FONT_DIRECTORY=${xmin_go_font_directory}"
      "-DXMIN_FONT_OUTPUT=${xmin_embedded_fonts}"
      -P "${PROJECT_SOURCE_DIR}/cmake/EmbedGoFonts.cmake"
    DEPENDS
      "${PROJECT_SOURCE_DIR}/cmake/EmbedGoFonts.cmake"
      ${xmin_go_font_files}
    COMMENT "Embedding the pinned Go font faces"
    VERBATIM
  )

  add_library(xmin_font_engine STATIC
    font/font_engine.cpp
    font/struetype_impl.cpp
    "${xmin_embedded_fonts}"
  )
  add_library(Xmin::FontEngine ALIAS xmin_font_engine)
  set_target_properties(xmin_font_engine PROPERTIES
    POSITION_INDEPENDENT_CODE ON
  )
  target_compile_features(xmin_font_engine PRIVATE cxx_std_17)
  target_include_directories(xmin_font_engine
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/font"
    SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/struetype"
  )
  if(MSVC)
    set_source_files_properties(font/struetype_impl.cpp PROPERTIES
      COMPILE_OPTIONS /w
    )
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(font/struetype_impl.cpp PROPERTIES
      COMPILE_OPTIONS -w
    )
  endif()
  xmin_enable_warnings(xmin_font_engine)
endif()

add_library(xmin_qt_x11 SHARED
  client_connection.cpp
  xkb_minimal.cpp
  xcb_core.cpp
  xcb_helpers.cpp
  xcb_render_glyphs.cpp
  xcb_wire.cpp
)
add_library(Xmin::ClientX11 ALIAS xmin_qt_x11)
set_target_properties(xmin_qt_x11 PROPERTIES
  EXPORT_NAME ClientX11
  OUTPUT_NAME XminClient
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
)
if(XMIN_BUILD_QT_CLIENT)
  add_library(xmin_qt_x11_sdk INTERFACE)
  add_library(Xmin::QtX11 ALIAS xmin_qt_x11_sdk)
  set_target_properties(xmin_qt_x11_sdk PROPERTIES EXPORT_NAME QtX11)
  target_link_libraries(xmin_qt_x11_sdk INTERFACE xmin_qt_x11)
endif()
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
if(XMIN_BUILD_QT_CLIENT OR XMIN_BUILD_TOOLKIT_CLIENT)
  target_sources(xmin_qt_x11 PRIVATE
    xlib_core.cpp
    xlib_events.cpp
    xlib_region.cpp
    xlib_minimal.cpp
  )
endif()
if(TARGET Xmin::FontEngine)
  target_sources(xmin_qt_x11 PRIVATE
    font/fontconfig_minimal.cpp
    font/render_font.cpp
    font/xft_minimal.cpp
    xcb_toolkit_wire.cpp
  )
  target_include_directories(xmin_qt_x11 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/font"
  )
  target_link_libraries(xmin_qt_x11 PRIVATE Xmin::FontEngine)
  add_library(xmin_toolkit_x11 INTERFACE)
  add_library(Xmin::ToolkitX11 ALIAS xmin_toolkit_x11)
  set_target_properties(xmin_toolkit_x11 PROPERTIES EXPORT_NAME ToolkitX11)
  target_include_directories(xmin_toolkit_x11 INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  )
  target_link_libraries(xmin_toolkit_x11 INTERFACE xmin_qt_x11)
endif()
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
  if(TARGET xmin_qt_x11_sdk)
    install(TARGETS xmin_qt_x11_sdk EXPORT XminTargets
      COMPONENT QtClientDevelopment
    )
  endif()
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
  if(XMIN_BUILD_QT_CLIENT OR XMIN_BUILD_TOOLKIT_CLIENT)
    install(FILES ${xmin_go_font_files}
      DESTINATION "${XMIN_INSTALL_FONTDIR}"
      COMPONENT ClientFontData
    )
  endif()
  if(XMIN_BUILD_QT_CLIENT OR TARGET xmin_toolkit_x11)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/X11"
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
      COMPONENT QtClientDevelopment
      FILES_MATCHING PATTERN "*.h"
    )
  endif()
  if(TARGET xmin_toolkit_x11)
    install(TARGETS xmin_toolkit_x11 EXPORT XminTargets
      COMPONENT ToolkitClientDevelopment
    )
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/fontconfig"
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
      COMPONENT ToolkitClientDevelopment
      FILES_MATCHING PATTERN "*.h"
    )
  endif()
endif()
