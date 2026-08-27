cmake_minimum_required(VERSION 3.20)

project(porTUI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_library(LIBPROC_LIBRARY proc REQUIRED)
find_package(Threads REQUIRED)
find_package(ftxui CONFIG REQUIRED)

add_executable(portui
  src/main.cpp
  src/app.cpp
  src/core/debug.cpp
  src/core/diff.cpp
  src/core/process_killer.cpp
  src/core/process_usage.cpp
  src/core/snapshot_pipeline.cpp
  src/tui/live_table.cpp
  src/scanner/scanner_support.cpp
  src/scanner/lsof_fallback.cpp
  src/scanner/serial_scanner.cpp
  src/scanner/parallel_scanner.cpp
)

target_include_directories(portui
  PRIVATE
    include
)

target_link_libraries(portui
  PRIVATE
    ${LIBPROC_LIBRARY}
    Threads::Threads
    ftxui::component
)
