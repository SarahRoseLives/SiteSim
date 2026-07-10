cmake_minimum_required(VERSION 3.20)
project(SiteSim VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SOAPY  REQUIRED SoapySDR)
pkg_check_modules(SDL2   REQUIRED sdl2)
find_package(OpenGL REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# dsd-neo backend (vendored source superbuild)
#
# The full source of dsd-neo + mbelib-neo lives in third_party/ and is built
# from source (see cmake/DsdNeoSuperbuild.cmake), producing the RX decode
# backend.  Set SITESIM_ENABLE_DSDNEO=OFF to build the stub RX path instead
# (GUI + control-channel TX only).
# ─────────────────────────────────────────────────────────────────────────────
option(SITESIM_ENABLE_DSDNEO "Build the vendored dsd-neo RX backend from source" ON)

if(SITESIM_ENABLE_DSDNEO AND EXISTS ${CMAKE_SOURCE_DIR}/third_party/dsd-neo/CMakeLists.txt)
    include(${CMAKE_SOURCE_DIR}/cmake/DsdNeoSuperbuild.cmake)
else()
    set(SITESIM_HAVE_DSDNEO OFF)
    message(STATUS "SiteSim: dsd-neo backend disabled — building stub RX pipeline")
endif()

set(DSDNEO_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/dsd-neo/include)

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

set(NLOHMANN_DIR ${CMAKE_SOURCE_DIR}/third_party/nlohmann/single_include)

set(SITESIM_SOURCES
    src/main.cpp
    src/p25/TSBK.cpp
    src/p25/Frame.cpp
    src/p25/C4FM.cpp
    src/tx/SoapyTx.cpp
    src/tx/ControlChannel.cpp
    src/rx/RxPipeline.cpp
    src/ui/App.cpp
    src/ui/SitePanel.cpp
    src/ui/CCPanel.cpp
    src/ui/LogPanel.cpp
    ${IMGUI_SOURCES}
)

add_executable(sitesim ${SITESIM_SOURCES})

set(SITESIM_CORE_SOURCES
    src/p25/TSBK.cpp
    src/p25/Frame.cpp
    src/p25/C4FM.cpp
    src/tx/SoapyTx.cpp
    src/tx/ControlChannel.cpp
    src/rx/RxPipeline.cpp
)

add_executable(uplink_test
    src/uplink_test_main.cpp
    ${SITESIM_CORE_SOURCES}
)

target_include_directories(sitesim PRIVATE
    src/
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${NLOHMANN_DIR}
    ${SOAPY_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${DSDNEO_INCLUDE_DIR}
)

target_include_directories(uplink_test PRIVATE
    src/
    ${SOAPY_INCLUDE_DIRS}
    ${DSDNEO_INCLUDE_DIR}
)

# ── Common link libraries (all platforms) ────────────────────────────────────
target_link_libraries(sitesim PRIVATE
    ${SOAPY_LIBRARIES}
    ${SDL2_LIBRARIES}
    OpenGL::GL
)

target_link_libraries(uplink_test PRIVATE
    ${SOAPY_LIBRARIES}
)

# ── dsd-neo backend link + defines (only when available) ─────────────────────
if(SITESIM_HAVE_DSDNEO)
    target_compile_definitions(sitesim     PRIVATE SITESIM_HAVE_DSDNEO=1)
    target_compile_definitions(uplink_test PRIVATE SITESIM_HAVE_DSDNEO=1)

    # sitesim::dsdneo (from the superbuild) carries the dsd-neo static archives,
    # mbe-neo, and all transitive system deps (SoapySDR, sndfile, rtlsdr, usb,
    # OpenSSL, PortAudio) with correct include dirs and link grouping.
    target_link_libraries(sitesim     PRIVATE sitesim::dsdneo)
    target_link_libraries(uplink_test PRIVATE sitesim::dsdneo)

    # Ensure the vendored dsd-neo/mbelib superbuild finishes (headers + archives
    # produced) before SiteSim's own sources compile — avoids parallel-build
    # races on a clean -j build.
    add_dependencies(sitesim     dsd_neo_ep)
    add_dependencies(uplink_test dsd_neo_ep)

    if(NOT WIN32)
        target_link_libraries(sitesim     PRIVATE dl)
        target_link_libraries(uplink_test PRIVATE dl)
        # Find the vendored libmbe-neo.so at runtime (built into vendor-prefix).
        set_target_properties(sitesim uplink_test PROPERTIES
            BUILD_RPATH "${CMAKE_BINARY_DIR}/vendor-prefix/lib"
        )
    endif()
endif()

# ── Warning flags (GCC/Clang) ────────────────────────────────────────────────
if(NOT MSVC)
    target_compile_options(sitesim PRIVATE
        ${SOAPY_CFLAGS_OTHER}
        ${SDL2_CFLAGS_OTHER}
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    )

    target_compile_options(uplink_test PRIVATE
        ${SOAPY_CFLAGS_OTHER}
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    )
endif()

# ── Windows runtime packaging ────────────────────────────────────────────────
if(WIN32)
    # sitesim is a GUI app — build it for the Windows subsystem so no console
    # window opens. uplink_test stays a console app. SDL2main provides WinMain
    # which forwards to our SDL_main (main is renamed via SDL2 cflags).
    set_target_properties(sitesim PROPERTIES WIN32_EXECUTABLE TRUE)

    # Copy each target's mingw DLL dependencies next to the .exe after build so
    # the binaries run outside the MSYS2 shell.
    find_program(LDD_EXECUTABLE ldd)
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_mingw_root "${_mingw_bin}" DIRECTORY)

    # SoapySDR device modules (HackRF / RTL-SDR) — bundled so SiteSim can reach
    # SDR hardware standalone. Module ABI dir is SoapySDR/modules<ABI>.
    set(_soapy_module_dir "")
    file(GLOB _soapy_mod_dirs "${_mingw_root}/lib/SoapySDR/modules*")
    if(_soapy_mod_dirs)
        list(GET _soapy_mod_dirs 0 _soapy_module_dir)
    endif()

    if(LDD_EXECUTABLE)
        # dsd-neo's superbuild produces libmbe-neo.dll in the vendor prefix;
        # make the copy script search there too so it (and any of its own deps)
        # are bundled alongside the mingw DLLs.
        set(_extra_dll_dirs "${CMAKE_BINARY_DIR}/vendor-prefix/bin")
        foreach(_tgt sitesim uplink_test)
            add_custom_command(TARGET ${_tgt} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    -DEXE=$<TARGET_FILE:${_tgt}>
                    -DDEST_DIR=$<TARGET_FILE_DIR:${_tgt}>
                    -DMINGW_BIN=${_mingw_bin}
                    -DLDD=${LDD_EXECUTABLE}
                    -DSOAPY_MODULE_DIR=${_soapy_module_dir}
                    -DEXTRA_DLL_DIRS=${_extra_dll_dirs}
                    -P ${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake
                VERBATIM
                COMMENT "Copying runtime DLLs + SoapySDR modules for ${_tgt}"
            )
        endforeach()
    else()
        message(WARNING "ldd not found — runtime DLLs will not be copied automatically")
    endif()
endif()

# ── subscriber_test (Go + cgo/libhackrf) ─────────────────────────────────────
# Built via the Go toolchain (cgo links libhackrf). Optional: only wired up when
# `go` is found. Produces build/subscriber_test[.exe] alongside the C++ targets.
option(SITESIM_BUILD_SUBSCRIBER_TEST "Build the Go subscriber_test tool" ON)

if(SITESIM_BUILD_SUBSCRIBER_TEST)
    find_program(GO_EXECUTABLE go
        HINTS
            ENV GOROOT
        PATHS
            "$ENV{ProgramFiles}/Go/bin"
            "$ENV{LOCALAPPDATA}/Programs/Go/bin"
            /usr/local/go/bin
    )
    if(GO_EXECUTABLE)
        set(_subtest_src ${CMAKE_SOURCE_DIR}/subscriber_test)
        if(WIN32)
            set(_subtest_out ${CMAKE_BINARY_DIR}/subscriber_test.exe)
            # cgo needs a C compiler; use the mingw gcc next to our C++ compiler.
            find_program(GO_CC gcc HINTS ${_mingw_bin} NO_DEFAULT_PATH)
            if(NOT GO_CC)
                set(GO_CC gcc)
            endif()
            set(_go_cc ${GO_CC})
        else()
            set(_subtest_out ${CMAKE_BINARY_DIR}/subscriber_test)
            set(_go_cc cc)
        endif()

        # cgo needs the C compiler and (on Windows) the mingw bin dir on PATH so
        # the compiler's own DLLs resolve during the build.
        add_custom_command(
            OUTPUT ${_subtest_out}
            COMMAND ${CMAKE_COMMAND} -E env
                CGO_ENABLED=1
                CC=${_go_cc}
                "PATH=${_mingw_bin}$<IF:$<BOOL:${WIN32}>,;,:>$ENV{PATH}"
                ${GO_EXECUTABLE} build -o ${_subtest_out} .
            WORKING_DIRECTORY ${_subtest_src}
            DEPENDS
                ${_subtest_src}/main.go
                ${_subtest_src}/hackrf.go
                ${_subtest_src}/p25.go
                ${_subtest_src}/go.mod
            COMMENT "Building subscriber_test (Go + libhackrf)"
            VERBATIM
        )
        add_custom_target(subscriber_test ALL DEPENDS ${_subtest_out})

        # Bundle its runtime DLLs (libhackrf + deps) on Windows.
        if(WIN32 AND LDD_EXECUTABLE)
            add_custom_command(TARGET subscriber_test POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    -DEXE=${_subtest_out}
                    -DDEST_DIR=${CMAKE_BINARY_DIR}
                    -DMINGW_BIN=${_mingw_bin}
                    -DLDD=${LDD_EXECUTABLE}
                    -P ${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake
                VERBATIM
                COMMENT "Copying runtime DLLs for subscriber_test"
            )
        endif()
    else()
        message(STATUS "SiteSim: Go not found — skipping subscriber_test "
                       "(set SITESIM_BUILD_SUBSCRIBER_TEST=OFF to silence)")
    endif()
endif()
