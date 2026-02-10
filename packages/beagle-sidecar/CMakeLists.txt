cmake_minimum_required(VERSION 3.16)
project(beagle-sidecar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BEAGLE_SDK_STUB "Build without the Beagle SDK linked" ON)

add_executable(beagle-sidecar
  src/main.cpp
  src/beagle_sdk.cpp
)

target_include_directories(beagle-sidecar PRIVATE src)

if(NOT BEAGLE_SDK_STUB)
  if(NOT DEFINED BEAGLE_SDK_ROOT)
    set(BEAGLE_SDK_ROOT $ENV{BEAGLE_SDK_ROOT})
  endif()
  if(NOT BEAGLE_SDK_ROOT)
    message(FATAL_ERROR "BEAGLE_SDK_ROOT is required when BEAGLE_SDK_STUB=OFF")
  endif()

  # TODO: Wire real SDK headers/libs here.
  # Example placeholders (adjust to actual SDK layout):
  # target_include_directories(beagle-sidecar PRIVATE ${BEAGLE_SDK_ROOT}/include)
  # target_link_libraries(beagle-sidecar PRIVATE elastos_carrier)
endif()
