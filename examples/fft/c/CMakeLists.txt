cmake_minimum_required(VERSION 2.8.0 FATAL_ERROR)

project(fft)

if(NOT DEFINED CC_ARCH)
    set(CC_ARCH "native")
endif()
# CC_MARCH / CC_MTUNE can be set independently for cross-compilation.
# When not specified they both fall back to CC_ARCH (native build behaviour).
if(NOT DEFINED CC_MARCH)
    set(CC_MARCH ${CC_ARCH})
endif()
if(NOT DEFINED CC_MTUNE)
    set(CC_MTUNE ${CC_ARCH})
endif()

SET(CMAKE_WARN_DEPRECATED       OFF CACHE BOOL "" FORCE)
add_compile_options(-Wno-deprecated-declarations)

message(STATUS "Architecture : ${CC_ARCH}  march=${CC_MARCH}  mtune=${CC_MTUNE}")

set(CMAKE_C_FLAGS_RELEASE "-Wno-deprecated-declarations -Ofast -march=${CC_MARCH} -mtune=${CC_MTUNE}")

add_executable(fft
	fft.c
    main.c
    fobos.c
    receiver.c
    ringbufer.c
    draw.c
    sgram.c
    spectrum_png.c
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR})

find_package(PkgConfig REQUIRED)
pkg_search_module(FFTWF REQUIRED fftw3f IMPORTED_TARGET)
pkg_search_module(FOBOS REQUIRED libfobos_sdr IMPORTED_TARGET)
pkg_search_module(PNG   REQUIRED libpng     IMPORTED_TARGET)

include_directories(PkgConfig::FFTWF)
link_libraries(PkgConfig::FFTWF)
include_directories(PkgConfig::FOBOS)
link_libraries(PkgConfig::FOBOS)

message(STATUS "FFTWF_INCLUDE_DIRS: ${FFTWF_INCLUDE_DIRS}")
message(STATUS "FFTWF_LIBRARIES: ${FFTWF_LIBRARIES}")

message(STATUS "FOBOS_INCLUDE_DIRS: ${FOBOS_INCLUDE_DIRS}")
message(STATUS "FOBOS_LIBRARIES: ${FOBOS_LIBRARIES}")


target_link_libraries(fft ${FFTWF_LIBRARIES})
target_link_libraries(fft ${FOBOS_LIBRARIES})

target_link_libraries(fft ${PNG_LIBRARIES})

# sys math lib + threading (required by receiver.c)
target_link_libraries(fft m pthread)

install(TARGETS fft DESTINATION bin)
