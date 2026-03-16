#include "ControlChannel.hpp"
#include "p25/TSBK.hpp"
#include "p25/Frame.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <array>

static const uint32_t kSimUnits[]     = {0x001001, 0x001002, 0x001003, 0x001004, 0x001005, 0x001006};
static const uint16_t kSimTalkgroups[] = {1, 2, 3, 100, 200};
static constexpr int kNumUnits     = 6;
static constexpr int kNumTalkgroups = 5;

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
    if (m_cfg.simEnabled)
        m_sim = std::thread(&ControlChannel::simulationThread, this);
    return true;
}

void ControlChannel::stop() {
    if (!m_running.exchange(false)) return;
    if (m_producer.joinable()) m_producer.join();
    if (m_sim.joinable()) m_sim.join();
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
    // Build system TSBKs from current config
    auto idenUp = p25::BuildIDENUp(0, 100, 0, 100, uint64_t(m_cfg.ccFreqHz));
    auto netSts  = p25::BuildNetStatusBcast(m_cfg.wacn, m_cfg.sysid, 0, 0, 0x00);
    auto rfssSts = p25::BuildRFSSStatusBcast(0x00, m_cfg.sysid, m_cfg.rfssid, m_cfg.siteid, 0, 0, 0x00);
    auto adjSts  = p25::BuildAdjStsBcast(m_cfg.sysid, m_cfg.rfssid, uint8_t(m_cfg.siteid + 1), 0, 0, 0x00);

    std::array<std::array<uint8_t, 12>, 4> sysTSBKs = { idenUp, netSts, rfssSts, adjSts };

    static const char* kSysNames[] = { "IDEN_UP", "NET_STS", "RFSS_STS", "ADJ_STS" };

    int sysIdx = 0;
    uint64_t frameN = 0;

    while (m_running.load()) {
        // Throttle: sleep if ring > half full
        while (m_tx.ringAvailable() > SoapyTx::kRingSize / 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!m_running.load()) return;
        }

        std::array<uint8_t, 12> tsbk;
        std::string logMsg;

        if (frameN % 4 == 0) {
            int idx = sysIdx % 4;
            tsbk = sysTSBKs[idx];
            logMsg = std::string("SYS ") + kSysNames[idx];
            sysIdx++;
        } else {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_activityQueue.empty()) {
                tsbk   = m_activityQueue.front();
                m_activityQueue.pop();
                logMsg = "ACT frame";
            } else {
                int idx = sysIdx % 4;
                tsbk = sysTSBKs[idx];
                logMsg = std::string("SYS ") + kSysNames[idx];
                sysIdx++;
            }
        }

        auto dibits = p25::BuildFrame(m_cfg.nac, tsbk);
        auto iq     = m_c4fm.modulate(dibits);
        m_tx.write(iq.data(), iq.size());
        m_frameCount.fetch_add(1);
        addLog(logMsg);
        frameN++;
    }
}

void ControlChannel::simulationThread() {
    auto send = [this](const std::array<uint8_t, 12>& t) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_activityQueue.size() < 128)
            m_activityQueue.push(t);
    };

    // Phase 1: register and affiliate all units
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "[sim] registering %d units across %d talkgroups",
                 kNumUnits, kNumTalkgroups);
        addLog(buf);
    }

    for (int i = 0; i < kNumUnits && m_running.load(); i++) {
        uint32_t unit = kSimUnits[i];
        uint16_t tg   = kSimTalkgroups[i % kNumTalkgroups];
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "[sim] register unit=0x%06X tg=%d", unit, tg);
            addLog(buf);
        }
        send(p25::BuildURegRsp(true, m_cfg.sysid, unit, unit));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (!m_running.load()) return;
        send(p25::BuildLocRegRsp(0, tg, m_cfg.rfssid, m_cfg.siteid, unit));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (!m_running.load()) return;
        send(p25::BuildGrpAffRsp(false, 1, tg, tg, unit));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (!m_running.load()) return;
    }
    addLog("[sim] all units online; starting call simulation");

    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> unitDist(0, kNumUnits - 1);
    std::uniform_int_distribution<int> tgDist(0, kNumTalkgroups - 1);
    std::uniform_int_distribution<int> callDur(2, 8);
    std::uniform_int_distribution<int> gapDur(1, 4);
    std::uniform_real_distribution<float> chance(0.0f, 1.0f);

    while (m_running.load()) {
        uint32_t src = kSimUnits[unitDist(rng)];
        uint16_t tg  = kSimTalkgroups[tgDist(rng)];

        {
            char buf[128];
            snprintf(buf, sizeof(buf), "[sim] voice grant tg=%d src=0x%06X ch=%d", tg, src, int(m_cfg.vChanNum));
            addLog(buf);
        }
        send(p25::BuildGrpVChGrant(0x00, 0, m_cfg.vChanNum, tg, src));

        // Send grant updates every ~500ms for call duration
        auto callEnd = std::chrono::steady_clock::now() + std::chrono::seconds(callDur(rng));
        while (std::chrono::steady_clock::now() < callEnd && m_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            send(p25::BuildGrpVChGrantUpdt(0, m_cfg.vChanNum, tg, 0, m_cfg.vChanNum, tg));
        }
        if (!m_running.load()) return;

        // Gap between calls: 1-4 seconds
        std::this_thread::sleep_for(std::chrono::seconds(gapDur(rng)));
        if (!m_running.load()) return;

        // Occasionally re-affiliate a random unit
        if (chance(rng) < 0.4f) {
            uint32_t unit = kSimUnits[unitDist(rng)];
            uint16_t tg2  = kSimTalkgroups[tgDist(rng)];
            {
                char buf[128];
                snprintf(buf, sizeof(buf), "[sim] affiliation tg=%d unit=0x%06X", tg2, unit);
                addLog(buf);
            }
            send(p25::BuildGrpAffRsp(false, 1, tg2, tg2, unit));
        }

        // Occasionally re-register a unit
        if (chance(rng) < 0.2f) {
            uint32_t unit = kSimUnits[unitDist(rng)];
            uint16_t tg2  = kSimTalkgroups[tgDist(rng)];
            {
                char buf[128];
                snprintf(buf, sizeof(buf), "[sim] registration unit=0x%06X", unit);
                addLog(buf);
            }
            send(p25::BuildURegRsp(true, m_cfg.sysid, unit, unit));
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            if (!m_running.load()) return;
            send(p25::BuildLocRegRsp(0, tg2, m_cfg.rfssid, m_cfg.siteid, unit));
        }
    }
}
