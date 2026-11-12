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
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <cstdint>
#include <cmath>
#include <cstdio>

namespace p25rx { class VoiceRepeater; }

struct SiteConfig {
    // One row in the user-entered channel plan.
    struct Channel {
        char   name[32] = {};
        double freqHz   = 0.0;
    };

    // System identity
    uint16_t nac       = 0x293;
    uint32_t wacn      = 0xBEEF0;
    uint16_t sysid     = 0x123;
    uint8_t  rfssid    = 0x01;
    uint8_t  siteid    = 0x01;

    // RF parameters
    double   ccFreqHz      = 438.800e6;
    double   txOffsetMHz   = -5.0;  // TX offset in MHz (+ or -); 0 = simplex
    // Peak (outer-symbol) C4FM deviation in Hz. Standard P25 = 1800 (inner
    // ±600, outer ±1800). Non-standard channels may expect a wider swing.
    double   txDeviationPeakHz = 1800.0;
    // Hz correction added to the requested TX frequency to compensate the
    // SDR's local-oscillator error (e.g. a Pluto a few ppm high).
    double   txFreqTrimHz = 0.0;
    double   gainDb        = 20.0;  // HackRF-style 0..47 dB gain
    // Pluto's SoapySDR TX range is 0..89; 89 = minimum attenuation (max output).
    // Kept separate so switching radios does not apply an unusable gain.
    double   plutoGainDb   = 70.0;
    bool     ampEnabled    = false;

    // Diagnostic "minimal CC": transmit only the standard core broadcasts
    // (IDEN_UP, NET_STS, RFSS_STS, SYNC) and omit the Motorola-proprietary
    // MFID-90 messages and SEC_CC. This is a provably standard control channel
    // used to isolate whether a fabricated broadcast is preventing lock.
    bool     minimalCc     = false;

    // Composite voice repeat: transmit the granted voice channel alongside the
    // CC and receive both uplinks. Opt-in because it widens the TX/RX RF
    // bandwidth; with it off the site uses the narrow CC-only tuning.
    bool     voiceRepeat   = false;

    // Run the uplink (site-input) receiver on a dedicated RTL-SDR instead of
    // sharing the Pluto with TX. A Pluto transmitting on the control channel
    // desenses its own receiver only ~5 MHz away, so the uplink is often
    // deaf; a separate RTL-SDR hears the radios cleanly.
    bool     rxUseRtlsdr   = false;

    // Derived: rxFreqHz = ccFreqHz + txOffsetMHz * 1e6
    double rxFreqHz() const { return ccFreqHz + txOffsetMHz * 1e6; }

    // IDEN_UP txOffsetUnits: 250 kHz/unit, signed magnitude
    // Derived from txOffsetMHz on configure()
    int16_t  txOffsetUnits = 0;

    // ── Channel plan ──────────────────────────────────────────────────────
    // The user edits this list and picks one channel for the control channel
    // and one for voice.  The control channel and the voice channels use
    // separate IDEN_UP band plans: the CC IDEN base sits on the control
    // frequency (CC = channel 0) and the voice IDEN base sits just below the
    // lowest voice channel (first VC = channel 1). This lets the VCs be spread
    // far from the CC for uplink-desense reasons while their channel numbers
    // stay small, which some subscribers require before they accept a grant.
    //
    // The defaults deliberately put the VCs below the CC: the site receiver is
    // tuned ~180 kHz above the CC uplink to dodge its DC spike, so voice
    // uplinks placed below the CC fall outside that RX passband.
    std::vector<Channel> channels = {
        { "CC 1", 438.8000e6 },
        { "VC 1", 438.3000e6 },
        { "VC 2", 438.3125e6 },
        { "VC 3", 438.3250e6 },
    };
    int controlIndex = 0;
    int voiceIndex   = 1;

    // IDEN_UP band plans (derived; see normalizeChannelPlan()).
    // chanId is the control-channel IDEN; voiceChanId is a second IDEN that
    // carries the voice channels, so they can be spread far from the CC with
    // small channel numbers.
    uint8_t  chanId          = 1;
    uint8_t  voiceChanId     = 2;
    uint16_t bwUnits         = 100;   // 100 = 12.5 kHz narrowband
    uint16_t chSpacUnits     = 100;   // 100 = 12.5 kHz (125 Hz units)
    double   ccBaseFreqHz    = 438.8000e6;  // CC IDEN base (CC = channel 0)
    double   vchanBaseFreqHz = 438.2875e6;  // voice IDEN base (first VC = channel 1)
    uint16_t vChanNum        = 1;     // selected voice channel number (voice IDEN)
    uint16_t ccChanNum       = 0;     // selected control channel number (CC IDEN)

    double spacingHz() const {
        return double(chSpacUnits ? chSpacUnits : 100) * 125.0;
    }

    // Raw (possibly out-of-range) channel number in the voice IDEN.
    long long channelNumberRaw(double freqHz) const {
        return llround((freqHz - vchanBaseFreqHz) / spacingHz());
    }

    // Channel number clamped to the 12-bit grant field.
    uint16_t channelNumberFor(double freqHz) const {
        long long n = channelNumberRaw(freqHz);
        if (n < 0) return 0;
        if (n > 4095) return 4095;
        return uint16_t(n);
    }

    // Frequency the voice IDEN_UP maps a channel number to.
    double channelFrequency(uint16_t num) const {
        return vchanBaseFreqHz + double(num) * spacingHz();
    }
    double voiceChannelFrequency(uint16_t num) const { return channelFrequency(num); }

    // Frequency the control IDEN_UP maps a channel number to.
    double ccChannelFrequency(uint16_t num) const {
        return ccBaseFreqHz + double(num) * spacingHz();
    }

    double controlFreqHz() const {
        if (controlIndex >= 0 && controlIndex < int(channels.size()))
            return channels[size_t(controlIndex)].freqHz;
        return ccFreqHz;
    }

    double voiceFreqHz() const {
        if (voiceIndex >= 0 && voiceIndex < int(channels.size()))
            return channels[size_t(voiceIndex)].freqHz;
        return channelFrequency(vChanNum);
    }

    // Recompute the two band plans and selected channel numbers. Returns false
    // when the voice channels cannot be represented by one voice IDEN_UP
    // (span > 4095 steps).
    bool normalizeChannelPlan() {
        if (channels.empty()) {
            Channel c{};
            std::snprintf(c.name, sizeof(c.name), "CC");
            c.freqHz = ccFreqHz;
            channels.push_back(c);
        }
        if (chSpacUnits == 0) chSpacUnits = 100;

        if (controlIndex < 0 || controlIndex >= int(channels.size()))
            controlIndex = 0;
        if (voiceIndex < 0 || voiceIndex >= int(channels.size()))
            voiceIndex = channels.size() > 1 ? 1 : 0;

        chanId      &= 0x0F;
        voiceChanId &= 0x0F;
        if (voiceChanId == chanId)
            voiceChanId = uint8_t((chanId + 1) & 0x0F);

        const double spacing = spacingHz();

        // Control IDEN: base on the control frequency, so the CC is channel 0.
        ccFreqHz     = channels[size_t(controlIndex)].freqHz;
        ccBaseFreqHz = ccFreqHz;
        ccChanNum    = 0;

        // Voice IDEN: cover every channel except the selected control channel,
        // with the base one spacing below the lowest so the first VC is channel
        // 1 (the old, known-good grant numbering).
        double minV = 0.0;
        bool haveV = false;
        for (size_t i = 0; i < channels.size(); ++i) {
            if (int(i) == controlIndex) continue;
            if (!haveV || channels[i].freqHz < minV) {
                minV = channels[i].freqHz;
                haveV = true;
            }
        }
        if (!haveV) minV = ccFreqHz;
        vchanBaseFreqHz = minV - spacing;
        if (vchanBaseFreqHz < 0.0) vchanBaseFreqHz = 0.0;

        bool ok = true;
        for (size_t i = 0; i < channels.size(); ++i) {
            if (int(i) == controlIndex) continue;
            long long n = llround((channels[i].freqHz - vchanBaseFreqHz) / spacing);
            if (n < 0 || n > 4095) ok = false;
        }
        vChanNum = channelNumberFor(channels[size_t(voiceIndex)].freqHz);
        return ok;
    }

    // Full validation for the UI: non-empty, indices, amateur band, IDEN span.
    // Returns true when valid; otherwise fills `error` when supplied.
    bool channelPlanValid(std::string* error = nullptr) const {
        auto fail = [&](const std::string& msg) {
            if (error) *error = msg;
            return false;
        };

        if (channels.empty())
            return fail("Channel plan is empty.");
        if (controlIndex < 0 || controlIndex >= int(channels.size()))
            return fail("No control channel selected.");
        if (voiceIndex < 0 || voiceIndex >= int(channels.size()))
            return fail("No voice channel selected.");
        if (spacingHz() <= 0.0)
            return fail("Invalid channel spacing.");

        auto inBand = [](double f) {
            return (f >= 144.0e6 && f <= 148.0e6) ||
                   (f >= 420.0e6 && f <= 450.0e6);
        };

        const double spacing = spacingHz();
        double minV = 0.0, maxV = 0.0;
        bool haveV = false;
        for (size_t i = 0; i < channels.size(); ++i) {
            const auto& c = channels[i];
            if (!inBand(c.freqHz))
                return fail(std::string("Channel \"") + c.name +
                            "\" is outside the 2m/70cm amateur bands.");
            if (int(i) == controlIndex) continue;
            if (!haveV || c.freqHz < minV) minV = c.freqHz;
            if (!haveV || c.freqHz > maxV) maxV = c.freqHz;
            haveV = true;
        }
        if (haveV && llround((maxV - minV) / spacing) > 4095)
            return fail("Voice channels span more than 4095 x spacing; "
                        "use fewer/adjacent voice channels or wider spacing.");

        if (!inBand(rxFreqHz()))
            return fail("Uplink / site input is outside the 2m/70cm amateur bands.");
        for (size_t i = 0; i < channels.size(); ++i) {
            if (int(i) == controlIndex) continue;
            if (!inBand(channels[i].freqHz + txOffsetMHz * 1e6))
                return fail(std::string("Voice uplink for \"") + channels[i].name +
                            "\" is outside the 2m/70cm amateur bands.");
        }

        long long offUnits = llround(txOffsetMHz * 4.0);  // 250 kHz units
        if (offUnits < -255 || offUnits > 255)
            return fail("TX offset exceeds the IDEN_UP range (+/-63.75 MHz).");
        return true;
    }

    void resetDefaultChannelPlan() {
        channels = {
            { "CC 1", 438.8000e6 },
            { "VC 1", 438.3000e6 },
            { "VC 2", 438.3125e6 },
            { "VC 3", 438.3250e6 },
        };
        controlIndex = 0;
        voiceIndex   = 1;
        chSpacUnits  = 100;
        chanId       = 1;
        voiceChanId  = 2;
        normalizeChannelPlan();
    }
};

// One subscriber as seen by the site, for the live System Activity view.
struct RadioActivity {
    uint32_t unitId       = 0;
    uint16_t talkgroup    = 0;
    uint16_t activeChan   = 0;
    uint8_t  activeIden   = 0;
    double   activeFreqHz = 0.0;
    bool     registered   = false;
    bool     affiliated   = false;
    bool     keyed        = false;
    bool     emergency    = false;
    bool     unitToUnit   = false;
    std::chrono::steady_clock::time_point lastSeen{};
    std::chrono::steady_clock::time_point keyedSince{};
};

class ControlChannel {
public:
    explicit ControlChannel(SoapyTx& tx);
    ~ControlChannel();

    void configure(const SiteConfig& cfg);
    // Apply TX-side setup that must happen before the TX stream is activated
    // (currently the RF bandwidth needed for the composite CC+voice carrier).
    void prepareTx();
    bool start();
    void stop();
    bool isRunning() const;

    // Queue a TSBK for transmission (thread-safe). Used by RX pipeline to
    // inject grant/registration/affiliation responses.
    void queueTSBK(const std::array<uint8_t, 12>& tsbk);

    uint64_t frameCount() const { return m_frameCount.load(); }

    struct LogEntry { std::string msg; };
    std::vector<LogEntry> getLog(size_t maxEntries = 100) const;

    // ── Subscriber activity (System Activity panel) ──────────────────────
    // Thread-safe: updated from the RX thread, read by the UI thread.
    void noteRegistration(uint32_t unit);
    void noteAffiliation(uint32_t unit, uint16_t tgid);
    void noteDeregistration(uint32_t unit);
    void noteVoiceRequest(uint32_t unit, uint16_t tgid, uint8_t iden,
                          uint16_t chan, double freqHz);
    void noteUnitToUnit(uint32_t source, uint32_t target);
    void noteEmergency(uint32_t unit, uint16_t tgid);
    // Snapshot with the keyed state expired after a short hangtime.
    std::vector<RadioActivity> radioSnapshot() const;

    // Access control for call handling. A fresh control-channel start forgets
    // all subscribers (a real site restart loses registrations/affiliations).
    void clearActivity();
    bool unitRegistered(uint32_t unit) const;
    bool unitAffiliated(uint32_t unit, uint16_t tgid) const;

    // Composite voice repeat. The repeater itself is owned by the RX pipeline;
    // hand it over before start(). True while a voice transmission is present.
    void setVoiceRepeater(p25rx::VoiceRepeater* v) { m_voice = v; }
    bool voiceActive() const;
    bool voiceRepeatEnabled() const;

private:
    void producerThread();
    void addLog(const std::string& msg);
    RadioActivity& upsertRadioLocked(uint32_t unit);
    void pruneRadiosLocked();

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

    mutable std::mutex   m_activityMutex;
    std::unordered_map<uint32_t, RadioActivity> m_radios;

    p25rx::VoiceRepeater* m_voice = nullptr;
    double   m_voiceOffsetTx = 0.0;   // voice downlink relative to the CC carrier
    uint32_t m_lastTalker    = 0;     // guarded by m_activityMutex
    std::chrono::steady_clock::time_point m_callStarted{};  // guarded likewise
};

