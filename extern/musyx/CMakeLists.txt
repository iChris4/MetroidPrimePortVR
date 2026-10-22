cmake_minimum_required(VERSION 3.13)
project(MusyX)

set(MUSY_TARGET MUSY_TARGET_PC)

set(CMAKE_C_STANDARD 23)
add_library(musyx STATIC
        src/musyx/runtime/seq.c
        src/musyx/runtime/synth.c
        src/musyx/runtime/seq_api.c
        src/musyx/runtime/snd_synthapi.c
        src/musyx/runtime/stream.c
        src/musyx/runtime/synthdata.c
        src/musyx/runtime/synthmacros.c
        src/musyx/runtime/synthvoice.c
        src/musyx/runtime/synth_ac.c
        src/musyx/runtime/synth_adsr.c
        src/musyx/runtime/synth_vsamples.c
        src/musyx/runtime/synth_dbtab.c
        src/musyx/runtime/s_data.c
        src/musyx/runtime/hw_dspctrl.c
        src/musyx/runtime/hw_volconv.c
        src/musyx/runtime/snd3d.c
        src/musyx/runtime/snd_init.c
        src/musyx/runtime/snd_math.c
        src/musyx/runtime/snd_midictrl.c
        src/musyx/runtime/snd_service.c
        src/musyx/runtime/hardware.c
        src/musyx/runtime/hw_aramdma.c
        src/musyx/runtime/dsp_import.c
        src/musyx/runtime/hw_dolphin.c
        src/musyx/runtime/hw_pc.c
        src/musyx/runtime/hw_memory.c
        src/musyx/runtime/hw_lib_dolphin.c
        src/musyx/runtime/CheapReverb/creverb_fx.c
        src/musyx/runtime/CheapReverb/creverb.c
        src/musyx/runtime/StdReverb/reverb_fx.c
        src/musyx/runtime/StdReverb/reverb.c
        src/musyx/runtime/Delay/delay_fx.c
        src/musyx/runtime/Chorus/chorus_fx.c
)

target_include_directories(musyx PUBLIC include)
if (CMAKE_SYSTEM_NAME STREQUAL Linux)
target_compile_options(musyx PRIVATE -Wno-pointer-to-int-cast -gdwarf-4)
# GCC only: clang has no -Wstringop-overflow and warns about the unknown option.
if (CMAKE_C_COMPILER_ID STREQUAL GNU)
target_compile_options(musyx PRIVATE -Wno-stringop-overflow)
endif ()
else ()
target_compile_options(musyx PRIVATE)
endif ()
target_compile_definitions(musyx PUBLIC
        -DMUSY_TARGET=${MUSY_TARGET}
        $<$<CONFIG:Release>:NDEBUG>
        $<$<CONFIG:MinSizeRel>:NDEBUG>)
set_target_properties(musyx PROPERTIES DEBUG_POSTFIX D)
set_target_properties(musyx PROPERTIES RELWITHDEBINFO_POSTFIX RD)
set_target_properties(musyx PROPERTIES MINSIZEREL_POSTFIX MIN)

# SDL3 is normally provided by Aurora (FetchContent) when MusyX is built as
# part of the port; fall back to a standalone find_package otherwise.
if (DEFINED AURORA_SDL3_TARGET AND TARGET ${AURORA_SDL3_TARGET})
  set(MUSYX_SDL3_TARGET ${AURORA_SDL3_TARGET})
elseif (TARGET SDL3::SDL3-static)
  set(MUSYX_SDL3_TARGET SDL3::SDL3-static)
elseif (TARGET SDL3::SDL3)
  set(MUSYX_SDL3_TARGET SDL3::SDL3)
else ()
  find_package(SDL3 REQUIRED)
  set(MUSYX_SDL3_TARGET SDL3::SDL3)
endif ()
target_link_libraries(musyx PUBLIC ${MUSYX_SDL3_TARGET})

option(MUSYX_BUILD_PLAYER "Build the standalone MusyX sample player" OFF)
if(MUSYX_BUILD_PLAYER)
  add_executable(musyx_player test/main.c)
  target_link_libraries(musyx_player PRIVATE musyx)
  if(NOT MSVC)
    target_link_libraries(musyx_player PRIVATE m)
  endif()
endif()
