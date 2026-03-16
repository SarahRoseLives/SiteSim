#include "SoapyTx.hpp"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <cstring>
#include <thread>
#include <chrono>
#include <cstdio>

SoapyTx::SoapyTx() {
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
    try {
        m_device = SoapySDR::Device::make(args);
        if (!m_device) {
            m_lastError = "SoapySDR::Device::make returned null";
            return false;
        }
        m_device->setSampleRate(SOAPY_SDR_TX, 0, m_sampleRate);
        m_device->setFrequency(SOAPY_SDR_TX, 0, m_frequency);
        m_device->setGain(SOAPY_SDR_TX, 0, m_gain);
        // Try amp if supported
        try {
            m_device->setGain(SOAPY_SDR_TX, 0, "AMP", m_ampEnabled ? 14.0 : 0.0);
        } catch (...) {}

        m_stream = m_device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
        if (!m_stream) {
            m_lastError = "setupStream failed";
            SoapySDR::Device::unmake(m_device);
            m_device = nullptr;
            return false;
        }
        m_lastError.clear();
        return true;
    } catch (const std::exception& e) {
        m_lastError = e.what();
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

void SoapyTx::setFrequency(double hz) {
    m_frequency = hz;
    if (m_device) {
        try { m_device->setFrequency(SOAPY_SDR_TX, 0, hz); } catch (...) {}
    }
}

void SoapyTx::setGain(double db) {
    m_gain = db;
    if (m_device) {
        try { m_device->setGain(SOAPY_SDR_TX, 0, db); } catch (...) {}
    }
}

void SoapyTx::setAmpEnabled(bool en) {
    m_ampEnabled = en;
    if (m_device) {
        try { m_device->setGain(SOAPY_SDR_TX, 0, "AMP", en ? 14.0 : 0.0); } catch (...) {}
    }
}

void SoapyTx::setSampleRate(double sps) {
    m_sampleRate = sps;
    if (m_device) {
        try { m_device->setSampleRate(SOAPY_SDR_TX, 0, sps); } catch (...) {}
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

std::string SoapyTx::lastError() const { return m_lastError; }

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

    m_device->activateStream(m_stream);

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

        void* ptrs[1] = { buf.data() };
        int flags = 0;
        long long timeNs = 0;
        int ret = m_device->writeStream(m_stream, ptrs, kChunkSamples, flags, timeNs, 1000000);
        if (ret < 0) {
            // Ignore errors, keep going
        }
    }

    m_device->deactivateStream(m_stream);
}
