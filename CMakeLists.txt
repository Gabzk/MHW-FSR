cmake_minimum_required(VERSION 3.24)

project(MHW-FSR VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Path to FidelityFX SDK
if(NOT DEFINED FFX_SDK_DIR)
  set(FFX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../FidelityFX-SDK")
endif()

set(FFX_API_INCLUDE "${FFX_SDK_DIR}/Kits/FidelityFX/api/include")
set(FFX_UPSCALE_INCLUDE "${FFX_SDK_DIR}/Kits/FidelityFX/upscalers/include")
set(FFX_LIB_DIR "${FFX_SDK_DIR}/Kits/FidelityFX/signedbin")

add_library(MHW-FSR SHARED
  src/dllmain.cpp
  src/config.cpp
  src/crash.cpp
  src/d3d_util.cpp
  src/fsr_context.cpp
  src/fsr_loader.cpp
  src/game.cpp
  src/log.cpp
  src/memory.cpp
  src/ngx_params.cpp
  src/ngx_shim.cpp
  src/probe.cpp
)

target_include_directories(MHW-FSR PRIVATE
  src
  "${FFX_API_INCLUDE}"
  "${FFX_UPSCALE_INCLUDE}"
)

target_compile_definitions(MHW-FSR PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
  _CRT_SECURE_NO_WARNINGS
  MHW_FSR_VERSION="${PROJECT_VERSION}"
  $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

target_compile_options(MHW-FSR PRIVATE /W4 /permissive- /utf-8 /EHsc /Zi "$<$<NOT:$<CONFIG:Debug>>:/O2;/Gw>")
target_link_options(MHW-FSR PRIVATE /DEBUG /OPT:REF /OPT:ICF /DELAYLOAD:amd_fidelityfx_loader_dx12.dll)

target_link_directories(MHW-FSR PRIVATE "${FFX_LIB_DIR}")
target_link_libraries(MHW-FSR PRIVATE
  amd_fidelityfx_loader_dx12.lib
  delayimp.lib
  version
  d3d12
  dxgi
)
