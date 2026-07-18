add_test(NAME xmin.version COMMAND Xmin::server --version)
set_tests_properties(xmin.version PROPERTIES
  PASS_REGULAR_EXPRESSION "Xmin ${PROJECT_VERSION}"
)
add_test(NAME xmin.help COMMAND Xmin::server --help)
set_tests_properties(xmin.help PROPERTIES
  PASS_REGULAR_EXPRESSION "usage: Xmin .*DISPLAY"
)

add_executable(xmin_foundation foundation.cpp)
target_compile_features(xmin_foundation PRIVATE cxx_std_17)
target_link_libraries(xmin_foundation PRIVATE Xmin::server_core)
xmin_enable_warnings(xmin_foundation)
add_test(NAME xmin.foundation COMMAND xmin_foundation)

add_executable(xmin_platform_test platform.cpp)
target_compile_features(xmin_platform_test PRIVATE cxx_std_17)
target_compile_definitions(xmin_platform_test PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(xmin_platform_test PRIVATE Xmin::platform)
xmin_enable_warnings(xmin_platform_test)
add_test(NAME xmin.platform COMMAND xmin_platform_test)

add_executable(xmin_handshake integration/handshake.cpp)
target_compile_features(xmin_handshake PRIVATE cxx_std_17)
target_compile_definitions(xmin_handshake PRIVATE _POSIX_C_SOURCE=200809L)
target_include_directories(xmin_handshake PRIVATE "${PROJECT_BINARY_DIR}/generated")
xmin_enable_warnings(xmin_handshake)
add_test(NAME xmin.handshake
  COMMAND xmin_handshake $<TARGET_FILE:xmin_server>
)
set_tests_properties(xmin.handshake PROPERTIES TIMEOUT 20)

add_executable(xmin_standalone integration/standalone.cpp)
target_compile_features(xmin_standalone PRIVATE cxx_std_17)
target_compile_definitions(xmin_standalone PRIVATE _POSIX_C_SOURCE=200809L)
xmin_enable_warnings(xmin_standalone)
add_test(NAME xmin.standalone
  COMMAND xmin_standalone $<TARGET_FILE:xmin_server>
)
set_tests_properties(xmin.standalone PROPERTIES TIMEOUT 30)

if(TARGET Xmin::run)
  add_executable(xmin_launcher_child integration/launcher_child.c)
  target_compile_features(xmin_launcher_child PRIVATE c_std_11)
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

  if(TARGET Xmin::QtX11)
    add_executable(xmin_qt_client_core integration/qt_client_core.c)
    target_compile_features(xmin_qt_client_core PRIVATE c_std_11)
    target_include_directories(xmin_qt_client_core PRIVATE
      "${PROJECT_BINARY_DIR}/generated"
    )
    target_link_libraries(xmin_qt_client_core PRIVATE Xmin::QtX11)
    xmin_enable_warnings(xmin_qt_client_core)
    add_test(NAME xmin.qt-client-core
      COMMAND Xmin::run
        --server $<TARGET_FILE:xmin_server>
        --screen 96x80x24
        -- $<TARGET_FILE:xmin_qt_client_core>
    )
    set_tests_properties(xmin.qt-client-core PROPERTIES TIMEOUT 20)
  endif()

  # Host XCB is test-only and remains an independent protocol authority.
  find_path(XMIN_TEST_XCB_INCLUDE_DIR xcb/xcb.h)
  find_library(XMIN_TEST_XCB_LIBRARY NAMES xcb)

  if(TARGET Xmin::ctl AND XMIN_TEST_XCB_INCLUDE_DIR AND XMIN_TEST_XCB_LIBRARY)
    add_executable(xminctl_target integration/xminctl_target.c)
    target_compile_features(xminctl_target PRIVATE c_std_11)
    target_include_directories(xminctl_target SYSTEM PRIVATE
      "${XMIN_TEST_XCB_INCLUDE_DIR}"
    )
    target_compile_definitions(xminctl_target PRIVATE _POSIX_C_SOURCE=200809L)
    target_link_libraries(xminctl_target PRIVATE "${XMIN_TEST_XCB_LIBRARY}")
    xmin_enable_warnings(xminctl_target)

    add_executable(xminctl_integration integration/xminctl_integration.c)
    target_compile_features(xminctl_integration PRIVATE c_std_11)
    target_compile_definitions(xminctl_integration PRIVATE _POSIX_C_SOURCE=200809L)
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

  if(XMIN_TEST_XCB_INCLUDE_DIR AND XMIN_TEST_XCB_LIBRARY)
    function(xmin_add_core_xcb_test target source test_name width height)
      add_executable(${target} ${source})
      target_compile_features(${target} PRIVATE c_std_11)
      target_include_directories(${target}
        PRIVATE "${PROJECT_BINARY_DIR}/generated"
        SYSTEM PRIVATE "${XMIN_TEST_XCB_INCLUDE_DIR}"
      )
      target_link_libraries(${target} PRIVATE "${XMIN_TEST_XCB_LIBRARY}")
      xmin_enable_warnings(${target})
      add_test(NAME ${test_name}
        COMMAND Xmin::run
          --server $<TARGET_FILE:xmin_server>
          --screen ${width}x${height}x24
          -- $<TARGET_FILE:${target}>
      )
      set_tests_properties(${test_name} PROPERTIES TIMEOUT 20)
    endfunction()

    xmin_add_core_xcb_test(
      xmin_xcb_raster integration/xcb_raster.c xmin.xcb-raster 96 80)
    xmin_add_core_xcb_test(
      xmin_xcb_core integration/xcb_core.c xmin.xcb-core 96 80)

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(XMIN_TEST_XCB_EXTENSIONS QUIET IMPORTED_TARGET
        xcb-render xcb-randr xcb-xfixes xcb-shape xcb-sync xcb-xinput
        xcb-xkb xcb-xtest xcb-composite xcb-damage xcb-present xcb-shm
        xcb-dbe xcb-xinerama xcb-screensaver
      )
      if(TARGET PkgConfig::XMIN_TEST_XCB_EXTENSIONS)
        function(xmin_add_extension_xcb_test target source test_name width height)
          add_executable(${target} ${source})
          target_compile_features(${target} PRIVATE c_std_11)
          target_include_directories(${target} PRIVATE
            "${PROJECT_BINARY_DIR}/generated"
          )
          target_link_libraries(${target} PRIVATE
            PkgConfig::XMIN_TEST_XCB_EXTENSIONS
            "${XMIN_TEST_XCB_LIBRARY}"
          )
          xmin_enable_warnings(${target})
          add_test(NAME ${test_name}
            COMMAND Xmin::run
              --server $<TARGET_FILE:xmin_server>
              --screen ${width}x${height}x24
              -- $<TARGET_FILE:${target}>
          )
          set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
        endfunction()

        xmin_add_extension_xcb_test(
          xmin_xcb_extensions integration/xcb_extensions.c
          xmin.xcb-extensions 96 80)
        xmin_add_extension_xcb_test(
          xmin_xcb_xtest_state integration/xcb_xtest_state.c
          xmin.xcb-xtest-state 96 80)
        xmin_add_extension_xcb_test(
          xmin_xcb_xfixes_state integration/xcb_xfixes_state.c
          xmin.xcb-xfixes-state 96 80)
        xmin_add_extension_xcb_test(
          xmin_xcb_randr_state integration/xcb_randr_state.c
          xmin.xcb-randr-state 100 90)
        xmin_add_extension_xcb_test(
          xmin_xcb_damage_state integration/xcb_damage_state.c
          xmin.xcb-damage-state 96 80)
        xmin_add_extension_xcb_test(
          xmin_xcb_composite_state integration/xcb_composite_state.c
          xmin.xcb-composite-state 96 80)
        xmin_add_extension_xcb_test(
          xmin_xcb_present_state integration/xcb_present_state.c
          xmin.xcb-present-state 96 80)
        xmin_add_extension_xcb_test(
          xmin_xcb_xkb_state integration/xcb_xkb_state.c
          xmin.xcb-xkb-state 96 80)
        xmin_add_extension_xcb_test(
          xmin_xcb_xi2_state integration/xcb_xi2_state.c
          xmin.xcb-xi2-state 96 80)
        if(XMIN_HAVE_MITSHM)
          xmin_add_extension_xcb_test(
            xmin_xcb_shm_state integration/xcb_shm_state.c
            xmin.xcb-shm-state 96 80)
          target_compile_definitions(xmin_xcb_shm_state PRIVATE
            _POSIX_C_SOURCE=200809L
          )
        endif()

        pkg_check_modules(XMIN_TEST_XKBCOMMON_X11 QUIET IMPORTED_TARGET
          xkbcommon-x11
        )
        if(TARGET PkgConfig::XMIN_TEST_XKBCOMMON_X11)
          add_executable(xmin_xkbcommon_state integration/xkbcommon_state.c)
          target_compile_features(xmin_xkbcommon_state PRIVATE c_std_11)
          target_link_libraries(xmin_xkbcommon_state PRIVATE
            PkgConfig::XMIN_TEST_XKBCOMMON_X11
            PkgConfig::XMIN_TEST_XCB_EXTENSIONS
            "${XMIN_TEST_XCB_LIBRARY}"
          )
          xmin_enable_warnings(xmin_xkbcommon_state)
          add_test(NAME xmin.xkbcommon-state
            COMMAND Xmin::run
              --server $<TARGET_FILE:xmin_server>
              --screen 96x80x24
              -- $<TARGET_FILE:xmin_xkbcommon_state>
          )
          set_tests_properties(xmin.xkbcommon-state PROPERTIES TIMEOUT 20)
          if(XMIN_ENABLE_SANITIZERS)
            set_property(TEST xmin.xkbcommon-state APPEND PROPERTY
              ENVIRONMENT "ASAN_OPTIONS=strict_string_checks=0"
            )
          endif()
        endif()
      endif()
    endif()
  endif()
endif()

# The direct client library is intentionally independent of server-side GLX.
if(TARGET Xmin::gl_client)
  add_executable(xmin_osmesa_separate glx/osmesa_separate.c)
  target_compile_features(xmin_osmesa_separate PRIVATE c_std_11)
  target_link_libraries(xmin_osmesa_separate PRIVATE Xmin::gl_client)
  xmin_enable_warnings(xmin_osmesa_separate)
  add_test(NAME xmin.osmesa-separate COMMAND xmin_osmesa_separate)

  add_executable(xmin_software_direct_glsl glx/software_direct_glsl.c)
  target_compile_features(xmin_software_direct_glsl PRIVATE c_std_11)
  target_link_libraries(xmin_software_direct_glsl PRIVATE Xmin::gl_client)
  xmin_enable_warnings(xmin_software_direct_glsl)
  add_test(NAME xmin.software-direct-glsl COMMAND xmin_software_direct_glsl)

  find_path(XMIN_TEST_X11_INCLUDE_DIR X11/Xlib.h)
  find_library(XMIN_TEST_X11_LIBRARY NAMES X11)
  if(XMIN_TEST_X11_INCLUDE_DIR AND XMIN_TEST_X11_LIBRARY AND TARGET Xmin::run)
    add_executable(xmin_software_direct_window glx/software_direct_window.c)
    target_compile_features(xmin_software_direct_window PRIVATE c_std_11)
    target_include_directories(xmin_software_direct_window SYSTEM PRIVATE
      "${XMIN_TEST_X11_INCLUDE_DIR}"
    )
    target_link_libraries(xmin_software_direct_window PRIVATE
      Xmin::gl_client "${XMIN_TEST_X11_LIBRARY}"
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

# Toolkits remain optional, application-side acceptance dependencies.
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
      ENVIRONMENT "QT_QPA_PLATFORM=xcb" TIMEOUT 30)

    if(TARGET Xmin::gl_client)
      add_executable(xmin_${qt_suffix}_opengl qt/opengl.cpp)
      target_compile_features(xmin_${qt_suffix}_opengl PRIVATE cxx_std_17)
      target_compile_definitions(xmin_${qt_suffix}_opengl PRIVATE
        XMIN_QT_MAJOR=${qt_major}
      )
      target_link_libraries(xmin_${qt_suffix}_opengl PRIVATE
        Xmin::gl_client ${qt_gui_target}
      )
      if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(xmin_${qt_suffix}_opengl PRIVATE
          "LINKER:--no-as-needed")
      endif()
      xmin_enable_warnings(xmin_${qt_suffix}_opengl)
      add_test(NAME xmin.${qt_suffix}-opengl
        COMMAND Xmin::run
          --server $<TARGET_FILE:xmin_server>
          --screen 320x240x24
          -- $<TARGET_FILE:xmin_${qt_suffix}_opengl>
      )
      set_tests_properties(xmin.${qt_suffix}-opengl PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=xcb" TIMEOUT 30)
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
      target_link_libraries(xmin_gtk3_raster PRIVATE PkgConfig::XMIN_TEST_GTK3)
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
    message(FATAL_ERROR "Toolkit tests require XMIN_BUILD_LAUNCHER=ON")
  endif()
  if(NOT XMIN_HAVE_QT5_TESTS AND NOT XMIN_HAVE_QT6_TESTS)
    message(FATAL_ERROR "Toolkit tests require Qt 5 Gui or Qt 6 Gui")
  endif()
  if(NOT XMIN_HAVE_GTK3_TESTS)
    message(FATAL_ERROR "Toolkit tests require GTK 3 development files")
  endif()
endif()

if(XMIN_ENABLE_INSTALL)
  set(xmin_install_have_launcher FALSE)
  set(xmin_install_have_gl FALSE)
  set(xmin_install_test_arguments
    "-DBINARY_DIR=${PROJECT_BINARY_DIR}"
    "-DSTAGE_DIR=${PROJECT_BINARY_DIR}/install-test/prefix"
    "-DBINDIR=${CMAKE_INSTALL_BINDIR}"
    "-DLIBDIR=${CMAKE_INSTALL_LIBDIR}"
    "-DVERSION=${PROJECT_VERSION}"
  )
  if(TARGET Xmin::run)
    set(xmin_install_have_launcher TRUE)
    list(APPEND xmin_install_test_arguments
      "-DTEST_CHILD=$<TARGET_FILE:xmin_launcher_child>"
    )
  endif()
  if(TARGET Xmin::gl_client)
    set(xmin_install_have_gl TRUE)
  endif()
  list(APPEND xmin_install_test_arguments
    "-DHAVE_LAUNCHER=${xmin_install_have_launcher}"
    "-DHAVE_GL=${xmin_install_have_gl}"
  )
  add_test(NAME xmin.install-relocation-dependencies
    COMMAND "${CMAKE_COMMAND}" ${xmin_install_test_arguments}
      -P "${PROJECT_SOURCE_DIR}/cmake/VerifyInstall.cmake"
  )
  set_tests_properties(xmin.install-relocation-dependencies PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 60)
endif()

set(XMIN_HAVE_QT5_TESTS ${XMIN_HAVE_QT5_TESTS} PARENT_SCOPE)
set(XMIN_HAVE_QT6_TESTS ${XMIN_HAVE_QT6_TESTS} PARENT_SCOPE)
set(XMIN_HAVE_GTK3_TESTS ${XMIN_HAVE_GTK3_TESTS} PARENT_SCOPE)
