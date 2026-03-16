#include "ControlChannel.hpp"
#include "p25/TSBK.hpp"
#include "p25/Frame.hpp"
#include <chrono>
#include <cstdio>
#include <cmath>
#include <string>
#include <array>

ControlChannel::ControlChannel(SoapyTx& tx) : m_tx(tx) {}

ControlChannel::~ControlChannel() { stop(); }

void ControlChannel::configure(const SiteConfig& cfg) {
    m_cfg = cfg;
    // Derive IDEN_UP offset units from human-readable MHz value.
    // 1 unit = 250 kHz; round to nearest.
    double offKhz = m_cfg.txOffsetMHz * 1000.0;
    m_cfg.txOffsetUnits = int16_t(std::round(offKhz / 250.0));
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

void ControlChannel::queueTSBK(const std::array<uint8_t, 12>& tsbk) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_tsbkQueue.size() < 128)
        m_tsbkQueue.push(tsbk);
}

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

// Send one frame (TSBK or TDU) into the TX ring.
// Returns false if asked to stop.
static void sendFrame(SoapyTx& tx, p25::C4FM& c4fm, const std::vector<uint8_t>& dibits) {
    auto iq = c4fm.modulate(dibits);
    tx.write(iq.data(), iq.size());
}

void ControlChannel::producerThread() {
    // Build system TSBKs from current config.
    // IDEN_UP uses the channel plan from SiteConfig so radios can resolve
    // voice channel numbers to actual frequencies.
    auto idenUp  = p25::BuildIDENUp(
        m_cfg.chanId,
        m_cfg.bwUnits,
        m_cfg.txOffsetUnits,
        m_cfg.chSpacUnits,
        uint64_t(m_cfg.vchanBaseFreqHz)
    );
    auto netSts  = p25::BuildNetStatusBcast(m_cfg.wacn, m_cfg.sysid, 0, 0, 0x00);
    auto rfssSts = p25::BuildRFSSStatusBcast(0x00, m_cfg.sysid, m_cfg.rfssid, m_cfg.siteid,
                                              m_cfg.chanId, 0, 0x00);
    auto adjSts  = p25::BuildAdjStsBcast(m_cfg.sysid, m_cfg.rfssid,
                                          uint8_t(m_cfg.siteid + 1), 0, 0, 0x00);

    std::array<std::array<uint8_t, 12>, 4> sysTSBKs = { idenUp, netSts, rfssSts, adjSts };
    static const char* kSysNames[] = { "IDEN_UP", "NET_STS", "RFSS_STS", "ADJ_STS" };

    // Pre-build TDU dibit stream (reused every cycle)
    auto tduDibits = p25::BuildTDUFrame(m_cfg.nac);

    int      sysIdx = 0;
    uint64_t frameN = 0;

    while (m_running.load()) {
        // Back off if ring is over half full
        while (m_tx.ringAvailable() > SoapyTx::kRingSize / 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!m_running.load()) return;
        }

        // Every 4th frame slot: priority system broadcast (IDEN/NET/RFSS/ADJ)
        // Other slots: drain queued TSBKs, else fill with next sys broadcast
        std::array<uint8_t, 12> tsbk;
        std::string logMsg;

        if (frameN % 4 == 0) {
            // Mandatory sys-info slot
            int idx = sysIdx % 4;
            tsbk    = sysTSBKs[idx];
            logMsg  = std::string("SYS ") + kSysNames[idx];
            sysIdx++;
        } else {
            // Use queued response if available, else another sys broadcast
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_tsbkQueue.empty()) {
                tsbk   = m_tsbkQueue.front();
                m_tsbkQueue.pop();
                logMsg = "ACT TSBK";
            } else {
                int idx = sysIdx % 4;
                tsbk    = sysTSBKs[idx];
                logMsg  = std::string("SYS ") + kSysNames[idx];
                sysIdx++;
            }
        }

        sendFrame(m_tx, m_c4fm, p25::BuildFrame(m_cfg.nac, tsbk));
        m_frameCount.fetch_add(1);
        addLog(logMsg);
        frameN++;

        // After each full rotation of 4 frames, transmit a TDU to signal
        // end of the TSBK burst. Real P25 CCs do this between bursts.
        if (frameN % 4 == 0) {
            sendFrame(m_tx, m_c4fm, tduDibits);
            // TDU is not a "frame" in the TSBK sense — don't increment frameCount
        }
    }
}


