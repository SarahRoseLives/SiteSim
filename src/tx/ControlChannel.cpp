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
    m_cfg.normalizeChannelPlan();
    // Derive IDEN_UP offset units from human-readable MHz value.
    // 1 unit = 250 kHz; round to nearest.
    double offKhz = m_cfg.txOffsetMHz * 1000.0;
    m_cfg.txOffsetUnits = int16_t(std::round(offKhz / 250.0));
    m_c4fm.setDeviationPeakHz(m_cfg.txDeviationPeakHz);
}

bool ControlChannel::start() {
    if (m_running.load()) return true;
    // Keep the modulator at the transmitter's hardware sample rate. Full-duplex
    // Pluto uses 1.536 MS/s so TX and RX share the AD9361 baseband rate.
    if (m_tx.isOpen()) {
        m_c4fm.configure(m_tx.sampleRate());
        const double actual = m_tx.actualSampleRate();
        const int    osr    = m_c4fm.oversampling();
        char info[192];
        snprintf(info, sizeof(info),
                 "TX rate requested %.0f Hz, actual %.0f Hz; OSR %d -> %.2f sym/s (%.0f bps)",
                 m_tx.sampleRate(), actual, osr,
                 actual / double(osr), 2.0 * actual / double(osr));
        addLog(info);
    }
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
    auto netSts  = p25::BuildNetStatusBcast(m_cfg.wacn, m_cfg.sysid,
                                              m_cfg.chanId, m_cfg.ccChanNum,
                                              p25::SSC_VOICE_DATA_REGISTRATION);
    auto rfssSts = p25::BuildRFSSStatusBcast(0x00, m_cfg.sysid, m_cfg.rfssid, m_cfg.siteid,
                                              m_cfg.chanId, m_cfg.ccChanNum,
                                              p25::SSC_VOICE_DATA_REGISTRATION);
    // No ADJ_STS: a simulator with no real neighbor must not advertise itself
    // (or a bogus same-frequency site), which can make subscribers reject the
    // control channel.

    // Motorola-proprietary (MFID 0x90) station messages that a real Motorola
    // ASTRO 25 site broadcasts on its control channel. The Control Channel Base
    // Station ID identifies the site to Motorola subscribers; the Scan Marker
    // and Traffic Channel ID keep the Moto CC cadence intact.
    auto motoBsid = p25::BuildMotoBaseStationId(m_cfg.chanId, m_cfg.ccChanNum, "");
    auto motoScan = p25::BuildMotoScanMarker(0x0, 0xC0, 0x0000);
    auto motoTcId = p25::BuildMotoTrafficChannelId(0x4000C00000000800ull);

    // Secondary control channel broadcast: advertise the voice channel as the
    // backup control channel (as a real ASTRO 25 site does).
    auto secCc = p25::BuildSecCcBcast(m_cfg.rfssid, m_cfg.siteid,
                                      m_cfg.chanId, m_cfg.vChanNum, 0x04,
                                      m_cfg.chanId, m_cfg.vChanNum, 0x04);

    struct SysMsg {
        std::array<uint8_t, 12> tsbk;
        const char*             name;
        bool                    liveSync;  // rebuilt each pass (timestamp)
    };
    std::vector<SysMsg> rotation;
    rotation.push_back({ idenUp,  "IDEN_UP",  false });
    rotation.push_back({ netSts,  "NET_STS",  false });
    rotation.push_back({ rfssSts, "RFSS_STS", false });
    if (!m_cfg.minimalCc) {
        rotation.push_back({ motoBsid, "MOTO_CBSID", false });
        rotation.push_back({ motoScan, "MOTO_SCAN",  false });
        rotation.push_back({ motoTcId, "MOTO_TCID",  false });
        rotation.push_back({ secCc,    "SEC_CC",     false });
    }
    rotation.push_back({ {}, "SYNC", true });

    {
        char planMsg[192];
        snprintf(planMsg, sizeof(planMsg),
                 "PLAN CC %.5f MHz (IDEN %u ch %u)  VC %.5f MHz (IDEN %u ch %u)",
                 m_cfg.ccFreqHz / 1e6, unsigned(m_cfg.chanId), unsigned(m_cfg.ccChanNum),
                 m_cfg.voiceFreqHz() / 1e6, unsigned(m_cfg.chanId), unsigned(m_cfg.vChanNum));
        addLog(planMsg);
    }

    size_t   rotIdx = 0;
    uint64_t frameN = 0;

    while (m_running.load()) {
        // Back off if ring is over half full
        while (m_tx.ringAvailable() > SoapyTx::kRingSize / 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!m_running.load()) return;
        }

        std::array<uint8_t, 12> tsbk;
        std::string logMsg;
        bool usedQueued = false;

        // Drain a queued (active) TSBK on non-system frames; every 4th frame is
        // reserved for a system/identity broadcast so status stays fresh.
        if (frameN % 4 != 0) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_tsbkQueue.empty()) {
                tsbk = m_tsbkQueue.front();
                m_tsbkQueue.pop();
                logMsg     = "ACT TSBK";
                usedQueued = true;
            }
        }
        if (!usedQueued) {
            const SysMsg& m = rotation[rotIdx % rotation.size()];
            tsbk   = m.liveSync ? p25::BuildSyncBcast() : m.tsbk;
            logMsg = std::string("SYS ") + m.name;
            rotIdx++;
        }

        sendFrame(m_tx, m_c4fm, p25::BuildFrame(m_cfg.nac, tsbk));
        m_frameCount.fetch_add(1);
        addLog(logMsg);
        frameN++;
    }
}

