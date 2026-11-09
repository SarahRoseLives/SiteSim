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
    // A fresh start is a site restart: forget all registrations/affiliations so
    // subscribers must re-register/affiliate before they can be granted a call.
    clearActivity();
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
    // Two IDEN_UP broadcasts: one for the control channel (base = CC frequency,
    // CC = channel 0) and one for the voice channels (base just below the
    // lowest VC). That keeps voice channel numbers small even when the VCs are
    // spread far from the CC.
    auto ccIdenUp = p25::BuildIDENUp(
        m_cfg.chanId,
        m_cfg.bwUnits,
        m_cfg.txOffsetUnits,
        m_cfg.chSpacUnits,
        uint64_t(m_cfg.ccBaseFreqHz)
    );
    auto voiceIdenUp = p25::BuildIDENUp(
        m_cfg.voiceChanId,
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
    // backup control channel (as a real ASTRO 25 site does). The voice channel
    // is on the voice IDEN.
    auto secCc = p25::BuildSecCcBcast(m_cfg.rfssid, m_cfg.siteid,
                                      m_cfg.voiceChanId, m_cfg.vChanNum, 0x04,
                                      m_cfg.voiceChanId, m_cfg.vChanNum, 0x04);

    struct SysMsg {
        std::array<uint8_t, 12> tsbk;
        const char*             name;
        bool                    liveSync;  // rebuilt each pass (timestamp)
    };
    std::vector<SysMsg> rotation;
    rotation.push_back({ ccIdenUp,    "IDEN_UP_CC", false });
    rotation.push_back({ voiceIdenUp, "IDEN_UP_VC", false });
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
        char planMsg[224];
        snprintf(planMsg, sizeof(planMsg),
                 "PLAN CC %.5f MHz (IDEN %u base %.5f ch %u)  VC %.5f MHz (IDEN %u base %.5f ch %u)",
                 m_cfg.ccFreqHz / 1e6, unsigned(m_cfg.chanId), m_cfg.ccBaseFreqHz / 1e6,
                 unsigned(m_cfg.ccChanNum),
                 m_cfg.voiceFreqHz() / 1e6, unsigned(m_cfg.voiceChanId),
                 m_cfg.vchanBaseFreqHz / 1e6, unsigned(m_cfg.vChanNum));
        addLog(planMsg);
    }

    size_t   rotIdx = 0;
    uint64_t frameN = 0;

    while (m_running.load()) {
        // Back off once the ring holds more than ~85 ms of audio. Keeping this
        // low matters for queued responses (voice channel grants): the producer
        // generates frames ahead of the transmitter, and a large backlog delays
        // a grant by however much audio is buffered. At the Pluto's 1.536 MS/s a
        // half-full 4 MB ring was ~0.65 s (vs ~0.43 s on a 2.4 MS/s HackRF),
        // which is enough for a subscriber to miss its talk-permit window and
        // repeat the request. 256 kB keeps it well under 100 ms.
        while (m_tx.ringAvailable() > SoapyTx::kRingSize / 16) {
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


// ── Subscriber activity ──────────────────────────────────────────────────────
// A "keyed" flag is only inferred from the uplink request/grant (SiteSim does
// not carry voice), so it expires after a short hangtime instead of waiting for
// an unkey we cannot observe. Any later control-channel traffic from that unit
// clears it immediately, since a radio cannot be on a voice channel and sending
// control TSBKs at the same time.
static constexpr auto kKeyedHangtime = std::chrono::seconds(3);
static constexpr size_t kMaxTrackedRadios = 256;

RadioActivity& ControlChannel::upsertRadioLocked(uint32_t unit)
{
    auto it = m_radios.find(unit);
    if (it != m_radios.end())
        return it->second;
    if (m_radios.size() >= kMaxTrackedRadios)
        pruneRadiosLocked();
    RadioActivity r;
    r.unitId = unit;
    auto res = m_radios.emplace(unit, r);
    return res.first->second;
}

void ControlChannel::pruneRadiosLocked()
{
    if (m_radios.empty()) return;
    auto oldest = m_radios.begin();
    for (auto it = m_radios.begin(); it != m_radios.end(); ++it)
        if (it->second.lastSeen < oldest->second.lastSeen)
            oldest = it;
    m_radios.erase(oldest);
}

void ControlChannel::noteRegistration(uint32_t unit)
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    auto& r = upsertRadioLocked(unit);
    r.registered = true;
    r.keyed      = false;   // back on the control channel, not on a voice call
    r.emergency  = false;
    r.lastSeen   = std::chrono::steady_clock::now();
}

void ControlChannel::noteAffiliation(uint32_t unit, uint16_t tgid)
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    auto& r = upsertRadioLocked(unit);
    r.registered = true;
    r.affiliated = true;
    r.keyed      = false;   // back on the control channel, not on a voice call
    r.emergency  = false;
    r.talkgroup  = tgid;
    r.lastSeen   = std::chrono::steady_clock::now();
}

void ControlChannel::noteDeregistration(uint32_t unit)
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    // The radio has left the system: drop it from the roster so it no longer
    // shows as registered/affiliated and cannot be granted a call.
    m_radios.erase(unit);
}

void ControlChannel::noteVoiceRequest(uint32_t unit, uint16_t tgid, uint8_t iden,
                                      uint16_t chan, double freqHz)
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    auto& r = upsertRadioLocked(unit);
    r.talkgroup    = tgid;
    r.activeIden   = iden;
    r.activeChan   = chan;
    r.activeFreqHz = freqHz;
    r.keyed        = true;
    r.unitToUnit   = false;
    r.keyedSince   = std::chrono::steady_clock::now();
    r.lastSeen     = r.keyedSince;
}

void ControlChannel::noteUnitToUnit(uint32_t source, uint32_t target)
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    auto& r = upsertRadioLocked(source);
    r.keyed      = true;
    r.unitToUnit = true;
    r.keyedSince = std::chrono::steady_clock::now();
    r.lastSeen   = r.keyedSince;
    // Keep the target visible too, so an active unit-to-unit call lists both.
    auto& t = upsertRadioLocked(target);
    t.lastSeen = r.keyedSince;
}

void ControlChannel::noteEmergency(uint32_t unit, uint16_t tgid)
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    auto& r = upsertRadioLocked(unit);
    r.talkgroup  = tgid;
    r.emergency  = true;
    r.keyed      = true;
    r.keyedSince = std::chrono::steady_clock::now();
    r.lastSeen   = r.keyedSince;
}

std::vector<RadioActivity> ControlChannel::radioSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    const auto now = std::chrono::steady_clock::now();
    std::vector<RadioActivity> out;
    out.reserve(m_radios.size());
    for (const auto& kv : m_radios) {
        RadioActivity r = kv.second;
        if (r.keyed && now - r.keyedSince > kKeyedHangtime) {
            r.keyed      = false;
            r.emergency  = false;
            r.unitToUnit = false;
        }
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(),
              [](const RadioActivity& a, const RadioActivity& b) {
                  if (a.keyed != b.keyed) return a.keyed > b.keyed;
                  if (a.emergency != b.emergency) return a.emergency > b.emergency;
                  return a.lastSeen > b.lastSeen;
              });
    return out;
}

void ControlChannel::clearActivity()
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    m_radios.clear();
}

bool ControlChannel::unitRegistered(uint32_t unit) const
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    auto it = m_radios.find(unit);
    if (it == m_radios.end()) return false;
    return it->second.registered || it->second.affiliated;
}

bool ControlChannel::unitAffiliated(uint32_t unit, uint16_t tgid) const
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    auto it = m_radios.find(unit);
    if (it == m_radios.end()) return false;
    const auto& r = it->second;
    if (!r.registered || !r.affiliated) return false;
    if (tgid != 0 && r.talkgroup != tgid) return false;
    return true;
}
