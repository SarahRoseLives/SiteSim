#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
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
// Frequency trim (Hz) applied on top of the requested TX frequency to
// compensate the SDR's LO error (e.g. a Pluto that is a few ppm off). Set
// before setFrequency/open.
void setFrequencyTrimHz(double hz) { m_freqTrimHz = hz; }
    void setGain(double db);
    void setAmpEnabled(bool en);
    void setSampleRate(double sps);
    // TX RF bandwidth (Hz). A composite CC+voice stream needs a bandwidth that
    // spans both carriers; ignored by radios that do not expose it.
    void setTxBandwidth(double hz);
    double txBandwidth() const { return m_txBandwidth; }

    // Write int8 IQ bytes into ring buffer
    void write(const int8_t* data, size_t n);

    bool startTx();
    void stopTx();
    bool isRunning() const;

    std::string lastError() const;
    std::vector<std::string> listDevices();

    double sampleRate() const { return m_sampleRate; }
    // Rate the hardware actually reports after we asked for m_sampleRate.
    // If this differs, the C4FM symbol rate is off and a real radio's timing
    // recovery may not lock even though an SDR decoder does.
    double actualSampleRate() const { return m_actualSampleRate > 0.0 ? m_actualSampleRate : m_sampleRate; }
    double frequency() const { return m_frequency; }
    double gain() const { return m_gain; }
    // False when the hardware did not actually accept the requested frequency
    // (e.g. a Pluto asked for 145 MHz, below its ~325 MHz minimum).
    bool frequencyValid() const;
    std::string argsString() const { return m_args; }
    std::string driverKey() const { return m_driverKey; }
    std::string hardwareKey() const { return m_hardwareKey; }

    // Underlying SoapySDR handle (borrowed). Used to share one device between
    // the TX stream and the dsd-neo RX stream on full-duplex radios.
    SoapySDR::Device* device() const { return m_device; }

    // True for radios that can transmit and receive simultaneously from a
    // single device handle (ADALM-Pluto in particular).
    bool isFullDuplex() const;
    bool isPluto() const;
    bool isHackRf() const;
    std::string displayName() const;

    static constexpr size_t kRingSize = 4 * 1024 * 1024;

    size_t ringAvailable() const;

private:
    void txThread();
    void ringWrite(const int8_t* src, size_t n);
    void ringRead(int8_t* dst, size_t n);
    void setError(const std::string& msg);
    bool applyFrequencyChecked(double hz);

    SoapySDR::Device* m_device   = nullptr;
    SoapySDR::Stream* m_stream   = nullptr;
    double m_frequency  = 145.050e6;
double m_freqTrimHz = 0.0;
    double m_gain       = 20.0;
    double m_sampleRate = 2.4e6;
    double m_actualSampleRate = 0.0;
    double m_txBandwidth = 0.0;
    bool   m_txBandwidthDirty = false;
    bool   m_ampEnabled = false;

    std::atomic<bool> m_running{false};
    std::thread       m_txThread;
    mutable std::mutex m_errMutex;
    std::string       m_lastError;
    std::string       m_freqError;
    std::string       m_args;
    std::string       m_driverKey;
    std::string       m_hardwareKey;
    bool              m_writeErrorReported = false;

    std::vector<int8_t> m_ring;
    std::atomic<size_t> m_wpos{0};
    std::atomic<size_t> m_rpos{0};
};
