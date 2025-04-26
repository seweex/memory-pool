cmake_minimum_required(VERSION 3.15)

project(swx-mempool)
option(HEADER_ONLY ON)
add_library(${PROJECT_NAME} INTERFACE)

target_include_directories (${PROJECT_NAME} INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

install (DIRECTORY include/
  DESTINATION include
  FILES_MATCHING PATTERN "*.h"
)