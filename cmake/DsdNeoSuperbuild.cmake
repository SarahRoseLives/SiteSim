# DsdNeoSuperbuild.cmake — build the vendored mbelib-neo and dsd-neo from source
# (third_party/) and expose the dsd-neo static libraries SiteSim links against.
#
# Rationale: dsd-neo builds as a CLI app and does not export a consumable CMake
# package, so we drive its build with ExternalProject and reference the produced
# static archives directly.  mbelib-neo *does* install a CMake package, which
# dsd-neo consumes via CMAKE_PREFIX_PATH.
#
# Provides:
#   sitesim::dsdneo   INTERFACE target — link this to get the full RX backend
#   SITESIM_HAVE_DSDNEO=ON
#
# Requires (found by the caller / dsd-neo's own CMake):
#   mingw/gcc toolchain, libsndfile, openssl, ncurses-or-portaudio, librtlsdr,
#   SoapySDR, libusb — all available in MSYS2 mingw64.

include(ExternalProject)

set(_VENDOR_DIR   ${CMAKE_SOURCE_DIR}/third_party)
set(_MBE_SRC      ${_VENDOR_DIR}/mbelib-neo)
set(_DSD_SRC      ${_VENDOR_DIR}/dsd-neo)
set(_DEPS_PREFIX  ${CMAKE_BINARY_DIR}/vendor-prefix)

# mbe-neo import/shared library name differs per platform.
if(WIN32)
    set(_MBE_LIB ${_DEPS_PREFIX}/lib/libmbe-neo.dll.a)   # mingw import lib
else()
    set(_MBE_LIB ${_DEPS_PREFIX}/lib/libmbe-neo.so)       # ELF shared object
endif()

# These directories are populated by the ExternalProject builds below, but must
# exist at configure time so the imported target's INTERFACE_INCLUDE_DIRECTORIES
# validation passes.
file(MAKE_DIRECTORY ${_DEPS_PREFIX}/include)
file(MAKE_DIRECTORY ${_DEPS_PREFIX}/lib)

# ── mbelib-neo ───────────────────────────────────────────────────────────────
ExternalProject_Add(mbelib_neo_ep
    SOURCE_DIR   ${_MBE_SRC}
    PREFIX       ${CMAKE_BINARY_DIR}/mbelib-neo-ep
    INSTALL_DIR  ${_DEPS_PREFIX}
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=${_DEPS_PREFIX}
        -DMBELIB_BUILD_TESTS=OFF
        -DMBELIB_BUILD_EXAMPLES=OFF
        -DMBELIB_WARNINGS_AS_ERRORS=OFF
    BUILD_BYPRODUCTS
        ${_MBE_LIB}
    UPDATE_COMMAND ""
)

# ── dsd-neo (libraries only; the CLI app/tests are not built) ────────────────
# dsd-neo has no "libraries only" switch, so we build just the archive targets
# SiteSim needs.  BUILD_TESTING=OFF drops the test tree; we never invoke the
# dsd-cli link step, sidestepping its curses/link-order requirements.
set(_DSD_BUILD ${CMAKE_BINARY_DIR}/dsd-neo-ep/src/dsd_neo_ep-build)

set(_DSD_LIB_TARGETS
    dsd-neo_engine dsd-neo_dispatch dsd-neo_core dsd-neo_runtime
    dsd-neo_proto_p25 dsd-neo_io_radio dsd-neo_io_control dsd-neo_io_audio
    dsd-neo_dsp dsd-neo_fec dsd-neo_crypto dsd-neo_platform
    dsd-neo_io_iq dsd-neo_io_udp_control dsd-neo_pffft
    dsd-neo_proto_dmr dsd-neo_proto_dpmr dsd-neo_proto_dstar
    dsd-neo_proto_edacs dsd-neo_proto_m17 dsd-neo_proto_nxdn
    dsd-neo_proto_provoice dsd-neo_proto_x2tdma dsd-neo_proto_ysf
)

# Static archive locations within the dsd-neo build tree, in link order
# (engine/dispatch first; leaf libs like platform/fec/crypto/pffft last).
set(_DSD_LIBS
    ${_DSD_BUILD}/src/engine/libdsd-neo_engine.a
    ${_DSD_BUILD}/src/engine/libdsd-neo_dispatch.a
    ${_DSD_BUILD}/src/protocol/p25/libdsd-neo_proto_p25.a
    ${_DSD_BUILD}/src/protocol/dmr/libdsd-neo_proto_dmr.a
    ${_DSD_BUILD}/src/protocol/dpmr/libdsd-neo_proto_dpmr.a
    ${_DSD_BUILD}/src/protocol/dstar/libdsd-neo_proto_dstar.a
    ${_DSD_BUILD}/src/protocol/edacs/libdsd-neo_proto_edacs.a
    ${_DSD_BUILD}/src/protocol/m17/libdsd-neo_proto_m17.a
    ${_DSD_BUILD}/src/protocol/nxdn/libdsd-neo_proto_nxdn.a
    ${_DSD_BUILD}/src/protocol/provoice/libdsd-neo_proto_provoice.a
    ${_DSD_BUILD}/src/protocol/x2tdma/libdsd-neo_proto_x2tdma.a
    ${_DSD_BUILD}/src/protocol/ysf/libdsd-neo_proto_ysf.a
    ${_DSD_BUILD}/src/io/libdsd-neo_io_radio.a
    ${_DSD_BUILD}/src/io/libdsd-neo_io_control.a
    ${_DSD_BUILD}/src/io/libdsd-neo_io_audio.a
    ${_DSD_BUILD}/src/io/libdsd-neo_io_iq.a
    ${_DSD_BUILD}/src/io/libdsd-neo_io_udp_control.a
    ${_DSD_BUILD}/src/core/libdsd-neo_core.a
    ${_DSD_BUILD}/src/dsp/libdsd-neo_dsp.a
    ${_DSD_BUILD}/src/runtime/libdsd-neo_runtime.a
    ${_DSD_BUILD}/src/fec/libdsd-neo_fec.a
    ${_DSD_BUILD}/src/crypto/libdsd-neo_crypto.a
    ${_DSD_BUILD}/src/platform/libdsd-neo_platform.a
    ${_DSD_BUILD}/src/third_party/pffft/libdsd-neo_pffft.a
)

# dsd-neo audio backend: PortAudio on Windows (its default there), PulseAudio on
# Linux/macOS (dsd-neo's default). SiteSim never uses dsd-neo's audio output, but
# the backend must link.
if(WIN32)
    set(_DSD_AUDIO_ARG -DDSD_USE_PORTAUDIO=ON)
else()
    set(_DSD_AUDIO_ARG -DDSD_USE_PORTAUDIO=OFF)
endif()

ExternalProject_Add(dsd_neo_ep
    SOURCE_DIR    ${_DSD_SRC}
    PREFIX        ${CMAKE_BINARY_DIR}/dsd-neo-ep
    DEPENDS       mbelib_neo_ep
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_PREFIX_PATH=${_DEPS_PREFIX}
        -DBUILD_TESTING=OFF
        -DDSD_ENABLE_WARNINGS=OFF
        -DDSD_WARNINGS_AS_ERRORS=OFF
        -DDSD_ENABLE_TERMINAL_UI=OFF
        ${_DSD_AUDIO_ARG}
        -DDSD_ENABLE_RTLSDR=ON
        -DDSD_ENABLE_SOAPYSDR=ON
    # Build only the library targets, never the dsd-cli executable.
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target ${_DSD_LIB_TARGETS}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${_DSD_LIBS}
    UPDATE_COMMAND ""
)

# ── Imported interface for SiteSim ───────────────────────────────────────────
# External system libs dsd-neo depends on (resolved via pkg-config / find_*).
find_library(SNDFILE_LIB   NAMES sndfile libsndfile)
find_library(RTLSDR_LIB    NAMES rtlsdr librtlsdr)
find_library(USB_LIB       NAMES usb-1.0 libusb-1.0)
find_package(OpenSSL)
pkg_check_modules(PORTAUDIO portaudio-2.0)
# dsd-neo auto-detects libcurl (rdio export) and Codec2; include curl if the
# build picked it up so the runtime lib's references resolve.
find_package(CURL QUIET)

add_library(sitesim::dsdneo INTERFACE IMPORTED)
add_dependencies(sitesim::dsdneo dsd_neo_ep)

# Platform system libraries dsd-neo pulls in.
if(WIN32)
    # winsock2 (sockets), iconv (nxdn alias decode), plus the Win32 libs the
    # SoapySDR/rtlsdr device layer touches.
    find_library(ICONV_LIB NAMES iconv libiconv)
    set(_DSD_PLATFORM_LIBS ws2_32 ${ICONV_LIB} winmm setupapi ole32 uuid)
else()
    set(_DSD_PLATFORM_LIBS "")
endif()

if(CURL_FOUND)
    list(APPEND _DSD_PLATFORM_LIBS CURL::libcurl)
endif()

set_target_properties(sitesim::dsdneo PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_DSD_SRC}/include;${_DEPS_PREFIX}/include"
    # Wrap the archives in a start/end group so inter-library cyclic references
    # resolve without hand-tuning order.
    INTERFACE_LINK_LIBRARIES
        "-Wl,--start-group;${_DSD_LIBS};-Wl,--end-group;${_MBE_LIB};${SOAPY_LIBRARIES};${SNDFILE_LIB};${RTLSDR_LIB};${USB_LIB};OpenSSL::Crypto;${PORTAUDIO_LIBRARIES};${_DSD_PLATFORM_LIBS}"
)

set(SITESIM_HAVE_DSDNEO ON)
message(STATUS "SiteSim: dsd-neo backend built from vendored source — RX enabled")
