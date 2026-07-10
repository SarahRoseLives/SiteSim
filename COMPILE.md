# Compiling SiteSim

## System dependencies (Debian/Ubuntu)

```bash
sudo apt install build-essential cmake pkg-config ninja-build \
    libsoapysdr-dev libsdl2-dev libgl-dev \
    libhackrf-dev librtlsdr-dev \
    libsndfile1-dev libssl-dev libpulse-dev libusb-1.0-0-dev \
    soapysdr-module-hackrf soapysdr-module-rtlsdr \
    golang-go
```

The full source of `dsd-neo` and `mbelib-neo` is vendored in `third_party/`
(see `third_party/VENDORING.md`) and built from source automatically as part of
the SiteSim build — no separate install step. Dear ImGui and nlohmann/json are
also vendored.

## Build

```bash
cd SiteSim

# C++ binaries (sitesim + uplink_test); builds vendored dsd-neo + mbelib-neo too.
# subscriber_test (Go) is built automatically when `go` is found.
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
cd ..
```

Output:

```
build/sitesim          # main P25 site simulator GUI
build/uplink_test      # standalone uplink receiver CLI
build/subscriber_test  # P25 ISP test transmitter (HackRF)
```

`subscriber_test` is a Go program that links libhackrf via cgo; the CMake build
drives the Go toolchain for it. Requires Go and a C compiler (cgo). Disable with
`-DSITESIM_BUILD_SUBSCRIBER_TEST=OFF`. To build it by hand instead:

```bash
cd subscriber_test && CGO_ENABLED=1 go build -o ../build/subscriber_test . && cd ..
```

## Windows (MSYS2 / mingw64)

Windows is fully supported, including the P25 RX path (dsd-neo) and HackRF /
RTL-SDR via SoapySDR. Everything is built from the vendored source.

Install the MSYS2 mingw64 toolchain and dependencies from a **MSYS2 MINGW64**
shell:

```bash
pacman -S --needed \
    mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-SDL2 \
    mingw-w64-x86_64-soapysdr \
    mingw-w64-x86_64-soapyhackrf \
    mingw-w64-x86_64-soapyrtlsdr \
    mingw-w64-x86_64-rtl-sdr \
    mingw-w64-x86_64-hackrf \
    mingw-w64-x86_64-libsndfile \
    mingw-w64-x86_64-openssl \
    mingw-w64-x86_64-portaudio \
    mingw-w64-x86_64-libusb
```

Build (from the **MSYS2 MINGW64** shell):

```bash
cd /c/path/to/SiteSim
cmake -G Ninja -S . -B build-mingw -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw -j
```

Output: `build-mingw/sitesim.exe`, `build-mingw/uplink_test.exe`, and (when Go is
installed) `build-mingw/subscriber_test.exe`.

`subscriber_test` is built with the Go toolchain via cgo against mingw's
libhackrf. Install Go for Windows (https://go.dev/dl/) — CMake finds it in
`C:\Program Files\Go\bin` automatically; the build sets `CGO_ENABLED=1` and
points cgo's compiler at mingw gcc. Its libhackrf DLLs are bundled next to the
executable like the other targets.

`sitesim.exe` is built for the Windows GUI subsystem (no console window). The
build automatically copies every required runtime DLL and the SoapySDR device
modules (HackRF / RTL-SDR) next to the executables, so the `build-mingw`
directory is self-contained and runs outside the MSYS2 shell:

```
build-mingw/
  sitesim.exe
  uplink_test.exe
  *.dll                       # full runtime DLL closure (incl. libmbe-neo.dll)
  SoapySDR/modules0.8/        # HackRF + RTL-SDR Soapy device modules
```

`SoapyTx` points `SOAPY_SDR_PLUGIN_PATH` at the bundled `SoapySDR/modules0.8`
at startup, so SDR hardware is found automatically.

To build the GUI + control-channel TX only (stub RX, no dsd-neo), configure with
`-DSITESIM_ENABLE_DSDNEO=OFF`.

## Running (Linux)

```bash
./build/sitesim
./build/uplink_test -f 451.0e6
./build/subscriber_test -f 451.0e6 -src 1234
```

The vendored `libmbe-neo.so` is built into `build/vendor-prefix/lib`; the binary
RPATH points there, so it is found automatically from the build tree.

## File structure

```
third_party/
  imgui/              # Dear ImGui (immediate-mode GUI)
  nlohmann/           # nlohmann/json (config file I/O)
  dsd-neo/            # full dsd-neo source (P25 decoder) — built from source
  mbelib-neo/         # full mbelib-neo source (IMBE vocoder) — built from source
  VENDORING.md        # upstream commits + local patch notes
```

