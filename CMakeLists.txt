cmake_minimum_required(VERSION 3.20)
project(SiteSim VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SOAPY  REQUIRED SoapySDR)
pkg_check_modules(SDL2   REQUIRED sdl2)
find_package(OpenGL REQUIRED)

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
    /usr/local/include
)

target_include_directories(uplink_test PRIVATE
    src/
    ${SOAPY_INCLUDE_DIRS}
    /usr/local/include
)

target_link_libraries(sitesim PRIVATE
    ${SOAPY_LIBRARIES}
    ${SDL2_LIBRARIES}
    OpenGL::GL
    dl
    -L/usr/local/lib
    -Wl,--allow-shlib-undefined
    dsd-neo_engine
    dsd-neo_dispatch
    dsd-neo_core
    dsd-neo_runtime
    dsd-neo_proto_p25
    dsd-neo_io_radio
    dsd-neo_io_control
    dsd-neo_io_audio
    mbe-neo
    rtlsdr
)

target_link_libraries(uplink_test PRIVATE
    ${SOAPY_LIBRARIES}
    dl
    -L/usr/local/lib
    -Wl,--allow-shlib-undefined
    dsd-neo_engine
    dsd-neo_dispatch
    dsd-neo_core
    dsd-neo_runtime
    dsd-neo_proto_p25
    dsd-neo_io_radio
    dsd-neo_io_control
    dsd-neo_io_audio
    mbe-neo
    rtlsdr
)

target_compile_options(sitesim PRIVATE
    ${SOAPY_CFLAGS_OTHER}
    ${SDL2_CFLAGS_OTHER}
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
)

target_compile_options(uplink_test PRIVATE
    ${SOAPY_CFLAGS_OTHER}
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
)
