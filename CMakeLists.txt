# Pixel shaders for the GPU accumulator, compiled to embedded bytecode headers by the Windows SDK's fxc.
find_program(FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}x86" "$ENV{WindowsSdkVerBinPath}/x86" REQUIRED)
set(SHADER_HEADERS)
foreach(name accum show show2x copy depthcopy)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${name}.hlsl)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}_ps.h)
  add_custom_command(OUTPUT ${out}
    COMMAND ${FXC} /nologo /T ps_2_0 /E main /O3 /Vn g_ps_${name} /Fh ${out} ${src}
    DEPENDS ${src} COMMENT "fxc ${name}.hlsl")
  list(APPEND SHADER_HEADERS ${out})
endforeach()

u2_add_script(photomode NFSU2PhotoMode
  dllmain.cpp
  photomode.cpp
  pauseentry.cpp
  settings.cpp
  capture.cpp
  accum.cpp
  supersample.cpp
  watermark.cpp
  grid.cpp
  overlay.cpp
  focusprobe.cpp
  photomode.rc
  ${SHADER_HEADERS})
target_include_directories(photomode PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
# The resource script embeds the built-in watermark PNGs: rebuild it when they change.
set_source_files_properties(photomode.rc PROPERTIES OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/nfsplanet_watermark.png;${CMAKE_CURRENT_SOURCE_DIR}/nfs.png")
