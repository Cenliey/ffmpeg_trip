cmake_minimum_required(VERSION 3.10)
project(ffmpeg_trip_02 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libswscale
    libavutil
)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_executable(ffmpeg_trip_02 main.cpp)
target_link_libraries(ffmpeg_trip_02 PRIVATE
    PkgConfig::FFMPEG
    PkgConfig::SDL2
)
