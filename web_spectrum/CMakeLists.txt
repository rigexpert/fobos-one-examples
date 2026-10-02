# FobosOne Web Spectrum Analyzer — build & install.
#
#   cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local/fobos/web_spectrum
#   cmake --build build -j
#   sudo cmake --install build          # installs files + systemd unit, enables service
#
# Builds BOTH backends: the C++ spectrumd binary and app.py compiled to bytecode
# (app.pyc). The installed launcher (launch.sh) runs whichever backend.conf selects;
# the app's Configure tab switches between them.
cmake_minimum_required(VERSION 3.16)
project(fobos_web_spectrum LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT CMAKE_INSTALL_PREFIX OR CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
  set(CMAKE_INSTALL_PREFIX "/usr/local/fobos/web_spectrum" CACHE PATH "install prefix" FORCE)
endif()

# ── C++ backend (spectrumd) ─────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_library(FOBOS_SDR_LIB fobos_sdr REQUIRED)
find_library(FFTW3F_LIB fftw3f REQUIRED)
find_path(FOBOS_SDR_INC fobos_sdr.h PATHS /usr/local/include REQUIRED)

add_executable(spectrumd
  cpp/main.cpp
  cpp/routes.cpp
  cpp/worker.cpp
  cpp/dsp.cpp
  cpp/frame.cpp
  cpp/state.cpp
  cpp/fobos.cpp
  cpp/broadcast.cpp
  cpp/calibration.cpp
  cpp/backend_switch.cpp
  cpp/wshttp.cpp
  cpp/json.cpp
  cpp/log.cpp
  cpp/timeutil.cpp)
target_compile_options(spectrumd PRIVATE -O3 -Wall)
target_include_directories(spectrumd PRIVATE ${FOBOS_SDR_INC} ${CMAKE_SOURCE_DIR}/cpp)
target_link_libraries(spectrumd PRIVATE
  ${FOBOS_SDR_LIB} ${FFTW3F_LIB} OpenSSL::Crypto ZLIB::ZLIB pthread)

# ── Python backend -> bytecode (app.pyc) ────────────────────────────────────
find_program(PYTHON3_EXE python3 REQUIRED)
set(APP_PYC ${CMAKE_BINARY_DIR}/app.pyc)
add_custom_command(
  OUTPUT ${APP_PYC}
  COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/cmake/compile_pyc.py
          ${CMAKE_SOURCE_DIR}/python/app.py ${APP_PYC}
  DEPENDS ${CMAKE_SOURCE_DIR}/python/app.py ${CMAKE_SOURCE_DIR}/cmake/compile_pyc.py
  VERBATIM
  COMMENT "Compiling app.py -> app.pyc (optimized bytecode)")
add_custom_target(app_pyc ALL DEPENDS ${APP_PYC})

# ── Install layout under ${CMAKE_INSTALL_PREFIX} ────────────────────────────
install(TARGETS spectrumd RUNTIME DESTINATION bin)
install(FILES ${APP_PYC} DESTINATION python)
install(DIRECTORY templates DESTINATION .)
install(DIRECTORY static    DESTINATION .)
install(PROGRAMS launch.sh  DESTINATION .)

# calibration tooling (scripts) — the CAL tab's orchestrator + helpers
install(DIRECTORY ${CMAKE_SOURCE_DIR}/calibration/ DESTINATION calibration
        FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)

# Service user (defaults to the building user; never root) — baked into the unit and the
# .deb maintainer scripts.
if(NOT INSTALL_USER)
  set(INSTALL_USER "$ENV{USER}")
endif()
if(NOT INSTALL_USER OR INSTALL_USER STREQUAL "root")
  set(INSTALL_USER "pi5")
endif()

# ── systemd service ─────────────────────────────────────────────────────────
configure_file(${CMAKE_SOURCE_DIR}/systemd/fobos-web-spectrum.service.in
               ${CMAKE_BINARY_DIR}/fobos-web-spectrum.service @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/fobos-web-spectrum.service DESTINATION /etc/systemd/system)

# Runtime setup for a DIRECT `cmake --install`. Skipped while CPack stages into DESTDIR;
# for the .deb the maintainer scripts (below) do this on the target instead.
install(CODE "
  if(\"\$ENV{DESTDIR}\" STREQUAL \"\")
    if(NOT EXISTS \"${CMAKE_INSTALL_PREFIX}/backend.conf\")
      file(WRITE \"${CMAKE_INSTALL_PREFIX}/backend.conf\" \"cpp\\n\")
    endif()
    if(NOT EXISTS \"${CMAKE_INSTALL_PREFIX}/calibration/fobos_calibration.json\"
       AND EXISTS \"${CMAKE_SOURCE_DIR}/calibration/fobos_calibration.json\")
      file(COPY \"${CMAKE_SOURCE_DIR}/calibration/fobos_calibration.json\"
           DESTINATION \"${CMAKE_INSTALL_PREFIX}/calibration\")
    endif()
    execute_process(COMMAND chown -R ${INSTALL_USER} \"${CMAKE_INSTALL_PREFIX}\")
    execute_process(COMMAND systemctl daemon-reload)
    execute_process(COMMAND systemctl enable fobos-web-spectrum.service)
    message(STATUS \"Installed to ${CMAKE_INSTALL_PREFIX}. Start: sudo systemctl restart fobos-web-spectrum\")
  endif()
")

# ── Debian package (CPack) ──────────────────────────────────────────────────
#   cmake --build build --target package    ->  fobos-web-spectrum_<ver>_<arch>.deb
configure_file(${CMAKE_SOURCE_DIR}/debian/postinst.in ${CMAKE_BINARY_DIR}/debian/postinst @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/debian/prerm.in    ${CMAKE_BINARY_DIR}/debian/prerm    @ONLY)
configure_file(${CMAKE_SOURCE_DIR}/debian/postrm.in   ${CMAKE_BINARY_DIR}/debian/postrm   @ONLY)
file(CHMOD ${CMAKE_BINARY_DIR}/debian/postinst ${CMAKE_BINARY_DIR}/debian/prerm
           ${CMAKE_BINARY_DIR}/debian/postrm
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                 WORLD_READ WORLD_EXECUTE)

execute_process(COMMAND dpkg --print-architecture
                OUTPUT_VARIABLE DEB_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT DEB_ARCH)
  set(DEB_ARCH "arm64")
endif()

set(CPACK_GENERATOR "DEB")
# Land the (relative) install destinations under this prefix in the package; the systemd
# unit (an absolute destination) stays at /etc/systemd/system.
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local/fobos/web_spectrum")
set(CPACK_PACKAGE_NAME "fobos-web-spectrum")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_CONTACT "Serhii Nedosiekov <sergey.nedosekov@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "FobosOne web spectrum analyzer (native C++ + Python backends)")
set(CPACK_DEBIAN_PACKAGE_SECTION "hamradio")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${DEB_ARCH}")
# libfobos_sdr is not a packaged dependency; it is checked for in postinst instead.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libc6, libstdc++6, libfftw3-single3, libssl3, zlib1g, python3, python3-numpy, python3-flask, python3-flask-socketio")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_BINARY_DIR}/debian/postinst;${CMAKE_BINARY_DIR}/debian/prerm;${CMAKE_BINARY_DIR}/debian/postrm")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
include(CPack)
