# ============================================================================
#  FobosOne SDR — top-level "build everything" project
# ----------------------------------------------------------------------------
#  Aggregates every buildable component of the repository behind one build:
#
#    * examples/*         — the CMake C examples (added as subprojects)
#    * examples + tools   — the Makefile-based C harness tools (custom targets)
#    * web_spectrum       — the C++ backend + Python bytecode (isolated build)
#
#  Each component still builds standalone from its own directory exactly as
#  documented in its README; this file is only a convenience that drives them
#  all at once.
#
#  Usage:
#      cmake -B build
#      cmake --build build -j
#
#  Options (all default ON):
#      -DBUILD_EXAMPLES=ON       the CMake C examples
#      -DBUILD_TOOLS=ON          the Makefile C tools (OTA loopback, cal_sweep)
#      -DBUILD_WEB_SPECTRUM=ON   the web_spectrum backend (its own CMake build)
#
#  Note: web_spectrum keeps its own install / systemd / .deb packaging flow.
#  Install it from web_spectrum/ directly (see web_spectrum/README.md); this
#  top-level build only compiles it, it does not install or package it.
# ============================================================================

cmake_minimum_required(VERSION 3.13)

project(fobosone
    LANGUAGES C
    DESCRIPTION "FobosOne SDR examples, tools and web_spectrum backend"
)

option(BUILD_EXAMPLES     "Build the CMake C examples"                       ON)
option(BUILD_TOOLS        "Build the Makefile C tools (OTA loopback, cal)"   ON)
option(BUILD_WEB_SPECTRUM "Build web_spectrum (C++ backend + app.pyc)"       ON)

# Shared architecture flags honoured by the example subprojects. Override on
# the command line for cross-compilation (e.g. -DCC_MARCH=cortex-a76).
if(NOT DEFINED CC_ARCH)
    set(CC_ARCH "native" CACHE STRING "Target CPU for -march/-mtune")
endif()

# ----------------------------------------------------------------------------
#  1. CMake C examples (real subprojects — built as part of ALL)
# ----------------------------------------------------------------------------
#  The demod_* examples additionally require liquid-dsp; they are skipped
#  (with a notice) rather than failing the whole configure when it is absent.
if(BUILD_EXAMPLES)
    add_subdirectory(examples/fft/c           examples/fft)
    add_subdirectory(examples/fft-scan/c      examples/fft-scan)
    add_subdirectory(examples/detector/c      examples/detector)

    find_library(LIQUID_LIBRARY NAMES liquid)
    find_path(LIQUID_INCLUDE_DIR NAMES liquid/liquid.h)
    if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
        add_subdirectory(examples/demod_fm/c                 examples/demod_fm)
        add_subdirectory(examples/abstract_demod/demod_fsk/c examples/demod_fsk)
        add_subdirectory(examples/abstract_demod/demod_psk/c examples/demod_psk)
    else()
        message(WARNING
            "liquid-dsp not found — skipping demod_fm / demod_fsk / demod_psk. "
            "Install liquid-dsp (https://github.com/jgaeddert/liquid-dsp) to build them.")
    endif()
endif()

# ----------------------------------------------------------------------------
#  2. Makefile-based C tools (driven via custom targets)
# ----------------------------------------------------------------------------
#  These ship their own Makefile and build in-place inside their c/ directory,
#  so we simply invoke `make` on each as part of the default build.
if(BUILD_TOOLS)
    find_program(MAKE_PROGRAM NAMES make gmake REQUIRED)

    function(add_make_tool tgt subdir comment)
        add_custom_target(${tgt} ALL
            COMMAND ${MAKE_PROGRAM} -C ${CMAKE_SOURCE_DIR}/${subdir}
            COMMENT "${comment}"
            USES_TERMINAL
            VERBATIM
        )
    endfunction()

    add_make_tool(ota_fsk_loopback examples/aaronia_OTA/fsk_loopback/c
        "Building OTA FSK loopback tools (capture, gen_fsk, demod_fsk_file)")
    add_make_tool(ota_psk_loopback examples/aaronia_OTA/psk_loopback/c
        "Building OTA PSK loopback tools (capture, gen_psk, demod_psk_file)")
    add_make_tool(cal_sweep tools/cal_sweep/c
        "Building cal_sweep tool (measure)")
endif()

# ----------------------------------------------------------------------------
#  3. web_spectrum (self-contained CMake build, kept isolated)
# ----------------------------------------------------------------------------
#  web_spectrum carries its own install / systemd / CPack .deb flow, so we do
#  NOT add_subdirectory() it (that would merge those rules into this project).
#  Instead we configure and build it in its own build tree under this one.
if(BUILD_WEB_SPECTRUM)
    set(WEB_SPECTRUM_BUILD ${CMAKE_BINARY_DIR}/web_spectrum)
    add_custom_target(web_spectrum ALL
        COMMAND ${CMAKE_COMMAND}
                -S ${CMAKE_SOURCE_DIR}/web_spectrum
                -B ${WEB_SPECTRUM_BUILD}
                -DCMAKE_BUILD_TYPE=Release
        COMMAND ${CMAKE_COMMAND} --build ${WEB_SPECTRUM_BUILD}
        COMMENT "Building web_spectrum (C++ spectrumd + app.pyc)"
        USES_TERMINAL
        VERBATIM
    )
endif()

# ----------------------------------------------------------------------------
#  Configure-time summary
# ----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "FobosOne build configuration:")
message(STATUS "  CMake examples ....... ${BUILD_EXAMPLES}")
message(STATUS "  Makefile tools ....... ${BUILD_TOOLS}")
message(STATUS "  web_spectrum ......... ${BUILD_WEB_SPECTRUM}")
message(STATUS "  Target CPU (CC_ARCH) . ${CC_ARCH}")
message(STATUS "")
