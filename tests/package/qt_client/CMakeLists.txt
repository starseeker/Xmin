cmake_minimum_required(VERSION 3.21...4.0)
project(XminQtClientPackageProbe LANGUAGES C CXX)

find_package(Xmin 0.1 CONFIG REQUIRED)

add_executable(xmin_qt_client_package_probe
  "${CMAKE_CURRENT_LIST_DIR}/../../integration/qt_client.c"
)
target_link_libraries(xmin_qt_client_package_probe PRIVATE Xmin::QtX11)

add_executable(xmin_qt_client_cxx_probe qt_client_compile.cpp)
target_link_libraries(xmin_qt_client_cxx_probe PRIVATE Xmin::QtX11)
