cmake_minimum_required(VERSION 3.20)
# NFSU2 Photo Mode. Built on its own (build.ps1) or as a subdirectory of a build that already has the u2sdk target.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(nfsu2_photomode LANGUAGES C CXX)
  # u2sdk: the submodule (git submodule update --init), or another checkout (build.ps1 passes it).
  set(U2SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/u2sdk CACHE PATH "u2sdk checkout")
  if(NOT EXISTS ${U2SDK_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "u2sdk not found at ${U2SDK_DIR}: run git submodule update --init")
  endif()
  include(${U2SDK_DIR}/cmake/u2flags.cmake)
  add_subdirectory(${U2SDK_DIR} u2sdk)
  set(PHOTOMODE_TOP ON)
endif()

# Pixel shaders for the GPU accumulator, compiled to embedded bytecode headers by the Windows SDK's fxc.
find_program(FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}x86" "$ENV{WindowsSdkVerBinPath}/x86" REQUIRED)
set(SHADER_HEADERS)
foreach(name accum show show2x copy depthcopy)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${name}.hlsl)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}_ps.h)
  add_custom_command(OUTPUT ${out}
    COMMAND ${FXC} /nologo /T ps_2_0 /E main /O3 /Vn g_ps_${name} /Fh ${out} ${src}
    DEPENDS ${src} COMMENT "fxc ${name}.hlsl")
  list(APPEND SHADER_HEADERS ${out})
endforeach()

u2_add_script(photomode NFSU2PhotoMode
  src/dllmain.cpp
  src/photomode.cpp
  src/pauseentry.cpp
  src/garage.cpp
  src/settings.cpp
  src/capture.cpp
  src/accum.cpp
  src/supersample.cpp
  src/watermark.cpp
  src/grid.cpp
  src/overlay.cpp
  src/focusprobe.cpp
  src/tags.cpp
  src/wsfgamma.cpp
  src/photomode.rc
  ${SHADER_HEADERS})
target_include_directories(photomode PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
# The resource script embeds the built-in watermark PNGs: rebuild it when they change.
set_source_files_properties(src/photomode.rc PROPERTIES OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/nfsplanet_watermark.png;${CMAKE_CURRENT_SOURCE_DIR}/src/nfs.png")

# ---- host unit tests (pure code, no game needed) ---------------------------
# Inside the scripts workspace its u2tests runs tests/test_photomath.cpp instead.
if(PHOTOMODE_TOP)
  add_executable(photomode_tests ${U2SDK_DIR}/tests/test_main.cpp tests/test_photomath.cpp)
  target_include_directories(photomode_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${U2SDK_DIR}/include ${U2SDK_DIR}/tests)
endif()
