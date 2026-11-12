#include "SoapyTx.hpp"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#include <string>

namespace {
// On Windows, SoapySDR's module search path is baked to the build prefix. When
// SiteSim runs outside the MSYS2 shell it must be told where the bundled device
// modules (SoapyHackRF / SoapyRTLSDR) live.  Point SOAPY_SDR_PLUGIN_PATH at a
// "SoapySDR/modules0.8" directory next to the executable, unless the user has
// already set it.  Must run before any SoapySDR call.
bool dirExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void ensureSoapyPluginPath() {
    wchar_t exePath[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n == MAX_PATH)
        return;

    std::wstring dir(exePath, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return;
    dir.resize(slash);

    // Build a search list: bundled modules first, then PothosSDR's plugins, then
    // whatever the environment already pointed at. SoapyPlutoSDR is usually
    // installed by PothosSDR rather than MSYS2.
    std::wstring paths;
    auto add = [&paths](const std::wstring& p) {
        if (!dirExists(p))
            return;
        if (!paths.empty())
            paths += L";";
        paths += p;
    };

    add(dir + L"\\SoapySDR\\modules0.8");

    wchar_t programFiles[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH) > 0) {
        std::wstring pf(programFiles);
        add(pf + L"\\PothosSDR\\lib\\SoapySDR\\modules0.8");
        add(pf + L"\\PothosSDR\\lib\\SoapySDR\\modules0.7");
        std::wstring bin = pf + L"\\PothosSDR\\bin";
        if (dirExists(bin)) {
            wchar_t oldPath[32767] = {};
            GetEnvironmentVariableW(L"PATH", oldPath, 32767);
            std::wstring newPath = bin + L";" + oldPath;
            SetEnvironmentVariableW(L"PATH", newPath.c_str());
            _wputenv_s(L"PATH", newPath.c_str());
        }
    }

    wchar_t existing[32767] = {};
    if (GetEnvironmentVariableW(L"SOAPY_SDR_PLUGIN_PATH", existing, 32767) > 0 && existing[0]) {
        if (!paths.empty())
            paths += L";";
        paths += existing;
    }
    if (paths.empty())
        return;

    SetEnvironmentVariableW(L"SOAPY_SDR_PLUGIN_PATH", paths.c_str());
    // Also update the CRT environment so getenv-based lookups agree.
    _wputenv_s(L"SOAPY_SDR_PLUGIN_PATH", paths.c_str());
}
} // namespace
#endif

SoapyTx::SoapyTx() {
#if defined(_WIN32)
    ensureSoapyPluginPath();
#endif
    m_ring.resize(kRingSize, 0);
    // Suppress SoapySDR log noise
    SoapySDR::setLogLevel(SOAPY_SDR_WARNING);
}

SoapyTx::~SoapyTx() {
    stopTx();
    close();
}

std::vector<std::string> SoapyTx::listDevices() {
    std::vector<std::string> result;
    auto devs = SoapySDR::Device::enumerate("");
    for (auto& kw : devs) {
        std::string s;
        for (auto& kv : kw) {
            if (!s.empty()) s += ", ";
            s += kv.first + "=" + kv.second;
        }
        result.push_back(s);
    }
    if (result.empty()) result.push_back("(no device found)");
    return result;
}

bool SoapyTx::open(const std::string& args) {
    if (m_device) close();
    m_args = args;
    m_driverKey.clear();
    m_hardwareKey.clear();
    try {
        m_device = SoapySDR::Device::make(args);
        if (!m_device) {
            setError("SoapySDR::Device::make returned null");
            return false;
        }
        try { m_driverKey = m_device->getDriverKey(); } catch (...) {}
        try { m_hardwareKey = m_device->getHardwareKey(); } catch (...) {}
        m_device->setSampleRate(SOAPY_SDR_TX, 0, m_sampleRate);
        try { m_actualSampleRate = m_device->getSampleRate(SOAPY_SDR_TX, 0); }
        catch (...) { m_actualSampleRate = m_sampleRate; }
        // Keep the Pluto's TX emission inside its own channel. The AD9361 leaves
        // the RF bandwidth at the firmware default otherwise, and on a single
        // radio the broadband TX leakage desenses the co-located RX. A composite
        // CC+voice stream widens this to cover both downlinks.
        if (isPluto()) {
            const double bw = m_txBandwidth > 0.0 ? m_txBandwidth : 200e3;
            try { m_device->setBandwidth(SOAPY_SDR_TX, 0, bw); } catch (...) {}
            m_txBandwidthDirty = false;
        }
        applyFrequencyChecked(m_frequency);
        m_device->setGain(SOAPY_SDR_TX, 0, m_gain);
        // HackRF's "AMP" gain stage; other drivers may map the named setter
        // back onto the normal gain, so only apply it on HackRF.
        if (isHackRf()) {
            try {
                m_device->setGain(SOAPY_SDR_TX, 0, "AMP", m_ampEnabled ? 14.0 : 0.0);
            } catch (...) {}
        }

        m_stream = m_device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
        if (!m_stream) {
            setError("setupStream failed");
            SoapySDR::Device::unmake(m_device);
            m_device = nullptr;
            return false;
        }
        setError("");
        return true;
    } catch (const std::exception& e) {
        setError(e.what());
        m_device = nullptr;
        return false;
    }
}

void SoapyTx::close() {
    stopTx();
    if (m_device) {
        if (m_stream) {
            m_device->closeStream(m_stream);
            m_stream = nullptr;
        }
        SoapySDR::Device::unmake(m_device);
        m_device = nullptr;
    }
}

bool SoapyTx::isOpen() const { return m_device != nullptr; }

namespace {
std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
} // namespace

bool SoapyTx::isPluto() const {
    const std::string text = lowerCopy(m_driverKey + " " + m_hardwareKey + " " + m_args);
    return text.find("pluto") != std::string::npos ||
           text.find("adalm") != std::string::npos ||
           text.find("ad936") != std::string::npos;
}

bool SoapyTx::isHackRf() const {
    const std::string text = lowerCopy(m_driverKey + " " + m_hardwareKey + " " + m_args);
    return text.find("hackrf") != std::string::npos;
}

bool SoapyTx::isFullDuplex() const {
    if (m_device) {
        try {
            if (m_device->getFullDuplex(SOAPY_SDR_TX, 0) &&
                m_device->getFullDuplex(SOAPY_SDR_RX, 0))
                return true;
        } catch (...) {}
    }
    return isPluto();
}

std::string SoapyTx::displayName() const {
    if (isPluto()) return "ADALM-Pluto";
    if (!m_driverKey.empty()) return m_driverKey;
    return "SoapySDR";
}

bool SoapyTx::applyFrequencyChecked(double hz) {
    if (!m_device) return false;

    double actual = 0.0;
    try {
        m_device->setFrequency(SOAPY_SDR_TX, 0, hz);
        actual = m_device->getFrequency(SOAPY_SDR_TX, 0);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_errMutex);
        m_freqError = std::string("setFrequency failed: ") + e.what();
        return false;
    }

    std::string err;
    if (actual > 0.0 && std::abs(actual - hz) > 100e3) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "TX frequency rejected: asked for %.4f MHz, hardware is at %.4f MHz%s",
                 hz / 1e6, actual / 1e6,
                 isPluto() ? " (Pluto tunes ~325-3800 MHz)" : "");
        err = buf;
    }
    std::lock_guard<std::mutex> lock(m_errMutex);
    m_freqError = err;
    return err.empty();
}

bool SoapyTx::frequencyValid() const {
    std::lock_guard<std::mutex> lock(m_errMutex);
    return m_freqError.empty();
}

void SoapyTx::setFrequency(double hz) {
    m_frequency = hz + m_freqTrimHz;
    if (m_device) applyFrequencyChecked(m_frequency);
}

void SoapyTx::setGain(double db) {
    m_gain = db;
    if (m_device) {
        try { m_device->setGain(SOAPY_SDR_TX, 0, db); } catch (...) {}
    }
}

void SoapyTx::setAmpEnabled(bool en) {
    m_ampEnabled = en;
    if (m_device && isHackRf()) {
        try { m_device->setGain(SOAPY_SDR_TX, 0, "AMP", en ? 14.0 : 0.0); } catch (...) {}
    }
}

void SoapyTx::setSampleRate(double sps) {
    m_sampleRate = sps;
    m_actualSampleRate = 0.0;
    if (m_device) {
        try {
            m_device->setSampleRate(SOAPY_SDR_TX, 0, sps);
            m_actualSampleRate = m_device->getSampleRate(SOAPY_SDR_TX, 0);
        } catch (...) {}
    }
}

void SoapyTx::setTxBandwidth(double hz) {
    m_txBandwidth = hz;
    // Re-configuring the AD9361 RF bandwidth while a stream is live can crash
    // or glitch the Pluto driver. Apply it now only when idle; otherwise mark
    // it for the TX thread to apply before activateStream on the next start.
    if (m_device && !m_running.load()) {
        try {
            m_device->setBandwidth(SOAPY_SDR_TX, 0, hz);
            m_txBandwidthDirty = false;
        } catch (...) {}
    } else {
        m_txBandwidthDirty = true;
    }
}

size_t SoapyTx::ringAvailable() const {
    size_t w = m_wpos.load(std::memory_order_acquire);
    size_t r = m_rpos.load(std::memory_order_acquire);
    return (w - r + kRingSize) % kRingSize;
}

void SoapyTx::ringWrite(const int8_t* src, size_t n) {
    size_t w = m_wpos.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; i++) {
        m_ring[w] = src[i];
        w = (w + 1) % kRingSize;
    }
    m_wpos.store(w, std::memory_order_release);
}

void SoapyTx::ringRead(int8_t* dst, size_t n) {
    size_t r = m_rpos.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; i++) {
        dst[i] = m_ring[r];
        r = (r + 1) % kRingSize;
    }
    m_rpos.store(r, std::memory_order_release);
}

void SoapyTx::write(const int8_t* data, size_t n) {
    // Spin-wait if ring would overflow (should not happen with throttle)
    while (ringAvailable() + n >= kRingSize)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ringWrite(data, n);
}

bool SoapyTx::startTx() {
    if (m_running.load()) return true;
    m_running.store(true);
    m_txThread = std::thread(&SoapyTx::txThread, this);
    return true;
}

void SoapyTx::stopTx() {
    if (!m_running.exchange(false)) return;
    if (m_txThread.joinable()) m_txThread.join();
}

bool SoapyTx::isRunning() const { return m_running.load(); }

void SoapyTx::setError(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_errMutex);
    m_lastError = msg;
}

std::string SoapyTx::lastError() const {
    std::lock_guard<std::mutex> lock(m_errMutex);
    return m_freqError.empty() ? m_lastError : m_freqError;
}

void SoapyTx::txThread() {
    if (!m_stream || !m_device) {
        // No hardware: just drain the ring buffer slowly
        while (m_running.load()) {
            // Consume ring bytes to prevent overflow
            size_t avail = ringAvailable();
            if (avail > 0) {
                static thread_local std::vector<int8_t> trash(16384);
                size_t consume = std::min(avail, trash.size());
                ringRead(trash.data(), consume);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return;
    }

    try {
        // Apply any bandwidth requested while the stream was stopped (the
        // AD9361 must not be reconfigured underneath a live stream).
        if (m_txBandwidthDirty && m_txBandwidth > 0.0) {
            try { m_device->setBandwidth(SOAPY_SDR_TX, 0, m_txBandwidth); } catch (...) {}
            m_txBandwidthDirty = false;
        }
        int rc = m_device->activateStream(m_stream);
        if (rc != 0) {
            setError(std::string("TX activateStream failed: ") + SoapySDR::errToStr(rc));
            m_running.store(false);
            return;
        }
    } catch (const std::exception& e) {
        setError(std::string("TX activateStream failed: ") + e.what());
        m_running.store(false);
        return;
    }

    static constexpr int kChunkSamples = 8192;  // IQ pairs
    static constexpr int kChunkBytes   = kChunkSamples * 2;  // int8 bytes

    std::vector<int8_t> buf(kChunkBytes, 0);

    while (m_running.load()) {
        size_t avail = ringAvailable();
        if (avail >= size_t(kChunkBytes)) {
            ringRead(buf.data(), kChunkBytes);
        } else {
            // Underrun: deliver what we have + zero pad
            if (avail > 0)
                ringRead(buf.data(), avail);
            std::memset(buf.data() + avail, 0, kChunkBytes - avail);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // writeStream is allowed to accept fewer samples than requested. In
        // particular SoapyPlutoSDR 0.2.2 uses a fixed 4096-sample TX buffer, so
        // an 8192-sample chunk comes back as 4096 then 0 (after its internal
        // flush). Consume the chunk in a loop; otherwise the remainder is
        // dropped and the transmitted C4FM stream is corrupted.
        size_t offset = 0;
        while (m_running.load() && offset < size_t(kChunkSamples)) {
            void* ptrs[1] = { buf.data() + offset * 2 };
            int flags = 0;
            long long timeNs = 0;
            int ret = m_device->writeStream(
                m_stream, ptrs, kChunkSamples - offset, flags, timeNs, 1000000);
            if (ret > 0) {
                offset += size_t(ret);
                if (m_writeErrorReported) {
                    setError("");
                    m_writeErrorReported = false;
                }
            } else if (ret == 0 || ret == SOAPY_SDR_TIMEOUT) {
                // 0 means the driver flushed its internal buffer (retry now);
                // a timeout is normal backpressure. Do not sleep 1 ms here or
                // real-time TX can fall behind and underrun.
                std::this_thread::yield();
            } else {
                // Timeouts are normal under backpressure; anything else is worth
                // surfacing in the UI once. Drop the rest of this chunk.
                if (!m_writeErrorReported) {
                    setError(std::string("TX writeStream failed: ") + SoapySDR::errToStr(ret));
                    m_writeErrorReported = true;
                }
                break;
            }
        }
    }

    try { m_device->deactivateStream(m_stream); } catch (...) {}
}
