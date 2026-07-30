cmake_minimum_required(VERSION 3.15)
project(ScreenCapture CXX)

set(CMAKE_CXX_STANDARD 17)

add_executable(ScreenCapture WIN32
    src/main.cpp
    src/resource.h
    src/app.rc
)

target_link_libraries(ScreenCapture PRIVATE
    comctl32
    gdi32
    user32
    shell32
    comdlg32
    gdiplus
    vfw32
)

target_compile_definitions(ScreenCapture PRIVATE
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(ScreenCapture PRIVATE /W4)
endif()
