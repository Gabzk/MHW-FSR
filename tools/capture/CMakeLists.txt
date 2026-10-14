# Developer tool (not shipped): MHW-Capture.dll, see capture_layer.cpp.
#   pwsh: . $PROFILE; devshell; cmake -S tools/capture -B build-capture -G Ninja -DCMAKE_BUILD_TYPE=Release
#   pwsh: . $PROFILE; devshell; cmake --build build-capture
cmake_minimum_required(VERSION 3.20)
project(MHW-Capture LANGUAGES CXX)

find_path(RENDERDOC_INCLUDE_DIR renderdoc_app.h PATHS "$ENV{ProgramFiles}/RenderDoc" REQUIRED)

add_library(MHW-Capture SHARED capture_layer.cpp)
target_include_directories(MHW-Capture PRIVATE "${RENDERDOC_INCLUDE_DIR}")
target_compile_features(MHW-Capture PRIVATE cxx_std_20)
target_compile_definitions(MHW-Capture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
set_target_properties(MHW-Capture PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
