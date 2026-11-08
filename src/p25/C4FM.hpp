#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <cmath>

namespace p25 {

constexpr double kSampleRate  = 2'400'000.0;
constexpr double kSymbolRate  = 4'800.0;
constexpr double kDeviationHz = 600.0;
constexpr double kRrcAlpha    = 0.2;
// Longer RRC span reduces the ISI left by truncating the transmit pulse
// (P25 uses an alpha=0.2 raised-cosine family with long tails). Real radios
// slice the matched-filtered waveform and reject a pulse whose truncated
// tails leave too much ISI, even when a tolerant SDR decoder still decodes it.
constexpr int    kRrcSpan     = 10;
constexpr int    kPolyTaps    = kRrcSpan * 2 + 1;          // 21

// Gray-coded dibit → 4-level FM symbol (P25: 00 -> +inner, 01 -> +outer,
// 10 -> -inner, 11 -> -outer). SDRTrunk (a standard P25 receiver) only decodes
// this polarity, so it is the correct on-air convention; a real radio's slicer
// uses the same mapping.
static constexpr float kSymbol[4] = {+1.0f, +3.0f, -1.0f, -3.0f};

class C4FM {
public:
    C4FM();

    // Select the output sample rate. The rate must be an integer multiple of
    // the 4800 sym/s symbol rate (e.g. 2.4e6 -> OSR 500, 1.536e6 -> OSR 320).
    // Returns false and leaves the previous configuration on an invalid rate.
    bool configure(double sampleRateHz);
    double sampleRate() const { return m_sampleRate; }
    int    oversampling() const { return m_osr; }

    // Set the peak (outer ±3 symbol) frequency deviation in Hz. Standard P25
    // C4FM is 1800 Hz peak (inner ±600, outer ±1800). Pass, e.g., 5000 to make
    // the outer symbols swing ±5 kHz as some A/B deviated channels expect.
    void setDeviationPeakHz(double outerHz) {
        m_devPerUnit = (outerHz > 0.0) ? outerHz / 3.0 : kDeviationHz;
    }

    // Modulate dibits → interleaved int8 IQ  (size = dibits.size() * OSR * 2)
    std::vector<int8_t> modulate(const std::vector<uint8_t>& dibits);

private:
    std::vector<std::vector<float>> m_poly;
    std::vector<float> m_history;
    int    m_histPos   = 0;
    double m_phase     = 0.0;
    double m_sampleRate = kSampleRate;
    int    m_osr        = 500;
    double m_devPerUnit = kDeviationHz;   // Hz per symbol unit (±1 -> ±600 by default)

    void buildFilter();
    static double txPulse(double t);
};

} // namespace p25
