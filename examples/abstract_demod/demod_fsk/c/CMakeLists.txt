cmake_minimum_required(VERSION 2.8.0 FATAL_ERROR)

project(demod_fsk)

if(NOT DEFINED CC_ARCH)
    set(CC_ARCH "native")
endif()
if(NOT DEFINED CC_MARCH)
    set(CC_MARCH ${CC_ARCH})
endif()
if(NOT DEFINED CC_MTUNE)
    set(CC_MTUNE ${CC_ARCH})
endif()

set(CMAKE_WARN_DEPRECATED       OFF CACHE BOOL "" FORCE)
add_compile_options(-Wno-deprecated-declarations)

message(STATUS "Architecture : ${CC_ARCH}  march=${CC_MARCH}  mtune=${CC_MTUNE}")

set(CMAKE_C_FLAGS_RELEASE "-Wno-deprecated-declarations -Ofast -march=${CC_MARCH} -mtune=${CC_MTUNE}")

add_executable(demod_fsk
    main.c
    dsp.c
    fobos.c
    ringbufer.c
    receiver.c
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR})
find_package(Liquid REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_search_module(FOBOS REQUIRED libfobos_sdr IMPORTED_TARGET)

include_directories(PkgConfig::FOBOS)
link_libraries(PkgConfig::FOBOS)

message(STATUS "LIQUIDDSP_INCLUDE_DIRS: ${LIQUIDDSP_INCLUDE_DIRS}")
message(STATUS "LIQUIDDSP_LIBRARIES: ${LIQUIDDSP_LIBRARIES}")
message(STATUS "FOBOS_INCLUDE_DIRS: ${FOBOS_INCLUDE_DIRS}")
message(STATUS "FOBOS_LIBRARIES: ${FOBOS_LIBRARIES}")

include_directories(${LIQUIDDSP_INCLUDE_DIR})
target_link_libraries(demod_fsk ${LIQUIDDSP_LIBRARIES})
target_link_libraries(demod_fsk ${FOBOS_LIBRARIES})
target_link_libraries(demod_fsk pthread)
target_link_libraries(demod_fsk m)

install(TARGETS demod_fsk DESTINATION bin)

add_executable(test_demod_fsk test_demod_fsk.c)
target_link_libraries(test_demod_fsk m)

add_executable(test_fsk_e2e test_fsk_e2e.c)
target_link_libraries(test_fsk_e2e ${LIQUIDDSP_LIBRARIES} m)
