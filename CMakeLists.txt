cmake_minimum_required(VERSION 3.20)
project(BeyondOasisRecomp C CXX)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- SDL2 (static) ----------------------------------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sdl2 EXCLUDE_FROM_ALL)

# ---- Musashi 68000 interpreter (fallback for code not yet recompiled) --------
set(MUSASHI_DIR ${CMAKE_SOURCE_DIR}/third_party/musashi)
set(MUSASHI_GEN ${CMAKE_BINARY_DIR}/musashi_gen)
add_executable(m68kmake ${MUSASHI_DIR}/m68kmake.c)
add_custom_command(
  OUTPUT ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MUSASHI_GEN}
  COMMAND m68kmake ${MUSASHI_GEN} ${MUSASHI_DIR}/m68k_in.c
  DEPENDS m68kmake ${MUSASHI_DIR}/m68k_in.c)
add_library(musashi STATIC
  ${MUSASHI_DIR}/m68kcpu.c
  ${MUSASHI_DIR}/m68kdasm.c
  ${MUSASHI_DIR}/softfloat/softfloat.c
  ${MUSASHI_GEN}/m68kops.c)
target_include_directories(musashi PUBLIC ${MUSASHI_DIR} ${MUSASHI_GEN} ${CMAKE_SOURCE_DIR}/runtime/src)
target_compile_definitions(musashi PUBLIC "MUSASHI_CNF=\"musashi_conf.h\"")
target_compile_options(musashi PRIVATE -w)

# ---- Sound: Z80 core and YM2612 --------------------------------------------------
add_library(z80core STATIC third_party/z80/z80.c)
target_include_directories(z80core PUBLIC third_party/z80)
target_compile_options(z80core PRIVATE -w)
add_library(ymfm STATIC third_party/ymfm/src/ymfm_opn.cpp third_party/ymfm/src/ymfm_ssg.cpp
  third_party/ymfm/src/ymfm_adpcm.cpp third_party/ymfm/src/ymfm_misc.cpp)
target_include_directories(ymfm PUBLIC third_party/ymfm/src)
target_compile_options(ymfm PRIVATE -w)

# ---- Recompiled game code (generated from the user's ROM) -------------------------
set(RECOMP_SOURCES ${CMAKE_SOURCE_DIR}/generated/recomp_table.c)
foreach(i RANGE 0 15)
  if(i LESS 10)
    set(id "0${i}")
  else()
    set(id "${i}")
  endif()
  list(APPEND RECOMP_SOURCES ${CMAKE_SOURCE_DIR}/generated/recomp_${id}.c)
endforeach()
file(GLOB RECOMP_TOOLS ${CMAKE_SOURCE_DIR}/tools/*.mjs ${CMAKE_SOURCE_DIR}/tools/lib/*.mjs)
add_custom_command(
  OUTPUT ${RECOMP_SOURCES}
  COMMAND node ${CMAKE_SOURCE_DIR}/tools/recomp.mjs
  DEPENDS ${RECOMP_TOOLS}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Recompiling the ROM to C")
add_library(recomp STATIC ${RECOMP_SOURCES})
target_include_directories(recomp PRIVATE runtime/src)
target_link_libraries(recomp PRIVATE musashi)
target_compile_options(recomp PRIVATE -O1 -w)

# ---- Runtime ------------------------------------------------------------------
add_executable(BeyondOasis
  runtime/src/main.c
  runtime/src/machine.c
  runtime/src/bus.c
  runtime/src/vdp.c
  runtime/src/io.c
  runtime/src/settings.c
  runtime/src/z80bus.c
  runtime/src/sound.cpp)
target_link_libraries(BeyondOasis PRIVATE recomp musashi z80core ymfm SDL2::SDL2main SDL2::SDL2-static)
target_compile_options(BeyondOasis PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(WIN32)
  target_link_options(BeyondOasis PRIVATE -static)
endif()
