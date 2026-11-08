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
#include <cmath>
#include <cstdio>

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
    // and one for voice.  A single IDEN_UP band plan is derived from the
    // lowest frequency in the list, so every channel must fit in 4095 steps.
    std::vector<Channel> channels = {
        { "CC 1", 438.8000e6 },
        { "VC 1", 438.8125e6 },
        { "VC 2", 438.8250e6 },
        { "VC 3", 438.8375e6 },
    };
    int controlIndex = 0;
    int voiceIndex   = 1;

    // IDEN_UP band plan (derived; see normalizeChannelPlan()).
    uint8_t  chanId          = 1;
    uint16_t bwUnits         = 100;   // 100 = 12.5 kHz narrowband
    uint16_t chSpacUnits     = 100;   // 100 = 12.5 kHz (125 Hz units)
    double   vchanBaseFreqHz = 438.8000e6;
    uint16_t vChanNum        = 1;     // selected voice channel number
    uint16_t ccChanNum       = 0;     // selected control channel number

    double spacingHz() const {
        return double(chSpacUnits ? chSpacUnits : 100) * 125.0;
    }

    // Raw (possibly out-of-range) channel number for a frequency.
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

    // Frequency the IDEN_UP maps a channel number to.
    double channelFrequency(uint16_t num) const {
        return vchanBaseFreqHz + double(num) * spacingHz();
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

    // Recompute the band plan and selected channel numbers. Returns false when
    // the plan cannot be represented by one IDEN_UP (span > 4095 steps).
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

        double minF = channels[0].freqHz;
        for (const auto& c : channels)
            if (c.freqHz < minF) minF = c.freqHz;

        vchanBaseFreqHz = minF;
        ccFreqHz        = channels[size_t(controlIndex)].freqHz;

        bool ok = true;
        const double spacing = spacingHz();
        for (const auto& c : channels) {
            long long n = llround((c.freqHz - minF) / spacing);
            if (n < 0 || n > 4095) ok = false;
        }

        ccChanNum = channelNumberFor(ccFreqHz);
        vChanNum  = channelNumberFor(channels[size_t(voiceIndex)].freqHz);
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

        double minF = channels[0].freqHz;
        for (const auto& c : channels)
            if (c.freqHz < minF) minF = c.freqHz;

        const double spacing = spacingHz();
        for (const auto& c : channels) {
            bool in2m = (c.freqHz >= 144.0e6 && c.freqHz <= 148.0e6);
            bool in70 = (c.freqHz >= 420.0e6 && c.freqHz <= 450.0e6);
            if (!in2m && !in70)
                return fail(std::string("Channel \"") + c.name +
                            "\" is outside the 2m/70cm amateur bands.");
            long long n = llround((c.freqHz - minF) / spacing);
            if (n < 0 || n > 4095)
                return fail("Channel plan spans more than 4095 x spacing; "
                            "use fewer/adjacent channels or wider spacing.");
        }

        double uplink = rxFreqHz();
        bool up2m = (uplink >= 144.0e6 && uplink <= 148.0e6);
        bool up70 = (uplink >= 420.0e6 && uplink <= 450.0e6);
        if (!up2m && !up70)
            return fail("Uplink / site input is outside the 2m/70cm amateur bands.");

        long long offUnits = llround(txOffsetMHz * 4.0);  // 250 kHz units
        if (offUnits < -255 || offUnits > 255)
            return fail("TX offset exceeds the IDEN_UP range (+/-63.75 MHz).");
        return true;
    }

    void resetDefaultChannelPlan() {
        channels = {
            { "CC 1", 438.8000e6 },
            { "VC 1", 438.8125e6 },
            { "VC 2", 438.8250e6 },
            { "VC 3", 438.8375e6 },
        };
        controlIndex = 0;
        voiceIndex   = 1;
        chSpacUnits  = 100;
        chanId       = 1;
        normalizeChannelPlan();
    }
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

