#include "ControlChannel.hpp"
#include "p25/TSBK.hpp"
#include "p25/Frame.hpp"
#include <chrono>
#include <cstdio>
#include <string>
#include <array>

ControlChannel::ControlChannel(SoapyTx& tx) : m_tx(tx) {}

ControlChannel::~ControlChannel() { stop(); }

void ControlChannel::configure(const SiteConfig& cfg) {
    m_cfg = cfg;
}

bool ControlChannel::start() {
    if (m_running.load()) return true;
    m_running.store(true);
    m_frameCount.store(0);
    m_producer = std::thread(&ControlChannel::producerThread, this);
    return true;
}

void ControlChannel::stop() {
    if (!m_running.exchange(false)) return;
    if (m_producer.joinable()) m_producer.join();
}

bool ControlChannel::isRunning() const { return m_running.load(); }

std::vector<ControlChannel::LogEntry> ControlChannel::getLog(size_t maxEntries) const {
    std::lock_guard<std::mutex> lock(m_logMutex);
    std::vector<LogEntry> result;
    size_t start = m_log.size() > maxEntries ? m_log.size() - maxEntries : 0;
    for (size_t i = start; i < m_log.size(); i++)
        result.push_back(m_log[i]);
    return result;
}

void ControlChannel::addLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    m_log.push_back({msg});
    if (m_log.size() > 500) m_log.pop_front();
}

void ControlChannel::producerThread() {
    auto idenUp  = p25::BuildIDENUp(0, 100, 0, 100, uint64_t(m_cfg.ccFreqHz));
    auto netSts  = p25::BuildNetStatusBcast(m_cfg.wacn, m_cfg.sysid, 0, 0, 0x00);
    auto rfssSts = p25::BuildRFSSStatusBcast(0x00, m_cfg.sysid, m_cfg.rfssid, m_cfg.siteid, 0, 0, 0x00);
    auto adjSts  = p25::BuildAdjStsBcast(m_cfg.sysid, m_cfg.rfssid, uint8_t(m_cfg.siteid + 1), 0, 0, 0x00);

    std::array<std::array<uint8_t, 12>, 4> sysTSBKs = { idenUp, netSts, rfssSts, adjSts };
    static const char* kSysNames[] = { "IDEN_UP", "NET_STS", "RFSS_STS", "ADJ_STS" };

    int      sysIdx = 0;
    uint64_t frameN = 0;

    while (m_running.load()) {
        // Throttle: back off if ring is over half full
        while (m_tx.ringAvailable() > SoapyTx::kRingSize / 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!m_running.load()) return;
        }

        // Rotate through system TSBKs every frame
        int idx  = sysIdx % 4;
        auto tsbk = sysTSBKs[idx];
        addLog(std::string("SYS ") + kSysNames[idx]);
        sysIdx++;

        auto dibits = p25::BuildFrame(m_cfg.nac, tsbk);
        auto iq     = m_c4fm.modulate(dibits);
        m_tx.write(iq.data(), iq.size());
        m_frameCount.fetch_add(1);
        frameN++;
    }
}

