#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace p25rx {

/**
 * Native P25 Phase 1 (C4FM) uplink receiver.
 *
 * Consumes complex baseband I/Q (interleaved float I,Q) at a configured sample
 * rate and recovers TSBK PDUs from the 4-level C4FM waveform:
 *
 *   FM discriminator -> symbol timing recovery -> dibit stream
 *     -> frame sync (48-bit FSW) -> NID (BCH)
 *     -> strip OSP status symbols -> de-interleave -> Viterbi (trellis)
 *     -> CRC-16 -> 12-byte TSBK
 *
 * This is the inverse of p25::BuildFrame()/p25::C4FM::modulate(). It is used
 * for the site-input (uplink) path so SiteSim can decode radios in-process and
 * respond on the control channel.
 */
class P25Receiver {
public:
    using TsbkCallback = std::function<void(const std::array<uint8_t, 12>& tsbk)>;
    using LogCallback  = std::function<void(const std::string&)>;

    struct Stats {
        uint64_t samples       = 0;
        uint64_t symbols       = 0;
        uint64_t fswHits       = 0;
        uint64_t nidOk         = 0;
        uint64_t framesTried   = 0;
        uint64_t crcOk         = 0;
        uint64_t tsbks         = 0;
        int      minFswDist    = 99;      // best FSW Hamming distance seen
        int      minFswDistInv = 99;      // best FSW distance with inverted dibits
        uint64_t dibitCount[4] = {0,0,0,0};
        // Coarse symbol-value histogram, bins over [-6,+6] (24 bins).
        uint64_t symHist[24] = {0};
        float    symMin = 1e9f, symMax = -1e9f;
    };

    P25Receiver();

    // Configure the input sample rate (Hz) and expected NAC. NAC 0 disables the
    // NAC check (still validates NID DUID). Returns false on unsupported rate.
    bool configure(double sampleRateHz, uint16_t nac = 0);

    void setTsbkCallback(TsbkCallback cb) { m_onTsbk = std::move(cb); }
    void setLogCallback(LogCallback cb)   { m_onLog  = std::move(cb); }
    void setInvert(bool inv) { m_invert = inv; }   // flip C4FM polarity
    void setSsInterval(int n) { if (n >= 0) m_ssInterval = n; }
    // Downconvert an off-centre capture to baseband before decimation (brings
    // the signal to DC and pushes the tuner DC spike out of the passband).
    void setMixOffset(double hz) { m_mixOffsetHz = hz; m_mixPhase = 0.0; }
    void setGardnerGain(double g) { if (g > 0.0) m_gardnerGain = g; }
    void setFswMaxDist(int d) { if (d >= 0) m_fswMaxDist = d; }

    // Process complex baseband samples (interleaved I,Q; length = n*2).
    void process(const float* iq, size_t n);

    const Stats& stats() const { return m_stats; }
    void reset();

private:
    // FM discriminator accumulation
    bool   m_havePrev = false;
    float  m_prevI = 0.0f, m_prevQ = 0.0f;

    // IQ decimation to the demod rate (~48 kHz) before the FM discriminator,
    // matching dsd-neo (base_decim=32). Essential: at 1.5 MS/s the per-sample
    // phase step is tiny and noise dominates the discriminator.
    int    m_decim   = 1;
    int    m_decimN  = 0;
    double m_decimI  = 0.0, m_decimQ = 0.0;
    double m_demodRate = 48000.0;

    // Windowed-sinc decimating FIR (anti-alias before decimation).
    std::vector<float> m_firTaps;
    std::vector<float> m_firI, m_firQ;   // circular history
    int m_firLen = 0;
    int m_firPos = 0;
    double m_mixOffsetHz = 0.0;
    double m_mixPhase    = 0.0;
    double m_gardnerGain = 0.00015;
    int    m_fswMaxDist  = 8;
    double m_nf         = 0.0;    // adaptive noise floor for burst gating
    double m_gateFactor = 4.0;    // process only when power > nf*gate

    // Timing recovery (Gardner) state
    double m_sps        = 10.0;   // samples per symbol (fs / 4800)
    double m_spsEst     = 10.0;   // tracked symbol period
    double m_acc        = 0.0;    // timing accumulator (samples)
    float  m_symPrev    = 0.0f;   // previous symbol sample (for Gardner)
    float  m_midPrev    = 0.0f;   // previous mid-symbol sample
    bool   m_haveSym    = false;

    // FM discriminator history (absolute index = m_fmBase + vector index)
    std::vector<float> m_fmHist;
    size_t  m_fmBase  = 0;
    double  m_nextPos = 0.0;      // absolute position of next symbol sample
    float   m_agc     = 0.0f;     // adaptive level normalizer
    float   interpAt(double pos) const;
    float   integrateDump(double pos) const;

    // Dibit / frame state machine
    uint64_t m_fswWin   = 0;      // rolling 48-bit FSW window

    enum class State { Hunt, WaitLock };
    State    m_state    = State::Hunt;
    double   m_fswStartPos = 0.0; // absolute FM pos of frame dibit 1 (FSW sym 1)
    std::array<uint8_t, 130> m_frameBuf{};  // frame dibits 25..154 (NID+TSBK)
    std::array<uint8_t, 98>  m_dataDibits{};

    uint16_t m_nac     = 0;
    double   m_sampleRate = 48000.0;
    bool     m_invert  = false;
    float   m_fmScale = 1.0f;    // fm -> normalized units (outer symbol ~ +/-3)
    float   m_dc      = 0.0f;    // slow DC/carrier-offset tracker
    double  m_symSqAvg = 16.0;   // running mean of symbol power (adaptive slicer)
    int     m_ssInterval = 35;   // frame dibits between OSP status symbols
    float   m_fswTmpl[24] = {0}; // FSW symbol template (base polarity)
    double  m_fswTmplNorm = 1.0; // template energy norm

    Stats         m_stats;
    TsbkCallback  m_onTsbk;
    LogCallback   m_onLog;

    void pushDibit(uint8_t d);
    void tryExtract();
    static uint8_t sliceValue(float sym, float th);
    void processFrame();
    void decodeTsbkBlock();
    void sliceSymbol(float sym);
    bool decodeNid(uint64_t codeword, uint16_t* outNac, uint8_t* outDuid);
    void log(const std::string& s) { if (m_onLog) m_onLog(s); }
};

} // namespace p25rx
