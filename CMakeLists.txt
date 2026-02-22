cmake_minimum_required(VERSION 3.21)

project(trace LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(RT_QT6_ROOT "D:/Qt/6.9.3/msvc2022_64" CACHE PATH "Qt6 installation root")
set(RT_GLM_ROOT "D:/glm" CACHE PATH "GLM include root")

list(APPEND CMAKE_PREFIX_PATH "${RT_QT6_ROOT}")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

set(TRACE_SOURCES
    src/main.cpp
    src/app/MainWindow.h
    src/app/MainWindow.cpp
    src/render/RenderWidget.h
    src/render/RenderWidget.cpp
    src/render/Renderer.h
    src/render/Renderer.cpp
    src/scene/SceneData.h
    src/scene/SceneData.cpp
    src/scene/BvhBuilder.h
    src/scene/BvhBuilder.cpp
)

add_executable(trace ${TRACE_SOURCES})

target_include_directories(
    trace
    PRIVATE
        src
        "${RT_GLM_ROOT}"
)

target_link_libraries(
    trace
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
)

if(MSVC)
    target_compile_options(trace PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(trace PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_command(
    TARGET trace
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/src/render/glsl"
            "$<TARGET_FILE_DIR:trace>/glsl"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:trace>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RT_QT6_ROOT}/plugins/platforms/qwindows.dll"
            "$<TARGET_FILE_DIR:trace>/platforms/qwindows.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RT_QT6_ROOT}/plugins/platforms/qwindowsd.dll"
            "$<TARGET_FILE_DIR:trace>/platforms/qwindowsd.dll"
    COMMENT "Copying GLSL shaders to runtime directory."
)
