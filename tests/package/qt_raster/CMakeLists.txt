cmake_minimum_required(VERSION 3.21...4.0)

project(XminQtRasterProbe LANGUAGES C CXX)

# The Xmin SDK installs only the dynamic plugins selected for its deployment
# profile. Qt's generated metadata otherwise tries to import every plugin that
# was configured in the Qt build, including deliberately uninstalled ones.
set(QT_SKIP_AUTO_PLUGIN_INCLUSION ON)
find_package(Qt6 6.11 CONFIG REQUIRED COMPONENTS Gui)

add_executable(xmin_qt6_raster ../../qt/raster.cpp)
target_compile_features(xmin_qt6_raster PRIVATE cxx_std_17)
target_compile_definitions(xmin_qt6_raster PRIVATE XMIN_QT_MAJOR=6)
target_link_libraries(xmin_qt6_raster PRIVATE Qt6::Gui)

add_executable(xmin_qt6_multiclient ../../qt/multiclient.cpp)
target_compile_features(xmin_qt6_multiclient PRIVATE cxx_std_17)
target_link_libraries(xmin_qt6_multiclient PRIVATE Qt6::Gui)

add_executable(xmin_qt6_input ../../qt/input.cpp)
target_compile_features(xmin_qt6_input PRIVATE cxx_std_17)
target_link_libraries(xmin_qt6_input PRIVATE Qt6::Gui)

add_executable(xmin_qt6_input_driver ../../qt/input_driver.c)
target_compile_features(xmin_qt6_input_driver PRIVATE c_std_11)
