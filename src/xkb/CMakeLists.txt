# Complete XKB protocol, state, controls, event processing, allocation helpers,
# and generic DDX hooks.  The imported ddxLoad.c is retained for provenance,
# but Xmin compiles its fixed-map loader instead of the external xkbcomp route.
set(xmin_xkb_source_dir "${PROJECT_SOURCE_DIR}/third_party/xorg-server/xkb")
if(NOT HAVE_FMEMOPEN)
  message(FATAL_ERROR
    "Xmin's embedded XKB loader currently requires the POSIX fmemopen API."
  )
endif()
set(xmin_xkb_sources
  ddxBeep.c
  ddxCtrls.c
  ddxLEDs.c
  xkmread.c
  xkb.c
  xkbUtils.c
  xkbEvents.c
  xkbAccessX.c
  xkbSwap.c
  xkbLEDs.c
  xkbInit.c
  xkbActions.c
  xkbPrKeyEv.c
  XKBMisc.c
  XKBAlloc.c
  XKBGAlloc.c
  XKBMAlloc.c
  ddxKillSrv.c
  ddxPrivate.c
  ddxVT.c
)
list(TRANSFORM xmin_xkb_sources PREPEND "${xmin_xkb_source_dir}/")
list(APPEND xmin_xkb_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/xmin_xkb_case.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/xmin_xkb_keymap.c"
)

xmin_add_component(xkb
  UPSTREAM
  TYPE STATIC
  SOURCES ${xmin_xkb_sources}
  PRIVATE_DEFINITIONS HAVE_DIX_CONFIG_H=1
  PRIVATE_INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/data/xkb"
  PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
  PUBLIC_LIBRARIES
    Xmin::xserver_headers
    Xmin::xkb_data
    Xmin::dix
    Xmin::mi
)

# Xorg's XKB files contain several independently usable APIs per translation
# unit. Section granularity avoids retaining unrelated compiler and text paths.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(xmin_xkb PRIVATE -ffunction-sections -fdata-sections)
endif()
