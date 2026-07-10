#pragma once
#include "SoapyTx.hpp"
#include "p25/C4FM.hpp"
#include <string>
#include <vector>
#include <queue>
#include <deque>
#include <atomic>
#include <thread>
#include <mutex>
#include <array>
#include <cstdint>

struct SiteConfig {
    // System identity
    uint16_t nac       = 0x293;
    uint32_t wacn      = 0xBEEF0;
    uint16_t sysid     = 0x001;
    uint8_t  rfssid    = 0x01;
    uint8_t  siteid    = 0x01;

    // RF parameters
    double   ccFreqHz      = 145.050e6;
    double   txOffsetMHz   = 0.0;   // TX offset in MHz (+ or -); 0 = simplex
    double   gainDb        = 20.0;
    bool     ampEnabled    = false;

    // Derived: rxFreqHz = ccFreqHz + txOffsetMHz * 1e6
    double rxFreqHz() const { return ccFreqHz + txOffsetMHz * 1e6; }

    // IDEN_UP txOffsetUnits: 250 kHz/unit, signed magnitude
    // Derived from txOffsetMHz on configure()
    int16_t  txOffsetUnits = 0;

    // Channel plan (for IDEN_UP)
    uint8_t  chanId          = 0;
    uint16_t bwUnits         = 100;   // 100 = 12.5 kHz narrowband
    uint16_t chSpacUnits     = 100;   // 100 = 12.5 kHz
    double   vchanBaseFreqHz = 145.050e6;
    uint16_t vChanNum        = 1;
};

class ControlChannel {
public:
    explicit ControlChannel(SoapyTx& tx);
    ~ControlChannel();

    void configure(const SiteConfig& cfg);
    bool start();
    void stop();
    bool isRunning() const;

    // Queue a TSBK for transmission (thread-safe). Used by RX pipeline to
    // inject grant/registration/affiliation responses.
    void queueTSBK(const std::array<uint8_t, 12>& tsbk);

    uint64_t frameCount() const { return m_frameCount.load(); }

    struct LogEntry { std::string msg; };
    std::vector<LogEntry> getLog(size_t maxEntries = 100) const;

private:
    void producerThread();
    void addLog(const std::string& msg);

    SoapyTx&    m_tx;
    SiteConfig  m_cfg;
    p25::C4FM   m_c4fm;

    std::atomic<bool>     m_running{false};
    std::atomic<uint64_t> m_frameCount{0};
    std::thread           m_producer;

    std::queue<std::array<uint8_t, 12>> m_tsbkQueue;
    mutable std::mutex    m_queueMutex;

    mutable std::mutex   m_logMutex;
    std::deque<LogEntry> m_log;
};

