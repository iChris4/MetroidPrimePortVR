# mp_port_test: one standalone regression test (tests/*.cpp) and its ctest entry.
#
#   mp_port_test(<name>
#       SOURCES <file>...            the test and the production sources it exercises
#       [INCLUDES <dir>...]          PRIVATE include directories
#       [DEFINITIONS <def>...]       PRIVATE compile definitions
#       [LIBRARIES <lib>...]         PRIVATE link libraries
#       [ARGS <arg>...]              command-line arguments for the test run
#       [TIMEOUT <seconds>]          default 15
#       [LABELS <label>...]          default "port"
#       [PROPERTIES <prop> <value>...]  further set_tests_properties pairs
#       [CXX_STANDARD <n>]
#       [EXACT_IEEE]                 strict IEEE arithmetic (no fast-math, no FMA contraction),
#                                    for tests that compare bit-exact against retail values
#       [GC_SECTIONS]                drop unreferenced functions at link time, for tests that
#                                    build a decomp source whose other members need globals
#                                    the test never provides
#       [NO_TEST])                   build the executable but register no test
#
# The executable is named <name>, and so is the test. Windows gets the runtime DLLs copied
# next to it.
function(mp_port_test name)
  cmake_parse_arguments(PARSE_ARGV 1 T "EXACT_IEEE;GC_SECTIONS;NO_TEST" "TIMEOUT;CXX_STANDARD"
      "SOURCES;INCLUDES;DEFINITIONS;LIBRARIES;ARGS;LABELS;PROPERTIES")
  if(T_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "mp_port_test(${name}): unknown arguments ${T_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT T_SOURCES)
    message(FATAL_ERROR "mp_port_test(${name}): SOURCES is required")
  endif()
  add_executable(${name} ${T_SOURCES})
  # cl.exe's traditional preprocessor splits a raw string passed to a macro at its
  # first \" (CHECK_MSG(R"({"a\"b"})")), issue #170; the conforming one does not.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>)
  endif()
  if(T_INCLUDES)
    target_include_directories(${name} PRIVATE ${T_INCLUDES})
  endif()
  if(T_DEFINITIONS)
    target_compile_definitions(${name} PRIVATE ${T_DEFINITIONS})
  endif()
  if(T_LIBRARIES)
    target_link_libraries(${name} PRIVATE ${T_LIBRARIES})
  endif()
  if(T_CXX_STANDARD)
    set_target_properties(${name} PROPERTIES CXX_STANDARD ${T_CXX_STANDARD})
  endif()
  if(T_EXACT_IEEE)
    if(MSVC)
      target_compile_options(${name} PRIVATE /fp:strict)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${name} PRIVATE -fno-fast-math -ffp-contract=off)
    endif()
  endif()
  if(T_GC_SECTIONS)
    # lld-link resolves symbols before /OPT:REF, so the unreferenced ones only warn there.
    if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
      target_compile_options(${name} PRIVATE /Gy)
      target_link_options(${name} PRIVATE /OPT:REF /FORCE:UNRESOLVED)
    elseif(APPLE)
      target_link_options(${name} PRIVATE -Wl,-dead_strip)
    else()
      target_compile_options(${name} PRIVATE -ffunction-sections -fdata-sections)
      target_link_options(${name} PRIVATE -Wl,--gc-sections)
    endif()
  endif()
  if(T_NO_TEST)
    return()
  endif()
  if(NOT DEFINED T_TIMEOUT)
    set(T_TIMEOUT 15)
  endif()
  if(NOT T_LABELS)
    set(T_LABELS port)
  endif()
  add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
  set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT} LABELS "${T_LABELS}"
      ${T_PROPERTIES})
  aurora_copy_runtime_dlls(${name})
endfunction()
