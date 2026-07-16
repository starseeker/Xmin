add_subdirectory(fonts)
add_subdirectory(xkb)

if(XMIN_BUILD_QT_CLIENT)
  add_subdirectory(qt-fonts)
endif()
