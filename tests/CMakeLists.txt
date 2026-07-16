add_test(NAME xmin.version COMMAND Xmin::server --version)
set_tests_properties(xmin.version PROPERTIES
  PASS_REGULAR_EXPRESSION "Xmin ${PROJECT_VERSION}"
)

add_test(NAME xmin.help COMMAND Xmin::server --help)
set_tests_properties(xmin.help PROPERTIES
  PASS_REGULAR_EXPRESSION "use: X \\[:<display>\\] \\[option\\]"
)

if(TARGET Xmin::next_server)
  add_test(NAME xmin.next-version COMMAND Xmin::next_server --version)
  set_tests_properties(xmin.next-version PROPERTIES
    PASS_REGULAR_EXPRESSION "Xmin-next ${PROJECT_VERSION}"
  )

  add_test(NAME xmin.next-help COMMAND Xmin::next_server --help)
  set_tests_properties(xmin.next-help PROPERTIES
    PASS_REGULAR_EXPRESSION "usage: Xmin-next --client-fd"
  )

  add_executable(xmin_next_foundation next_foundation.cpp)
  target_compile_features(xmin_next_foundation PRIVATE cxx_std_17)
  target_link_libraries(xmin_next_foundation PRIVATE Xmin::next_core)
  xmin_enable_warnings(xmin_next_foundation)
  add_test(NAME xmin.next-foundation COMMAND xmin_next_foundation)

  add_executable(xmin_next_handshake integration/next_handshake.cpp)
  target_compile_features(xmin_next_handshake PRIVATE cxx_std_17)
  target_compile_definitions(xmin_next_handshake PRIVATE
    _POSIX_C_SOURCE=200809L
  )
  xmin_enable_warnings(xmin_next_handshake)
  add_test(NAME xmin.next-handshake
    COMMAND xmin_next_handshake $<TARGET_FILE:xmin_next>
  )
  set_tests_properties(xmin.next-handshake PROPERTIES TIMEOUT 20)
endif()

add_executable(xmin_server_handshake integration/server_handshake.c)
target_compile_features(xmin_server_handshake PRIVATE c_std_11)
target_include_directories(xmin_server_handshake PRIVATE
  "${PROJECT_BINARY_DIR}/generated"
)
xmin_enable_warnings(xmin_server_handshake)
add_test(NAME xmin.server-handshake
  COMMAND xmin_server_handshake $<TARGET_FILE:xmin_server>
)
set_tests_properties(xmin.server-handshake PROPERTIES TIMEOUT 20)

add_executable(xmin_server_stale_lock integration/server_stale_lock.c)
target_compile_features(xmin_server_stale_lock PRIVATE c_std_11)
xmin_enable_warnings(xmin_server_stale_lock)
add_test(NAME xmin.server-stale-lock
  COMMAND xmin_server_stale_lock $<TARGET_FILE:xmin_server>
)
set_tests_properties(xmin.server-stale-lock PROPERTIES TIMEOUT 20)

if(TARGET Xmin::run)
  add_executable(xmin_launcher_child integration/launcher_child.c)
  target_compile_features(xmin_launcher_child PRIVATE c_std_11)
  target_link_libraries(xmin_launcher_child PRIVATE Xmin::xau)
  xmin_enable_warnings(xmin_launcher_child)
  add_test(NAME xmin.launcher-auth
    COMMAND Xmin::run
      --server $<TARGET_FILE:xmin_server>
      --screen 320x240x24
      -- $<TARGET_FILE:xmin_launcher_child>
  )
  set_tests_properties(xmin.launcher-auth PROPERTIES TIMEOUT 20)

  add_executable(xmin_launcher_concurrent integration/launcher_concurrent.c)
  target_compile_features(xmin_launcher_concurrent PRIVATE c_std_11)
  xmin_enable_warnings(xmin_launcher_concurrent)
  add_test(NAME xmin.launcher-concurrent
    COMMAND xmin_launcher_concurrent
      $<TARGET_FILE:xmin_run>
      $<TARGET_FILE:xmin_server>
      $<TARGET_FILE:xmin_launcher_child>
  )
  set_tests_properties(xmin.launcher-concurrent PROPERTIES TIMEOUT 30)

  add_executable(xmin_launcher_signal integration/launcher_signal.c)
  target_compile_features(xmin_launcher_signal PRIVATE c_std_11)
  xmin_enable_warnings(xmin_launcher_signal)
  add_test(NAME xmin.launcher-signal-cleanup
    COMMAND xmin_launcher_signal
      $<TARGET_FILE:xmin_run>
      $<TARGET_FILE:xmin_server>
  )
  set_tests_properties(xmin.launcher-signal-cleanup PROPERTIES TIMEOUT 30)

  if(TARGET Xmin::ctl)
    add_executable(xminctl_target
      integration/xminctl_target.c
      "${PROJECT_SOURCE_DIR}/src/control/xmin_xcb_connect.c"
    )
    target_compile_features(xminctl_target PRIVATE c_std_11)
    target_include_directories(xminctl_target PRIVATE
      "${PROJECT_SOURCE_DIR}/src/control"
    )
    target_compile_definitions(xminctl_target PRIVATE _POSIX_C_SOURCE=200809L)
    target_link_libraries(xminctl_target PRIVATE
      Xmin::xcb
      Xmin::xau
      Xmin::xorgproto
    )
    xmin_enable_warnings(xminctl_target)

    add_executable(xminctl_integration integration/xminctl_integration.c)
    target_compile_features(xminctl_integration PRIVATE c_std_11)
    target_compile_definitions(xminctl_integration PRIVATE
      _POSIX_C_SOURCE=200809L
    )
    xmin_enable_warnings(xminctl_integration)
    add_test(NAME xmin.control-automation
      COMMAND Xmin::run
        --server $<TARGET_FILE:xmin_server>
        --screen 160x120x24
        -- $<TARGET_FILE:xminctl_integration>
           $<TARGET_FILE:xminctl>
           $<TARGET_FILE:xminctl_target>
    )
    set_tests_properties(xmin.control-automation PROPERTIES TIMEOUT 30)
  endif()

  # XCB is the transport used by Qt's xcb platform plugin. Keep this client
  # test optional and entirely outside the installed target graph.
  find_path(XMIN_TEST_XCB_INCLUDE_DIR xcb/xcb.h)
  find_library(XMIN_TEST_XCB_LIBRARY NAMES xcb)
  if(XMIN_TEST_XCB_INCLUDE_DIR AND XMIN_TEST_XCB_LIBRARY)
    add_executable(xmin_xcb_raster integration/xcb_raster.c)
    target_compile_features(xmin_xcb_raster PRIVATE c_std_11)
    target_include_directories(xmin_xcb_raster
      PRIVATE "${PROJECT_BINARY_DIR}/generated"
      SYSTEM PRIVATE "${XMIN_TEST_XCB_INCLUDE_DIR}"
    )
    target_link_libraries(xmin_xcb_raster PRIVATE "${XMIN_TEST_XCB_LIBRARY}")
    xmin_enable_warnings(xmin_xcb_raster)
    add_test(NAME xmin.xcb-raster
      COMMAND Xmin::run
        --server $<TARGET_FILE:xmin_server>
        --screen 96x80x24
        -- $<TARGET_FILE:xmin_xcb_raster>
    )
    set_tests_properties(xmin.xcb-raster PROPERTIES TIMEOUT 20)

    add_executable(xmin_xcb_core integration/xcb_core.c)
    target_compile_features(xmin_xcb_core PRIVATE c_std_11)
    target_include_directories(xmin_xcb_core
      PRIVATE "${PROJECT_BINARY_DIR}/generated"
      SYSTEM PRIVATE "${XMIN_TEST_XCB_INCLUDE_DIR}"
    )
    target_link_libraries(xmin_xcb_core PRIVATE "${XMIN_TEST_XCB_LIBRARY}")
    xmin_enable_warnings(xmin_xcb_core)
    add_test(NAME xmin.xcb-core
      COMMAND Xmin::run
        --server $<TARGET_FILE:xmin_server>
        --screen 96x80x24
        -- $<TARGET_FILE:xmin_xcb_core>
    )
    set_tests_properties(xmin.xcb-core PROPERTIES TIMEOUT 20)

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(XMIN_TEST_XCB_EXTENSIONS QUIET IMPORTED_TARGET
        xcb-render
        xcb-randr
        xcb-xfixes
        xcb-shape
        xcb-sync
        xcb-xinput
        xcb-xkb
        xcb-xtest
        xcb-composite
        xcb-damage
        xcb-present
        xcb-shm
        xcb-dbe
        xcb-xinerama
        xcb-screensaver
      )
      if(TARGET PkgConfig::XMIN_TEST_XCB_EXTENSIONS)
        add_executable(xmin_xcb_extensions integration/xcb_extensions.c)
        target_compile_features(xmin_xcb_extensions PRIVATE c_std_11)
        target_include_directories(xmin_xcb_extensions PRIVATE
          "${PROJECT_BINARY_DIR}/generated"
        )
        target_link_libraries(xmin_xcb_extensions PRIVATE
          PkgConfig::XMIN_TEST_XCB_EXTENSIONS
          "${XMIN_TEST_XCB_LIBRARY}"
        )
        xmin_enable_warnings(xmin_xcb_extensions)
        add_test(NAME xmin.xcb-extensions
          COMMAND Xmin::run
            --server $<TARGET_FILE:xmin_server>
            --screen 96x80x24
            -- $<TARGET_FILE:xmin_xcb_extensions>
        )
        set_tests_properties(xmin.xcb-extensions PROPERTIES TIMEOUT 20)
      endif()
    endif()
  endif()
endif()

if(TARGET Xmin::osmesa)
  add_executable(xmin_osmesa_context glx/osmesa_context.c)
  target_compile_features(xmin_osmesa_context PRIVATE c_std_11)
  target_include_directories(xmin_osmesa_context PRIVATE
    "${PROJECT_BINARY_DIR}/generated"
  )
  target_link_libraries(xmin_osmesa_context PRIVATE Xmin::osmesa)
  xmin_enable_warnings(xmin_osmesa_context)
  add_test(NAME xmin.osmesa-context COMMAND xmin_osmesa_context)

  add_executable(xmin_glx_render integration/glx_render.c)
  target_compile_features(xmin_glx_render PRIVATE c_std_11)
  target_link_libraries(xmin_glx_render PRIVATE Xmin::xorgproto)
  xmin_enable_warnings(xmin_glx_render)
  add_test(NAME xmin.glx-render
    COMMAND xmin_glx_render $<TARGET_FILE:xmin_server>
  )
  set_tests_properties(xmin.glx-render PROPERTIES TIMEOUT 20)

endif()

# The direct client library is a separate product from server-side indirect
# GLX. Its tests deliberately run against the clean server profile, where the
# GLX extension is absent.
if(TARGET Xmin::gl_client)
  add_executable(xmin_software_direct_glsl glx/software_direct_glsl.c)
  target_compile_features(xmin_software_direct_glsl PRIVATE c_std_11)
  target_link_libraries(xmin_software_direct_glsl PRIVATE Xmin::gl_client)
  xmin_enable_warnings(xmin_software_direct_glsl)
  add_test(NAME xmin.software-direct-glsl COMMAND xmin_software_direct_glsl)

  # Xlib is application-side and test-only. The shipped client bridge keeps
  # these references weak and does not acquire a host-X11 DT_NEEDED entry.
  find_path(XMIN_TEST_X11_INCLUDE_DIR X11/Xlib.h)
  find_library(XMIN_TEST_X11_LIBRARY NAMES X11)
  if(XMIN_TEST_X11_INCLUDE_DIR AND XMIN_TEST_X11_LIBRARY AND TARGET Xmin::run)
    add_executable(xmin_software_direct_window glx/software_direct_window.c)
    target_compile_features(xmin_software_direct_window PRIVATE c_std_11)
    target_include_directories(xmin_software_direct_window SYSTEM PRIVATE
      "${XMIN_TEST_X11_INCLUDE_DIR}"
    )
    target_link_libraries(xmin_software_direct_window PRIVATE
      Xmin::gl_client
      "${XMIN_TEST_X11_LIBRARY}"
    )
    xmin_enable_warnings(xmin_software_direct_window)
    add_test(NAME xmin.software-direct-window
      COMMAND Xmin::run
        --server $<TARGET_FILE:xmin_server>
        --screen 64x64x24
        -- $<TARGET_FILE:xmin_software_direct_window>
    )
    set_tests_properties(xmin.software-direct-window PROPERTIES TIMEOUT 20)
  endif()
endif()

# Toolkits are application-side, optional acceptance dependencies. Discover them
# only while building tests; none of these targets can enter Xmin's link graph.
set(XMIN_HAVE_QT5_TESTS OFF)
set(XMIN_HAVE_QT6_TESTS OFF)
set(XMIN_HAVE_GTK3_TESTS OFF)
if(TARGET Xmin::run)
  function(xmin_add_qt_acceptance_tests qt_major qt_gui_target)
    string(TOLOWER "qt${qt_major}" qt_suffix)

    add_executable(xmin_${qt_suffix}_raster qt/raster.cpp)
    target_compile_features(xmin_${qt_suffix}_raster PRIVATE cxx_std_17)
    target_compile_definitions(xmin_${qt_suffix}_raster PRIVATE
      XMIN_QT_MAJOR=${qt_major}
    )
    target_link_libraries(xmin_${qt_suffix}_raster PRIVATE ${qt_gui_target})
    xmin_enable_warnings(xmin_${qt_suffix}_raster)
    add_test(NAME xmin.${qt_suffix}-raster
      COMMAND Xmin::run
        --server $<TARGET_FILE:xmin_server>
        --screen 320x240x24
        -- $<TARGET_FILE:xmin_${qt_suffix}_raster>
    )
    set_tests_properties(xmin.${qt_suffix}-raster PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=xcb"
      TIMEOUT 30
    )

    if(TARGET Xmin::gl_client)
      add_executable(xmin_${qt_suffix}_opengl qt/opengl.cpp)
      target_compile_features(xmin_${qt_suffix}_opengl PRIVATE cxx_std_17)
      target_compile_definitions(xmin_${qt_suffix}_opengl PRIVATE
        XMIN_QT_MAJOR=${qt_major}
      )
      target_link_libraries(xmin_${qt_suffix}_opengl PRIVATE
        Xmin::gl_client
        ${qt_gui_target}
      )
      # Ubuntu's toolchain enables --as-needed by default.  Qt resolves its
      # OpenGL entry points at runtime, so no static undefined reference keeps
      # Xmin's libGL on the executable's DT_NEEDED list without this override.
      if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(xmin_${qt_suffix}_opengl PRIVATE
          "LINKER:--no-as-needed"
        )
      endif()
      xmin_enable_warnings(xmin_${qt_suffix}_opengl)
      add_test(NAME xmin.${qt_suffix}-opengl
        COMMAND Xmin::run
          --server $<TARGET_FILE:xmin_server>
          --screen 320x240x24
          -- $<TARGET_FILE:xmin_${qt_suffix}_opengl>
      )
      set_tests_properties(xmin.${qt_suffix}-opengl PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=xcb"
        TIMEOUT 30
      )
    endif()
  endfunction()

  find_package(Qt6 QUIET COMPONENTS Gui)
  if(TARGET Qt6::Gui)
    xmin_add_qt_acceptance_tests(6 Qt6::Gui)
    set(XMIN_HAVE_QT6_TESTS ON)
  endif()

  find_package(Qt5 QUIET COMPONENTS Gui)
  if(TARGET Qt5::Gui)
    xmin_add_qt_acceptance_tests(5 Qt5::Gui)
    set(XMIN_HAVE_QT5_TESTS ON)
  endif()

  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(XMIN_TEST_GTK3 QUIET IMPORTED_TARGET gtk+-3.0)
    if(TARGET PkgConfig::XMIN_TEST_GTK3)
      add_executable(xmin_gtk3_raster gtk/raster.c)
      target_compile_features(xmin_gtk3_raster PRIVATE c_std_11)
      target_link_libraries(xmin_gtk3_raster PRIVATE
        PkgConfig::XMIN_TEST_GTK3
      )
      xmin_enable_warnings(xmin_gtk3_raster)
      add_test(NAME xmin.gtk3-raster
        COMMAND Xmin::run
          --server $<TARGET_FILE:xmin_server>
          --screen 320x240x24
          -- $<TARGET_FILE:xmin_gtk3_raster>
      )
      set_tests_properties(xmin.gtk3-raster PROPERTIES TIMEOUT 30)
      set(XMIN_HAVE_GTK3_TESTS ON)
    endif()
  endif()
endif()

if(XMIN_REQUIRE_TOOLKIT_TESTS)
  if(NOT TARGET Xmin::run)
    message(FATAL_ERROR
      "Toolkit acceptance tests require XMIN_BUILD_LAUNCHER=ON."
    )
  endif()
  if(NOT XMIN_HAVE_QT5_TESTS AND NOT XMIN_HAVE_QT6_TESTS)
    message(FATAL_ERROR
      "Toolkit acceptance was required, but neither Qt 5 Gui nor Qt 6 Gui was found."
    )
  endif()
  if(NOT XMIN_HAVE_GTK3_TESTS)
    message(FATAL_ERROR
      "Toolkit acceptance was required, but GTK 3 development files were not found."
    )
  endif()
endif()

set(XMIN_HAVE_QT5_TESTS ${XMIN_HAVE_QT5_TESTS} PARENT_SCOPE)
set(XMIN_HAVE_QT6_TESTS ${XMIN_HAVE_QT6_TESTS} PARENT_SCOPE)
set(XMIN_HAVE_GTK3_TESTS ${XMIN_HAVE_GTK3_TESTS} PARENT_SCOPE)

add_executable(xmin_protocol_headers protocol_headers.c)
target_compile_features(xmin_protocol_headers PRIVATE c_std_11)
target_link_libraries(xmin_protocol_headers PRIVATE Xmin::xorgproto)
xmin_enable_warnings(xmin_protocol_headers)
add_test(NAME xmin.protocol-headers COMMAND xmin_protocol_headers)

add_executable(xmin_dix_atoms dix_atoms.c)
target_compile_features(xmin_dix_atoms PRIVATE c_std_11)
target_link_libraries(xmin_dix_atoms PRIVATE Xmin::dix)
xmin_enable_warnings(xmin_dix_atoms)
add_test(NAME xmin.dix-atoms COMMAND xmin_dix_atoms)

add_executable(xmin_dix_regions dix_regions.c)
target_compile_features(xmin_dix_regions PRIVATE c_std_11)
target_link_libraries(xmin_dix_regions PRIVATE Xmin::dix)
xmin_enable_warnings(xmin_dix_regions)
add_test(NAME xmin.dix-regions COMMAND xmin_dix_regions)

add_executable(xmin_dix_callbacks dix_callbacks.c)
target_compile_features(xmin_dix_callbacks PRIVATE c_std_11)
target_link_libraries(xmin_dix_callbacks PRIVATE Xmin::dix)
xmin_enable_warnings(xmin_dix_callbacks)
add_test(NAME xmin.dix-callbacks COMMAND xmin_dix_callbacks)

add_executable(xmin_dix_resources dix_resources.c)
target_compile_features(xmin_dix_resources PRIVATE c_std_11)
target_compile_definitions(xmin_dix_resources PRIVATE HAVE_DIX_CONFIG_H=1)
target_link_libraries(xmin_dix_resources PRIVATE Xmin::dix)
xmin_enable_warnings(xmin_dix_resources)
add_test(NAME xmin.dix-resources COMMAND xmin_dix_resources)

add_executable(xmin_dix_swaps dix_swaps.c)
target_compile_features(xmin_dix_swaps PRIVATE c_std_11)
target_link_libraries(xmin_dix_swaps PRIVATE Xmin::dix)
xmin_enable_warnings(xmin_dix_swaps)
add_test(NAME xmin.dix-swaps COMMAND xmin_dix_swaps)

add_executable(xmin_pixman_regions pixman_regions.c)
target_compile_features(xmin_pixman_regions PRIVATE c_std_11)
target_link_libraries(xmin_pixman_regions PRIVATE Xmin::pixman)
xmin_enable_warnings(xmin_pixman_regions)
add_test(NAME xmin.pixman-regions COMMAND xmin_pixman_regions)

add_executable(xmin_sha1 sha1.c)
target_compile_features(xmin_sha1 PRIVATE c_std_11)
target_link_libraries(xmin_sha1 PRIVATE Xmin::support)
xmin_enable_warnings(xmin_sha1)
add_test(NAME xmin.sha1 COMMAND xmin_sha1)

add_executable(xmin_render_transform render_transform.c)
target_compile_features(xmin_render_transform PRIVATE c_std_11)
target_link_libraries(xmin_render_transform PRIVATE Xmin::render)
xmin_enable_warnings(xmin_render_transform)
add_test(NAME xmin.render-transform COMMAND xmin_render_transform)

add_executable(xmin_randr_transform randr_transform.c)
target_compile_features(xmin_randr_transform PRIVATE c_std_11)
target_link_libraries(xmin_randr_transform PRIVATE Xmin::randr)
xmin_enable_warnings(xmin_randr_transform)
add_test(NAME xmin.randr-transform COMMAND xmin_randr_transform)

add_executable(xmin_damage_report
  damage_report.c
  "${PROJECT_SOURCE_DIR}/third_party/xorg-server/miext/damage/damage.c"
)
target_compile_features(xmin_damage_report PRIVATE c_std_11)
target_compile_definitions(xmin_damage_report PRIVATE HAVE_DIX_CONFIG_H=1)
target_link_libraries(xmin_damage_report PRIVATE Xmin::xserver_headers Xmin::dix)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xmin_damage_report PRIVATE
    -ffunction-sections -fdata-sections
  )
  set_property(SOURCE
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/miext/damage/damage.c"
    APPEND PROPERTY COMPILE_OPTIONS -Wno-unused-parameter
  )
  if(APPLE)
    target_link_options(xmin_damage_report PRIVATE "LINKER:-dead_strip")
  else()
    target_link_options(xmin_damage_report PRIVATE
      "LINKER:--gc-sections"
      "LINKER:--no-export-dynamic"
    )
  endif()
endif()
xmin_enable_warnings(xmin_damage_report)
add_test(NAME xmin.damage-report COMMAND xmin_damage_report)

add_executable(xmin_xfixes_disconnect
  xfixes_disconnect.c
  "${PROJECT_SOURCE_DIR}/third_party/xorg-server/xfixes/disconnect.c"
)
target_compile_features(xmin_xfixes_disconnect PRIVATE c_std_11)
target_compile_definitions(xmin_xfixes_disconnect PRIVATE HAVE_DIX_CONFIG_H=1)
target_link_libraries(xmin_xfixes_disconnect PRIVATE Xmin::xserver_headers)
xmin_enable_warnings(xmin_xfixes_disconnect)
add_test(NAME xmin.xfixes-disconnect COMMAND xmin_xfixes_disconnect)

add_executable(xmin_xi_property
  xi_property.c
  "${PROJECT_SOURCE_DIR}/third_party/xorg-server/Xi/xiproperty.c"
)
target_compile_features(xmin_xi_property PRIVATE c_std_11)
target_compile_definitions(xmin_xi_property PRIVATE HAVE_DIX_CONFIG_H=1)
target_link_libraries(xmin_xi_property PRIVATE Xmin::xserver_headers)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xmin_xi_property PRIVATE
    -ffunction-sections -fdata-sections
  )
  set_property(SOURCE
    "${PROJECT_SOURCE_DIR}/third_party/xorg-server/Xi/xiproperty.c"
    APPEND PROPERTY COMPILE_OPTIONS -w
  )
  if(APPLE)
    target_link_options(xmin_xi_property PRIVATE "LINKER:-dead_strip")
  else()
    # Clang's AddressSanitizer adds --export-dynamic, which roots every global
    # function and defeats the section GC these isolated upstream-TU tests
    # rely on. Keep only the helper reachable from the test executable.
    target_link_options(xmin_xi_property PRIVATE
      "LINKER:--gc-sections"
      "LINKER:--no-export-dynamic"
    )
  endif()
endif()
xmin_enable_warnings(xmin_xi_property)
add_test(NAME xmin.xi-property COMMAND xmin_xi_property)

add_executable(xmin_sync_fence
  sync_fence.c
  "${PROJECT_SOURCE_DIR}/third_party/xorg-server/miext/sync/misync.c"
)
target_compile_features(xmin_sync_fence PRIVATE c_std_11)
target_compile_definitions(xmin_sync_fence PRIVATE HAVE_DIX_CONFIG_H=1)
target_link_libraries(xmin_sync_fence PRIVATE Xmin::xserver_headers)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xmin_sync_fence PRIVATE
    -ffunction-sections -fdata-sections
  )
  if(APPLE)
    target_link_options(xmin_sync_fence PRIVATE "LINKER:-dead_strip")
  else()
    target_link_options(xmin_sync_fence PRIVATE
      "LINKER:--gc-sections"
      "LINKER:--no-export-dynamic"
    )
  endif()
endif()
xmin_enable_warnings(xmin_sync_fence)
add_test(NAME xmin.sync-fence COMMAND xmin_sync_fence)

add_executable(xmin_present_msc
  present_msc.c
  "${PROJECT_SOURCE_DIR}/third_party/xorg-server/present/present.c"
)
target_compile_features(xmin_present_msc PRIVATE c_std_11)
target_compile_definitions(xmin_present_msc PRIVATE HAVE_DIX_CONFIG_H=1)
target_link_libraries(xmin_present_msc PRIVATE Xmin::xserver_headers)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xmin_present_msc PRIVATE
    -ffunction-sections -fdata-sections
  )
  if(APPLE)
    target_link_options(xmin_present_msc PRIVATE "LINKER:-dead_strip")
  else()
    target_link_options(xmin_present_msc PRIVATE
      "LINKER:--gc-sections"
      "LINKER:--no-export-dynamic"
    )
  endif()
endif()
xmin_enable_warnings(xmin_present_msc)
add_test(NAME xmin.present-msc COMMAND xmin_present_msc)

add_executable(xmin_builtin_fonts builtin_fonts.c)
target_compile_features(xmin_builtin_fonts PRIVATE c_std_11)
target_link_libraries(xmin_builtin_fonts PRIVATE Xmin::xfont2)
xmin_enable_warnings(xmin_builtin_fonts)
add_test(NAME xmin.builtin-fonts COMMAND xmin_builtin_fonts)

add_executable(xmin_xkb_alloc xkb_alloc.c)
target_compile_features(xmin_xkb_alloc PRIVATE c_std_11)
target_link_libraries(xmin_xkb_alloc PRIVATE Xmin::xkb Xmin::os)
xmin_enable_warnings(xmin_xkb_alloc)
add_test(NAME xmin.xkb-alloc COMMAND xmin_xkb_alloc)

add_executable(xmin_xkb_embedded xkb_embedded.c)
target_compile_features(xmin_xkb_embedded PRIVATE c_std_11)
target_link_libraries(xmin_xkb_embedded PRIVATE Xmin::xkb)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xmin_xkb_embedded PRIVATE
    -ffunction-sections -fdata-sections
  )
  if(APPLE)
    target_link_options(xmin_xkb_embedded PRIVATE "LINKER:-dead_strip")
  else()
    target_link_options(xmin_xkb_embedded PRIVATE
      "LINKER:--gc-sections"
      "LINKER:--no-export-dynamic"
    )
  endif()
endif()
xmin_enable_warnings(xmin_xkb_embedded)
add_test(NAME xmin.xkb-embedded COMMAND xmin_xkb_embedded)

add_executable(xmin_fb_solid fb_solid.c)
target_compile_features(xmin_fb_solid PRIVATE c_std_11)
target_link_libraries(xmin_fb_solid PRIVATE Xmin::fb)
xmin_enable_warnings(xmin_fb_solid)
add_test(NAME xmin.fb-solid COMMAND xmin_fb_solid)

add_executable(xmin_vfb_config vfb_config.c)
target_compile_features(xmin_vfb_config PRIVATE c_std_11)
target_compile_definitions(xmin_vfb_config PRIVATE
  XMIN_TEST_WIDTH=${XMIN_DEFAULT_WIDTH}
  XMIN_TEST_HEIGHT=${XMIN_DEFAULT_HEIGHT}
  XMIN_TEST_DEPTH=${XMIN_DEFAULT_DEPTH}
  XMIN_TEST_DPI=${XMIN_DEFAULT_DPI}
)
target_link_libraries(xmin_vfb_config PRIVATE Xmin::vfb)
xmin_enable_warnings(xmin_vfb_config)
add_test(NAME xmin.vfb-config COMMAND xmin_vfb_config)

add_executable(xmin_xau_read xau_read.c)
target_compile_features(xmin_xau_read PRIVATE c_std_11)
target_link_libraries(xmin_xau_read PRIVATE Xmin::xau)
xmin_enable_warnings(xmin_xau_read)
add_test(NAME xmin.xau-read COMMAND xmin_xau_read)

add_executable(xmin_mit_cookie mit_cookie.c)
target_compile_features(xmin_mit_cookie PRIVATE c_std_11)
target_link_libraries(xmin_mit_cookie PRIVATE Xmin::os)
xmin_enable_warnings(xmin_mit_cookie)
add_test(NAME xmin.mit-cookie COMMAND xmin_mit_cookie)

if(NOT XMIN_ENABLE_TCP)
  add_executable(xmin_xtrans_local xtrans_local.c)
  target_compile_features(xmin_xtrans_local PRIVATE c_std_11)
  target_link_libraries(xmin_xtrans_local PRIVATE Xmin::os)
  xmin_enable_warnings(xmin_xtrans_local)
  add_test(NAME xmin.xtrans-local COMMAND xmin_xtrans_local)
endif()
