#pragma once
#include "tx/ControlChannel.hpp"
#include <deque>
#include <string>
#include <mutex>

class LogPanel {
public:
    explicit LogPanel(ControlChannel& cc);
    void render();

    // Called by the RX pipeline to add decoded/received entries
    void addRxEntry(const std::string& msg);

private:
    ControlChannel& m_cc;
    size_t          m_lastTxCount = 0;
    size_t          m_lastRxCount = 0;

    mutable std::mutex        m_rxMutex;
    std::deque<std::string>   m_rxLog;
    static constexpr size_t   kMaxRxEntries = 500;
};
