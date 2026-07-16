if(XMIN_ENABLE_INSTALL)
  install(FILES DejaVuSans.ttf
    DESTINATION "${CMAKE_INSTALL_DATADIR}/Xmin/qt-fonts"
    COMPONENT QtClientRuntime
  )
endif()
