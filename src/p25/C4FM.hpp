#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <cmath>

namespace p25 {

constexpr double kSampleRate  = 2'400'000.0;
constexpr double kSymbolRate  = 4'800.0;
constexpr int    kOSR         = 500;
constexpr double kDeviationHz = 600.0;
constexpr double kRrcAlpha    = 0.2;
constexpr int    kRrcSpan     = 4;
constexpr int    kRrcTaps     = kRrcSpan * 2 * kOSR + 1;  // 4001
constexpr int    kPolyTaps    = kRrcSpan * 2 + 1;          // 9

// Gray-coded dibit → 4-level FM symbol
static constexpr float kSymbol[4] = {+1.0f, +3.0f, -1.0f, -3.0f};

class C4FM {
public:
    C4FM();

    // Modulate dibits → interleaved int8 IQ  (size = dibits.size() * kOSR * 2)
    std::vector<int8_t> modulate(const std::vector<uint8_t>& dibits);

private:
    std::array<std::vector<float>, kOSR> m_poly;
    std::vector<float> m_history;
    int    m_histPos = 0;
    double m_phase   = 0.0;

    void buildFilter();
    static double rrcSample(double t, double T, double alpha);
};

} // namespace p25
