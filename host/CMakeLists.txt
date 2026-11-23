cmake_minimum_required(VERSION 3.20)
project(asura_host CXX)
set(CMAKE_CXX_STANDARD 20)

# The recompiled game code is compiled straight into the exe so generated/import_stubs.cpp can override the weak import thunks.
file(GLOB PPC_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../ppc/*.cpp")
set(HOST_SRC main.cpp kernel.cpp kfile.cpp kstring.cpp xam.cpp gpu.cpp audio.cpp hooks.cpp audio_out.cpp xma.cpp xenos.cpp xenos_glue.cpp options.cpp uescript.cpp shaderpack.cpp shaderindex.cpp funcprof.cpp video.cpp generated/import_stubs.cpp)
# libmspack's LZX decoder (LGPL 2.1, thirdparty\libmspack): unpacks the cooked packages for the shader index
set(MSPACK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/libmspack/mspack)
set(MSPACK_SRC ${MSPACK_DIR}/lzxd.c)
set_source_files_properties(${MSPACK_SRC} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "/TC;/O2;/w" COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS")   # C source, built by the C++ compiler in C mode (/TC)
add_executable(asura ${HOST_SRC} ${PPC_SRC} ${MSPACK_SRC})
target_include_directories(asura PRIVATE
    ${MSPACK_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ppc
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    C:/dev/XenonRecomp/thirdparty/simde)
target_link_libraries(asura PRIVATE user32 gdi32 ole32 shell32 bcrypt xinput winmm d3d11 dxgi d3dcompiler windowscodecs comdlg32 ole32)
target_link_options(asura PRIVATE /MAP:asura.map /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)   # a windowed program: no console window (main.cpp)
set(ASURA_GUEST_OPT "/O2" CACHE STRING "Optimisation flag for the recompiled game code")
# guest memory is shared between threads and read through byte-swapping macros, so strict aliasing stays off (as in BCR2)
set_source_files_properties(${PPC_SRC} PROPERTIES COMPILE_OPTIONS "${ASURA_GUEST_OPT};/w;-msse4.1;/clang:-fno-strict-aliasing")
# Profiling build (its own build folder): every game function counts its calls, see funcprof.h
option(ASURA_FUNCPROF "Count calls of every recompiled game function" OFF)
if (ASURA_FUNCPROF)
    target_compile_definitions(asura PRIVATE ASURA_FUNCPROF)
    set_property(SOURCE ${PPC_SRC} APPEND PROPERTY COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/funcprof.h")
endif()
set_source_files_properties(${HOST_SRC} PROPERTIES COMPILE_DEFINITIONS "NOMINMAX;_CRT_SECURE_NO_WARNINGS" COMPILE_OPTIONS "/O2;-msse4.1;/clang:-fno-strict-aliasing;-Wno-unused-function;-Wno-unused-variable")

# FFmpeg (LGPL, shared): the XMA decoder. thirdparty\ffmpeg is the BtbN "lgpl-shared" Windows build; its DLLs go next to the exe.
set(FFMPEG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/ffmpeg)
target_include_directories(asura PRIVATE ${FFMPEG_DIR}/include)
target_link_directories(asura PRIVATE ${FFMPEG_DIR}/lib)
file(GLOB FFMPEG_DLLS ${FFMPEG_DIR}/bin/avcodec-*.dll ${FFMPEG_DIR}/bin/avutil-*.dll ${FFMPEG_DIR}/bin/swresample-*.dll)
add_custom_command(TARGET asura POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:asura>)

# Setup (setup.h): finds and verifies the player's game files. Its own small program, so it builds and runs without the recompiled code.
add_executable(asura_setup WIN32 setup.cpp)
set_source_files_properties(setup.cpp PROPERTIES COMPILE_DEFINITIONS "NOMINMAX;_CRT_SECURE_NO_WARNINGS;UNICODE;_UNICODE" COMPILE_OPTIONS "/O2")
target_link_libraries(asura_setup PRIVATE user32 ole32 shell32 bcrypt comdlg32 uuid)
target_link_options(asura_setup PRIVATE /ENTRY:wmainCRTStartup)
