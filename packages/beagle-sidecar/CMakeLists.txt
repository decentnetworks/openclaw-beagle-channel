cmake_minimum_required(VERSION 3.16)
project(beagle-sidecar LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BEAGLE_SDK_STUB "Build without the Beagle SDK linked" ON)
set(BEAGLE_SDK_BUILD_DIR "" CACHE PATH "Carrier SDK build directory")

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

  if(NOT BEAGLE_SDK_BUILD_DIR)
    if(EXISTS "${BEAGLE_SDK_ROOT}/build/macos-arm64")
      set(BEAGLE_SDK_BUILD_DIR "${BEAGLE_SDK_ROOT}/build/macos-arm64")
    elseif(EXISTS "${BEAGLE_SDK_ROOT}/build/macos")
      set(BEAGLE_SDK_BUILD_DIR "${BEAGLE_SDK_ROOT}/build/macos")
    elseif(EXISTS "${BEAGLE_SDK_ROOT}/build/linux")
      set(BEAGLE_SDK_BUILD_DIR "${BEAGLE_SDK_ROOT}/build/linux")
    else()
      set(BEAGLE_SDK_BUILD_DIR "${BEAGLE_SDK_ROOT}/build")
    endif()
  endif()

  set(BEAGLE_SDK_INTERMEDIATE_DIR "${BEAGLE_SDK_BUILD_DIR}/intermediates")

  target_compile_definitions(beagle-sidecar PRIVATE BEAGLE_SDK_STUB=0)
  if(APPLE OR UNIX)
    target_compile_definitions(beagle-sidecar PRIVATE HAVE_UNISTD_H=1 HAVE_GETOPT_H=1)
  endif()
  target_sources(beagle-sidecar PRIVATE ${BEAGLE_SDK_ROOT}/config/carrier_config.c)
  set_source_files_properties(${BEAGLE_SDK_ROOT}/config/carrier_config.c PROPERTIES LANGUAGE C)

  target_include_directories(beagle-sidecar PRIVATE
    ${BEAGLE_SDK_ROOT}/src/carrier
    ${BEAGLE_SDK_ROOT}/config
    ${BEAGLE_SDK_INTERMEDIATE_DIR}/include
  )

  target_link_directories(beagle-sidecar PRIVATE
    ${BEAGLE_SDK_BUILD_DIR}/src/carrier
    ${BEAGLE_SDK_INTERMEDIATE_DIR}/lib
  )

  target_link_libraries(beagle-sidecar PRIVATE carrier config crystal)

  set_target_properties(beagle-sidecar PROPERTIES
    BUILD_RPATH "${BEAGLE_SDK_BUILD_DIR}/src/carrier;${BEAGLE_SDK_INTERMEDIATE_DIR}/lib"
    INSTALL_RPATH "${BEAGLE_SDK_BUILD_DIR}/src/carrier;${BEAGLE_SDK_INTERMEDIATE_DIR}/lib"
  )
else()
  target_compile_definitions(beagle-sidecar PRIVATE BEAGLE_SDK_STUB=1)
endif()
