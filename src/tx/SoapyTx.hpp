#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <cstdint>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Formats.hpp>

class SoapyTx {
public:
    SoapyTx();
    ~SoapyTx();

    bool open(const std::string& args = "");
    void close();
    bool isOpen() const;

    void setFrequency(double hz);
    void setGain(double db);
    void setAmpEnabled(bool en);
    void setSampleRate(double sps);

    // Write int8 IQ bytes into ring buffer
    void write(const int8_t* data, size_t n);

    bool startTx();
    void stopTx();
    bool isRunning() const;

    std::string lastError() const;
    std::vector<std::string> listDevices();

    static constexpr size_t kRingSize = 4 * 1024 * 1024;

    size_t ringAvailable() const;

private:
    void txThread();
    void ringWrite(const int8_t* src, size_t n);
    void ringRead(int8_t* dst, size_t n);

    SoapySDR::Device* m_device   = nullptr;
    SoapySDR::Stream* m_stream   = nullptr;
    double m_frequency  = 145.050e6;
    double m_gain       = 20.0;
    double m_sampleRate = 2.4e6;
    bool   m_ampEnabled = false;

    std::atomic<bool> m_running{false};
    std::thread       m_txThread;
    std::string       m_lastError;

    std::vector<int8_t> m_ring;
    std::atomic<size_t> m_wpos{0};
    std::atomic<size_t> m_rpos{0};
};
