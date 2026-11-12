#include "ControlChannel.hpp"
#include "p25/TSBK.hpp"
#include "p25/Frame.hpp"
#include "rx/VoiceRepeater.hpp"
#include <chrono>
#include <cstdio>
#include <cmath>
#include <string>
#include <array>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    m_voiceOffsetTx = m_cfg.voiceFreqHz() - m_cfg.ccFreqHz;
}

void ControlChannel::prepareTx() {
    if (!m_tx.isOpen()) return;
    const bool voicePlan = m_cfg.voiceRepeat && m_voice &&
                           std::abs(m_voiceOffsetTx) >= 100e3;
    if (voicePlan) {
        const double bw = 2.0 * std::abs(m_voiceOffsetTx) + 300e3;
        m_tx.setTxBandwidth(std::max(600e3, bw));
    } else {
        m_tx.setTxBandwidth(200e3);
    }
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
    // Composite CC + voice TX bandwidth is applied by prepareTx(), which the
    // caller must run before the TX stream is activated.
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

    // ── Composite voice repeat ─────────────────────────────────────────────
    // The CC frames are streamed in small blocks instead of one frame at a
    // time so a second C4FM carrier (the repeated voice, mixed to
    // m_voiceOffsetTx) can be added sample-for-sample. The RX pipeline owns the
    // VoiceRepeater and publishes normalised discriminator samples at ~96 kHz;
    // here they are linearly up-sampled to the TX rate and phase-accumulated.
    const bool voice = voiceRepeatEnabled();
    const double txRate = m_tx.sampleRate();
    const double voiceStep = voice ? (m_voice->demodRate() / txRate) : 0.0;
    const double voicePhasePerUnit = 2.0 * M_PI * 600.0 / txRate;  // +/-1 symbol = 600 Hz
    const double voiceOffsetInc    = 2.0 * M_PI * m_voiceOffsetTx / txRate;
    // Reserve headroom for the sum of two carriers. The C4FM modulator emits
    // +/-100, so 0.72 x CC + 54 x voice peaks at ~126 of the 127 int8 range.
    const float ccScale  = voice ? 0.72f : 1.0f;
    const float voiceAmp = 54.0f;

    struct VoiceTxState {
        double pos   = 0.0;   // fractional position between prev/next FM sample
        float  prev  = 0.0f;
        float  next  = 0.0f;
        double phase = 0.0;
        bool   primed = false;
    } vtx;
    const size_t voicePrime = voice ? size_t(m_voice->demodRate() * 0.05) : 0;

    if (voice) {
        char b[160];
        snprintf(b, sizeof(b), "VOICE repeat ON: VC %.5f MHz, TX mix %+.0f kHz (squelch adaptive)",
                 m_cfg.voiceFreqHz() / 1e6, m_voiceOffsetTx / 1e3);
        addLog(b);
    }

    std::vector<int8_t> ccIq;
    size_t ccPos = 0;

    auto generateNext = [&]() {
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

        ccIq = m_c4fm.modulate(p25::BuildFrame(m_cfg.nac, tsbk));
        ccPos = 0;
        frameN++;
        m_frameCount.fetch_add(1);
        addLog(logMsg);
    };

    const size_t kBlock = 2048;      // IQ pairs per mixed block (~1.3 ms @1.536 MS/s)
    std::vector<int8_t> out(kBlock * 2);

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

        // Prime the voice path once ~50 ms of discriminator samples exist, so
        // RX/TX clock jitter does not cause constant underruns. If the RX was
        // already running for a while, discard the stale backlog and keep only
        // the freshest ~50 ms so the repeat does not gain seconds of latency.
        if (voice && !vtx.primed) {
            while (m_voice->available() > voicePrime) {
                float stale = 0.0f;
                m_voice->readFm(stale);
            }
            if (m_voice->available() >= voicePrime)
                vtx.primed = true;
        }

        for (size_t i = 0; i < kBlock; ++i) {
            if (ccPos >= ccIq.size() / 2) generateNext();
            const int ccI = ccIq[2 * ccPos];
            const int ccQ = ccIq[2 * ccPos + 1];
            ++ccPos;

            float vI = 0.0f, vQ = 0.0f;
            if (voice && vtx.primed) {
                // Linear interpolation from the ~96 kHz discriminator stream.
                const float s = vtx.prev + (vtx.next - vtx.prev) * float(vtx.pos);
                vtx.pos += voiceStep;
                while (vtx.pos >= 1.0) {
                    vtx.pos -= 1.0;
                    vtx.prev = vtx.next;
                    float x = 0.0f;
                    vtx.next = m_voice->readFm(x) ? x : vtx.prev;
                }
                // s is in transmitter symbol units (+/-1 inner, +/-3 outer),
                // so this preserves the talker's deviation. The constant term
                // places the carrier at the voice downlink frequency.
                vtx.phase += s * voicePhasePerUnit + voiceOffsetInc;
                if (vtx.phase > 2.0 * M_PI)       vtx.phase -= 2.0 * M_PI;
                else if (vtx.phase < -2.0 * M_PI) vtx.phase += 2.0 * M_PI;

                if (m_voice->squelchOpen()) {
                    vI = float(std::cos(vtx.phase)) * voiceAmp;
                    vQ = float(std::sin(vtx.phase)) * voiceAmp;
                }
            }

            int I = int(std::lround(double(ccI) * ccScale + vI));
            int Q = int(std::lround(double(ccQ) * ccScale + vQ));
            if (I < -127) I = -127; else if (I > 127) I = 127;
            if (Q < -127) Q = -127; else if (Q > 127) Q = 127;
            out[2 * i]     = int8_t(I);
            out[2 * i + 1] = int8_t(Q);
        }
        m_tx.write(out.data(), out.size());
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
    // A radio cannot be on a voice call and sending control-channel traffic.
    if (m_lastTalker == unit) m_lastTalker = 0;
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
    if (m_lastTalker == unit) m_lastTalker = 0;
}

void ControlChannel::noteDeregistration(uint32_t unit)
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    // The radio has left the system: drop it from the roster so it no longer
    // shows as registered/affiliated and cannot be granted a call.
    m_radios.erase(unit);
    if (m_lastTalker == unit) m_lastTalker = 0;
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
    m_lastTalker   = unit;
    m_callStarted  = r.keyedSince;
}

void ControlChannel::noteUnitToUnit(uint32_t source, uint32_t target)
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    auto& r = upsertRadioLocked(source);
    r.keyed      = true;
    r.unitToUnit = true;
    r.keyedSince = std::chrono::steady_clock::now();
    r.lastSeen   = r.keyedSince;
    m_lastTalker = source;
    m_callStarted = r.keyedSince;
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
    const bool va  = voiceActive();
    // Safety net: a stuck squelch (or a radio that vanished without a
    // de-registration) must not keep the UI on "active call" forever. The
    // grant-derived keyed state expires normally once the voice path closes.
    const bool callRecent = m_callStarted.time_since_epoch().count() != 0 &&
                            now - m_callStarted < std::chrono::minutes(2);
    std::vector<RadioActivity> out;
    out.reserve(m_radios.size());
    for (const auto& kv : m_radios) {
        RadioActivity r = kv.second;
        // The grant-derived "keyed" flag normally expires after a short
        // hangtime. While the voice repeater hears audio, keep the current
        // talker shown as keyed for the duration of the transmission.
        const bool talking = va && callRecent && r.unitId == m_lastTalker;
        if (r.keyed && now - r.keyedSince > kKeyedHangtime && !talking) {
            r.keyed      = false;
            r.emergency  = false;
            r.unitToUnit = false;
        }
        if (talking) r.keyed = true;
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
    m_lastTalker = 0;
    m_callStarted = {};
}

bool ControlChannel::voiceActive() const
{
    return m_voice && m_voice->squelchOpen();
}

bool ControlChannel::voiceRepeatEnabled() const
{
    return m_cfg.voiceRepeat && m_voice && m_voice->enabled() &&
           std::abs(m_voiceOffsetTx) >= 100e3;
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
