cmake_minimum_required(VERSION 3.20)
project(asura_recomp_check LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PPC_DIR "${CMAKE_CURRENT_LIST_DIR}/../ppc")
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS "${PPC_DIR}/*.cpp")

add_library(asura_ppc STATIC ${PPC_SOURCES})

target_include_directories(asura_ppc PRIVATE
    "${PPC_DIR}"
    "C:/dev/XenonRecomp/thirdparty/simde")

# /Od: no optimization, so a first compile-check is fast. /w: silence warnings so errors are easy to see.
# -msse4.1: lets simde use native SSE4.1 for the vector code (my guess at a sensible baseline).
target_compile_options(asura_ppc PRIVATE /Od /w -msse4.1)