#include "C4FM.hpp"
#include <cmath>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

namespace p25 {

double C4FM::rrcSample(double t, double T, double alpha) {
    const double eps = 1e-9;
    if (std::abs(t) < eps)
        return 1.0 - alpha + 4.0 * alpha / M_PI;
    if (std::abs(std::abs(t) - T / (4.0 * alpha)) < eps)
        return alpha / M_SQRT2 * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * alpha)) +
                                   (1 - 2 / M_PI) * std::cos(M_PI / (4 * alpha)));
    double tN  = t / T;
    double num = std::sin(M_PI * tN * (1 - alpha)) + 4 * alpha * tN * std::cos(M_PI * tN * (1 + alpha));
    double den = M_PI * tN * (1 - (4 * alpha * tN) * (4 * alpha * tN));
    return num / den;
}

void C4FM::buildFilter() {
    const int    n      = kRrcTaps;
    const double centre = double(n - 1) / 2.0;
    const double T      = 1.0 / kSymbolRate;

    std::vector<float> h(n);
    for (int i = 0; i < n; i++) {
        double t = (double(i) - centre) / double(kOSR) / kSymbolRate;
        h[i] = float(rrcSample(t, T, kRrcAlpha));
    }

    // Normalise so centre tap == 1.0
    float scale = (h[n / 2] != 0.0f) ? (1.0f / h[n / 2]) : 1.0f;
    for (auto& v : h) v *= scale;

    // Split into OSR polyphase components: poly[p][j] = h[j*OSR + p]
    for (int p = 0; p < kOSR; p++) {
        m_poly[p].resize(kPolyTaps, 0.0f);
        for (int j = 0; j < kPolyTaps; j++) {
            int idx = j * kOSR + p;
            if (idx < n)
                m_poly[p][j] = h[idx];
        }
    }
}

C4FM::C4FM() {
    m_history.assign(kPolyTaps, 0.0f);
    buildFilter();
}

std::vector<int8_t> C4FM::modulate(const std::vector<uint8_t>& dibits) {
    std::vector<int8_t> out;
    out.reserve(dibits.size() * kOSR * 2);

    const double phaseInc = 2.0 * M_PI * kDeviationHz / kSampleRate;

    for (uint8_t d : dibits) {
        float sym = kSymbol[d & 3];

        m_history[m_histPos] = sym;
        m_histPos = (m_histPos + 1) % kPolyTaps;

        for (int p = 0; p < kOSR; p++) {
            float acc = 0.0f;
            const auto& comp = m_poly[p];
            for (int j = 0; j < kPolyTaps; j++) {
                int idx = (m_histPos - 1 - j + kPolyTaps) % kPolyTaps;
                acc += comp[j] * m_history[idx];
            }
            m_phase += double(acc) * phaseInc;
            out.push_back(int8_t(std::cos(m_phase) * 100.0));
            out.push_back(int8_t(std::sin(m_phase) * 100.0));
        }
    }
    return out;
}

} // namespace p25
