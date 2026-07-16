cmake_minimum_required(VERSION 3.21...4.0)

project(XminQtOpenGLProbe LANGUAGES CXX)

# Dynamic plugins are discovered at runtime from the installed Xmin profile.
# Avoid importing CMake packages for configured but deliberately uninstalled
# Qt plugins.
set(QT_SKIP_AUTO_PLUGIN_INCLUSION ON)
find_package(Qt6 6.11 CONFIG REQUIRED COMPONENTS
  Gui OpenGL Widgets OpenGLWidgets
)

add_executable(xmin_qt6_opengl ../../qt/opengl.cpp)
target_compile_features(xmin_qt6_opengl PRIVATE cxx_std_17)
target_compile_definitions(xmin_qt6_opengl PRIVATE XMIN_QT_MAJOR=6)
target_link_libraries(xmin_qt6_opengl PRIVATE Qt6::Gui Qt6::OpenGL)

add_executable(xmin_qt6_opengl_widget ../../qt/opengl_widget.cpp)
target_compile_features(xmin_qt6_opengl_widget PRIVATE cxx_std_17)
target_link_libraries(xmin_qt6_opengl_widget PRIVATE
  Qt6::Gui Qt6::OpenGL Qt6::Widgets Qt6::OpenGLWidgets
)
