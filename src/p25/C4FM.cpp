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

// P25 C4FM transmit pulse. TIA-102 C4FM is NOT root-raised-cosine: the
// transmitter applies a full raised-cosine (alpha=0.2) low-pass, multiplied by
// an inverse-sinc that pre-compensates the receiver's integrate-and-dump sinc
// response. A subscriber demodulator slices this pulse directly, so a
// root-raised-cosine (what SiteSim used before) leaves ISI and the radio never
// locks even though a tolerant SDR decoder still decodes it.
namespace {
constexpr double kPi = 3.14159265358979323846;

double rcFreq(double f, double Rs, double alpha) {
    const double f1 = (1.0 - alpha) * Rs * 0.5;
    const double f2 = (1.0 + alpha) * Rs * 0.5;
    f = std::abs(f);
    if (f <= f1) return 1.0;
    if (f <= f2) return 0.5 * (1.0 + std::cos(kPi / (alpha * Rs) * (f - f1)));
    return 0.0;
}
} // namespace

double C4FM::txPulse(double t) {
    // Plain raised-cosine (alpha=0.2), matching MMDVM's P25TX rcosdesign(0.2,8,5)
    // transmit filter. (MMDVM is the reference that real P25 radios decode.)
    const double Rs   = kSymbolRate;
    const double a    = kRrcAlpha;
    const double fmax = (1.0 + a) * Rs * 0.5;
    const int    N    = 400;
    const double df   = fmax / N;
    double sum = 0.0;
    for (int k = 0; k < N; k++) {
        const double f = (k + 0.5) * df;
        const double H = rcFreq(f, Rs, a);
        sum += H * std::cos(2.0 * kPi * f * t);
    }
    return 2.0 * sum * df;
}

void C4FM::buildFilter() {
    const int    n      = kRrcSpan * 2 * m_osr + 1;
    const double centre = double(n - 1) / 2.0;

    std::vector<float> h(n);
    for (int i = 0; i < n; i++) {
        double t = (double(i) - centre) / double(m_osr) / kSymbolRate;
        h[i] = float(txPulse(t));
    }

    // Normalise so centre tap == 1.0
    float scale = (h[n / 2] != 0.0f) ? (1.0f / h[n / 2]) : 1.0f;
    for (auto& v : h) v *= scale;

    // Split into OSR polyphase components: poly[p][j] = h[j*OSR + p]
    m_poly.assign(m_osr, std::vector<float>(kPolyTaps, 0.0f));
    for (int p = 0; p < m_osr; p++) {
        for (int j = 0; j < kPolyTaps; j++) {
            int idx = j * m_osr + p;
            if (idx < n)
                m_poly[p][j] = h[idx];
        }
    }
}

C4FM::C4FM() {
    m_history.assign(kPolyTaps, 0.0f);
    buildFilter();
}

bool C4FM::configure(double sampleRateHz) {
    if (!(sampleRateHz > 0.0))
        return false;
    const double osr = sampleRateHz / kSymbolRate;
    const int rounded = int(std::lround(osr));
    if (rounded <= 0 || std::abs(osr - double(rounded)) > 1e-6)
        return false;

    m_sampleRate = sampleRateHz;
    m_osr        = rounded;
    m_histPos    = 0;
    m_phase      = 0.0;
    m_history.assign(kPolyTaps, 0.0f);
    buildFilter();
    return true;
}

std::vector<int8_t> C4FM::modulate(const std::vector<uint8_t>& dibits) {
    std::vector<int8_t> out;
    out.reserve(dibits.size() * m_osr * 2);

    const double phaseInc = 2.0 * M_PI * m_devPerUnit / m_sampleRate;

    for (uint8_t d : dibits) {
        float sym = kSymbol[d & 3];

        m_history[m_histPos] = sym;
        m_histPos = (m_histPos + 1) % kPolyTaps;

        for (int p = 0; p < m_osr; p++) {
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
