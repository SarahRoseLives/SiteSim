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
RTL-SDR / ADALM-Pluto via SoapySDR. The dsd-neo/mbelib sources are vendored and
built automatically; the SDR device modules come from the packages above (plus
SoapyPlutoSDR for Pluto, see below).

Install the MSYS2 mingw64 toolchain and dependencies from a **MSYS2 MINGW64**
shell:

```bash
pacman -S --needed \
    mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-SDL2 \
    mingw-w64-x86_64-rtl-sdr \
    mingw-w64-x86_64-hackrf \
    mingw-w64-x86_64-libsndfile \
    mingw-w64-x86_64-openssl \
    mingw-w64-x86_64-portaudio \
    mingw-w64-x86_64-libusb
```

MSYS2 **no longer ships SoapySDR in the mingw64 repo**, but the compiled
packages are still in the package archive. Install the newest archived builds
(adjust versions if the archive has newer ones):

```bash
pkgs=$(mktemp -d) && cd "$pkgs"
base=https://repo.msys2.org/mingw/mingw64
for f in \
  mingw-w64-x86_64-soapysdr-0.8.1-4-any.pkg.tar.zst \
  mingw-w64-x86_64-soapyhackrf-0.3.4-3-any.pkg.tar.zst \
  mingw-w64-x86_64-soapyrtlsdr-0.3.3-2-any.pkg.tar.zst ; do
  curl -fLO "$base/$f"
done
pacman -U --noconfirm *.pkg.tar.zst
cd - >/dev/null && rm -rf "$pkgs"
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
libhackrf. Either install the MSYS2 package (`pacman -S
mingw-w64-x86_64-go`) or Go for Windows (https://go.dev/dl/). CMake finds Go in
the mingw prefix or `C:\Program Files\Go\bin`, sets `CGO_ENABLED=1`, points
cgo's compiler at mingw gcc, and (for the trimmed MSYS2 toolchain) sets
`GOROOT`/`GOCACHE` automatically. Its libhackrf DLLs are bundled next to the
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

### ADALM-Pluto (single-radio, full duplex)

An ADALM-Pluto can do both the downlink control-channel TX and the uplink RX at
the same time, so SiteSim needs only that one radio. This requires the
**SoapyPlutoSDR** module plus libiio/libad9361, which MSYS2 no longer ships in
the live mingw64 repo. Build the module with **mingw64 gcc** (an MSVC-built
PothosSDR module is not ABI-compatible with the mingw SoapySDR):

```bash
# libiio / libad9361 (newest archived mingw64 builds)
pkgs=$(mktemp -d) && cd "$pkgs"
base=https://repo.msys2.org/mingw/mingw64
for f in \
  mingw-w64-x86_64-libiio-0.26-4-any.pkg.tar.zst \
  mingw-w64-x86_64-libad9361-iio-0.3-1-any.pkg.tar.zst ; do
  curl -fLO "$base/$f"
done
pacman -U --noconfirm *.pkg.tar.zst
cd - >/dev/null && rm -rf "$pkgs"

# SoapyPlutoSDR from source, installed into the mingw64 prefix
src=$(mktemp -d) && cd "$src"
git clone --depth 1 https://github.com/pothosware/SoapyPlutoSDR.git
cd SoapyPlutoSDR
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/mingw64
cmake --build build
cmake --install build
cd - >/dev/null && rm -rf "$src"
```

`SoapySDRUtil --info` should then list `plutosdr` among the factories.

At build time SiteSim bundles Soapy modules from the mingw64 prefix,
`SOAPY_SDR_PLUGIN_PATH`, and `C:/Program Files/PothosSDR/lib/SoapySDR/modules0.8`
(plus their DLL dependencies). Override the search with
`-DSOAPY_MODULE_DIRS="dir1;dir2"` and the build prints a hint if no Pluto module
is found.

What SiteSim does when a Pluto is selected:

- prefers it when auto-opening a device (the Device menu still lets you pick
  another SoapySDR device);
- opens **one** SoapySDR device and shares that handle between the TX stream and
  dsd-neo's RX stream (no second context, safe full duplex);
- runs both directions at **1.536 MS/s** (320 samples/symbol) so the AD9361
  baseband rate is identical for RX and TX (dsd-neo's 24 kHz DSP path is a /64
  decimation of that rate);
- tunes TX to the control-channel frequency and RX to the Site Input Channel
  (control channel + TX offset).

**Frequency range:** the AD9363 in the ADALM-Pluto tunes roughly
**325 MHz – 3.8 GHz** on the stock firmware. The 2m band (144–148 MHz) is out of
range: requesting 145.1 MHz is silently ignored by the hardware and the LO stays
at its previous/default frequency. Use a **70cm** frequency (420–450 MHz) or an
upconverter for 2m. SiteSim now verifies the tuned frequency and refuses to key
TX if the hardware rejects it.

Because the Pluto front end is shared, give TX and RX different frequencies via
the **TX Offset** setting (for example +5 MHz), and provide antenna isolation
(duplexer/circulator) if both use the same port. TX and RX cannot share the same
frequency on one radio.

HackRF + RTL-SDR remains the default two-radio setup: with a non-Pluto Soapy
transmitter, RX stays on the native librtlsdr path. `uplink_test --pluto` also
works for RX-only testing.

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

