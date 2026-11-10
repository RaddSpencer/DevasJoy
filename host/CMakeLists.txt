cmake_minimum_required(VERSION 3.20)
project(asura_host CXX)
set(CMAKE_CXX_STANDARD 20)

# The recompiled game code is compiled straight into the exe so generated/import_stubs.cpp can override the weak import thunks.
file(GLOB PPC_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../ppc/*.cpp")
set(HOST_SRC main.cpp kernel.cpp kfile.cpp kstring.cpp xam.cpp gpu.cpp audio.cpp hooks.cpp generated/import_stubs.cpp)
add_executable(asura ${HOST_SRC} ${PPC_SRC})
target_include_directories(asura PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ppc
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    C:/dev/XenonRecomp/thirdparty/simde)
target_link_libraries(asura PRIVATE user32 gdi32 ole32 shell32 bcrypt xinput winmm)
target_link_options(asura PRIVATE /MAP:asura.map)
set(ASURA_GUEST_OPT "/O2" CACHE STRING "Optimisation flag for the recompiled game code")
# guest memory is shared between threads and read through byte-swapping macros, so strict aliasing stays off (as in BCR2)
set_source_files_properties(${PPC_SRC} PROPERTIES COMPILE_OPTIONS "${ASURA_GUEST_OPT};/w;-msse4.1;/clang:-fno-strict-aliasing")
set_source_files_properties(${HOST_SRC} PROPERTIES COMPILE_DEFINITIONS "NOMINMAX;_CRT_SECURE_NO_WARNINGS" COMPILE_OPTIONS "/O2;-msse4.1;/clang:-fno-strict-aliasing;-Wno-unused-function;-Wno-unused-variable")
