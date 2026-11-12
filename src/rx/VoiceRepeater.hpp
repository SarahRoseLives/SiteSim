#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace p25rx {

/**
 * VoiceRepeater — recovers the instantaneous frequency of a P25 voice-channel
 * uplink so it can be re-radiated on the voice downlink.
 *
 * This is deliberately a *symbol-agnostic* C4FM repeater: it mixes the selected
 * voice uplink to baseband, low-pass filters and FM-discriminates it, and
 * publishes the normalised discriminator samples. The transmitter integrates
 * those samples into a fresh C4FM carrier. No frame parsing or vocoder decoding
 * is required, so the talker's HDU/LDU/TDU framing, NAC, link control and IMBE
 * payload pass through unchanged — exactly what a real repeater does.
 *
 * The demodulated stream is at ~96 kHz (20 samples/symbol). The samples are
 * published through a lock-free single-producer/single-consumer ring that the
 * control-channel producer drains while it mixes the voice onto the CC carrier.
 */
class VoiceRepeater {
public:
    // sampleRateHz is the SDR rate; mixOffsetHz is the frequency of the voice
    // uplink relative to the RX centre in the P25Receiver convention
    // (mixOffset = centre - signal). Returns false on an unusable rate.
    bool configure(double sampleRateHz, double mixOffsetHz);
    void disable();
    void reset();
    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }
    double demodRate() const { return m_demodRate; }
    double mixOffsetHz() const { return m_mixOffsetHz; }

    // Feed interleaved complex baseband (I,Q) at the configured SDR rate.
    void process(const float* iq, size_t n);

    // Normalised discriminator sample: an outer C4FM symbol is ~ +/-3
    // (3 * f_dev / 1800 Hz). Non-blocking; returns false when empty.
    bool readFm(float& out);
    size_t available() const;

    // True while a voice transmission is present. In frame-gate mode this is
    // driven by decoded P25 voice frames (HDU/LDU/TDU), not raw energy, so
    // adjacent-channel interference and self-TX leakage cannot hold it open.
    bool squelchOpen() const { return m_squelch.load(std::memory_order_relaxed); }

    // Frame-gate mode: the RX pipeline attaches a P25Receiver to the voice
    // channel and calls noteVoiceFrame() for each valid voice frame.
    void enableFrameGate() { m_frameGate = true; }
    void noteVoiceFrame();
    bool frameGateEnabled() const { return m_frameGate; }

    struct Stats {
        uint64_t samples    = 0;
        float    power      = 0.0f;   // decimated channel power (last 5 ms block)
        float    noiseFloor = 0.0f;
    };
    const Stats& stats() const { return m_stats; }

private:
    void writeFm(float v);

    double m_sampleRate  = 0.0;
    double m_demodRate   = 96000.0;
    double m_mixOffsetHz = 0.0;
    double m_mixPhase    = 0.0;
    int    m_decim       = 1;
    int    m_decimN      = 0;

    // Decimating anti-alias FIR (windowed sinc, unity DC gain).
    std::vector<float> m_firTaps, m_firI, m_firQ;
    int m_firLen = 0;
    int m_firPos = 0;

    // FM discriminator
    bool  m_havePrev = false;
    float m_prevI    = 0.0f;
    float m_prevQ    = 0.0f;
    float m_dc       = 0.0f;
    float m_fmScale  = 1.0f;

    // Energy squelch
    double m_pwrAcc = 0.0;
    int    m_pwrN   = 0;
    float  m_nf     = 0.0f;
    int    m_hang   = 0;      // remaining 5 ms blocks to stay open
    int    m_hangMax = 30;    // 150 ms
    int    m_openRun = 0;     // consecutive blocks above the open threshold
    int    m_calBlocks = 0;   // startup noise-floor calibration blocks
    int    m_openBlocks = 0;  // continuous open time-out timer
    int    m_totBlocks  = 6000;
    int    m_coolDown   = 0;  // forced-closed blocks after the time-out timer
    bool   m_frameGate  = false;
    std::atomic<uint64_t> m_lastFrameMs{0};
    std::atomic<bool> m_squelch{false};

    std::atomic<bool> m_enabled{false};
    Stats m_stats;

    // SPSC sample ring (~1.4 s at 96 kHz).
    static constexpr size_t kRingSize = 1u << 17;
    std::vector<float> m_ring;
    std::atomic<size_t> m_wpos{0};
    std::atomic<size_t> m_rpos{0};
};

} // namespace p25rx
