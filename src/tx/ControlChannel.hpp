#pragma once
#include "SoapyTx.hpp"
#include "p25/C4FM.hpp"
#include <string>
#include <vector>
#include <deque>
#include <atomic>
#include <thread>
#include <mutex>
#include <array>
#include <cstdint>

struct SiteConfig {
    uint16_t nac       = 0x293;
    uint32_t wacn      = 0xBEEF0;
    uint16_t sysid     = 0x001;
    uint8_t  rfssid    = 0x01;
    uint8_t  siteid    = 0x01;
    double   ccFreqHz  = 145.050e6;
    double   gainDb    = 20.0;
    bool     ampEnabled = false;
    uint16_t vChanNum  = 1;
};

class ControlChannel {
public:
    explicit ControlChannel(SoapyTx& tx);
    ~ControlChannel();

    void configure(const SiteConfig& cfg);
    bool start();
    void stop();
    bool isRunning() const;

    uint64_t frameCount() const { return m_frameCount.load(); }

    struct LogEntry { std::string msg; };
    std::vector<LogEntry> getLog(size_t maxEntries = 100) const;

private:
    void producerThread();
    void addLog(const std::string& msg);

    SoapyTx&    m_tx;
    SiteConfig  m_cfg;
    p25::C4FM   m_c4fm;

    std::atomic<bool>   m_running{false};
    std::atomic<uint64_t> m_frameCount{0};
    std::thread         m_producer;

    mutable std::mutex  m_logMutex;
    std::deque<LogEntry> m_log;
};
