cmake_minimum_required(VERSION 3.20)

project(porTUI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_library(LIBPROC_LIBRARY proc REQUIRED)

add_executable(portui
  src/main.cpp
  src/app.cpp
  src/core/debug.cpp
  src/scanner/serial_scanner.cpp
)

target_include_directories(portui
  PRIVATE
    include
)

target_link_libraries(portui
  PRIVATE
    ${LIBPROC_LIBRARY}
)
