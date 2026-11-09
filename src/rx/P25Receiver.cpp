#include "rx/P25Receiver.hpp"

#include "p25/C4FM.hpp"
#include "p25/Frame.hpp"
#include "p25/NID.hpp"
#include "p25/TSBK.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace p25rx {

namespace {

// Trellis used by the transmitter (see p25::trellisEncode). Decoder mirrors it.
const uint8_t kTrellis[4][4][2] = {
    {{0,2},{3,0},{0,1},{3,3}},
    {{3,2},{0,0},{3,1},{0,3}},
    {{2,1},{1,3},{2,2},{1,0}},
    {{1,1},{2,3},{1,2},{2,0}},
};

inline int popcount64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(v);
#else
    int c = 0; while (v) { v &= v - 1; ++c; } return c;
#endif
}

} // namespace

P25Receiver::P25Receiver() = default;

bool P25Receiver::configure(double sampleRateHz, uint16_t nac)
{
    if (!(sampleRateHz > 0.0)) return false;
    // Decimate IQ to ~48 kHz (like dsd-neo base_decim) before FM demodulation.
    m_decim     = std::max(1, int(std::lround(sampleRateHz / 48000.0)));
    m_demodRate = sampleRateHz / double(m_decim);
    const double sps = m_demodRate / p25::kSymbolRate;
    if (sps < 2.0) return false;

    m_sampleRate = sampleRateHz;
    m_sps        = sps;
    m_spsEst     = sps;
    m_nac        = nac;
    // Normalize the discriminator at the demod rate so outer symbol -> +/-3.
    m_fmScale = float(3.0 * m_demodRate / (2.0 * M_PI * 1800.0));

    // Windowed-sinc low-pass for the decimation stage (anti-alias).
    m_firLen = std::max(31, 12 * m_decim);
    if ((m_firLen & 1) == 0) ++m_firLen;
    m_firTaps.assign(m_firLen, 0.0f);
    const double fc = 0.42 / double(m_decim);      // cutoff just below output Nyquist
    const double center = (m_firLen - 1) / 2.0;
    double sum = 0.0;
    for (int i = 0; i < m_firLen; ++i) {
        double n = double(i) - center;
        double s = (n == 0.0) ? 1.0 : std::sin(2.0 * M_PI * fc * n) / (2.0 * M_PI * fc * n);
        double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / double(m_firLen - 1));  // Hann
        m_firTaps[i] = float(2.0 * fc * s * w);
        sum += m_firTaps[i];
    }
    if (sum != 0.0) for (auto& t : m_firTaps) t = float(double(t) / sum);
    m_firI.assign(m_firLen, 0.0f);
    m_firQ.assign(m_firLen, 0.0f);
    m_firPos = 0;

    // Precompute the FSW symbol template (+/-3, +/-1 base polarity) for direct
    // correlation-based sync detection.
    static const float kSymOfDibit[4] = {1.0f, 3.0f, -1.0f, -3.0f};
    double tn = 0.0;
    for (int k = 0; k < 24; ++k) {
        uint8_t db = uint8_t((p25::kFrameSync >> (46 - 2 * k)) & 0x3u);
        m_fswTmpl[k] = kSymOfDibit[db & 3];
        tn += double(m_fswTmpl[k]) * m_fswTmpl[k];
    }
    m_fswTmplNorm = std::sqrt(tn) + 1e-9;

    reset();
    return true;
}

void P25Receiver::reset()
{
    m_havePrev = false;
    m_haveSym  = false;
    m_acc      = 0.0;
    m_decimN   = 0;
    m_decimI   = 0.0;
    m_decimQ   = 0.0;
    m_spsEst   = m_sps;
    m_fswWin   = 0;
    m_state    = State::Hunt;
    m_fswStartPos = 0.0;
    m_dc       = 0.0f;
    m_nf       = 0.0;
    m_symSqAvg = 16.0;
    m_dataDibits.fill(0);
    m_frameBuf.fill(0);
    m_fmHist.clear();
    m_fmBase   = 0;
    m_nextPos  = 0.0;
    m_agc      = 0.0f;
    m_stats    = Stats{};
}

// Linear interpolation of the FM stream at absolute position `pos`.
float P25Receiver::interpAt(double pos) const
{
    if (m_fmHist.size() < 2) return 0.0f;
    if (!(pos == pos)) return 0.0f;                 // NaN guard
    double ip = std::floor(pos);
    size_t idx = (ip < 0.0) ? m_fmBase : size_t(ip);
    if (idx < m_fmBase) idx = m_fmBase;
    size_t li = idx - m_fmBase;
    if (li + 1 >= m_fmHist.size()) li = m_fmHist.size() - 2;
    double frac = pos - ip;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    float a = m_fmHist[li];
    float b = m_fmHist[li + 1];
    return a + float(frac) * (b - a);
}

float P25Receiver::integrateDump(double pos) const
{
    // Average the discriminator output over one symbol period centred on pos.
    // Averages out high-frequency noise; P25 C4FM slices well on this.
    if (m_fmHist.size() < 2) return 0.0f;
    double lo = pos - m_spsEst * 0.5;
    double hi = pos + m_spsEst * 0.5;
    long i0 = long(std::floor(lo));
    long i1 = long(std::floor(hi));
    long last = long(m_fmBase + m_fmHist.size()) - 1;
    if (i0 < long(m_fmBase)) i0 = long(m_fmBase);
    if (i1 > last) i1 = last;
    if (i1 <= i0) return interpAt(pos);
    double sum = 0.0;
    for (long i = i0; i <= i1; ++i) {
        double v = m_fmHist[size_t(i - long(m_fmBase))];
        if (v > 9.0)  v = 9.0;      // reject +/-pi wrap spikes
        if (v < -9.0) v = -9.0;
        sum += v;
    }
    return float(sum / double(i1 - i0 + 1));
}

void P25Receiver::process(const float* iq, size_t n)
{
    if (!iq || n == 0 || m_sps <= 0.0) return;

    // Decimate IQ to the demod rate, then FM-discriminate.
    const double mixInc = 2.0 * M_PI * m_mixOffsetHz / m_sampleRate;
    m_fmHist.reserve(m_fmHist.size() + n / size_t(std::max(1, m_decim)) + 8);
    for (size_t k = 0; k < n; ++k) {
        float I = iq[2 * k];
        float Q = iq[2 * k + 1];
        if (m_mixOffsetHz != 0.0) {
            double c = std::cos(m_mixPhase), s = std::sin(m_mixPhase);
            float mI = float(double(I) * c - double(Q) * s);
            float mQ = float(double(I) * s + double(Q) * c);
            I = mI; Q = mQ;
            m_mixPhase += mixInc;
            if (m_mixPhase > 2.0 * M_PI) m_mixPhase -= 2.0 * M_PI;
        }
        m_decimI += I;
        m_decimQ += Q;
        m_stats.samples++;
        if (++m_decimN < m_decim) continue;
        float dI = float(m_decimI / double(m_decim));
        float dQ = float(m_decimQ / double(m_decim));
        m_decimI = 0.0; m_decimQ = 0.0; m_decimN = 0;

        float fm = 0.0f;
        if (m_havePrev) {
            float re = dI * m_prevI + dQ * m_prevQ;   // dot
            float im = dQ * m_prevI - dI * m_prevQ;   // cross
            fm = std::atan2(im, re);
        }
        m_prevI = dI; m_prevQ = dQ; m_havePrev = true;
        m_dc += (fm - m_dc) * 0.0007f;   // ~30 ms time constant at 48 kHz
        m_fmHist.push_back((fm - m_dc) * m_fmScale);
    }

    // Direct FSW correlation detection. Slide over the FM stream at the symbol
    // rate; a normalized correlation of the 24-symbol sync template above the
    // threshold means the sync word starts here. Scale-invariant (works for any
    // signal level) and polarity-agnostic (|score| picks up inverted IQ too).
    if (m_state == State::Hunt) {
        const double sps = m_sps;
        const double end = double(m_fmBase + m_fmHist.size());
        while (m_nextPos + 24.0 * sps + 1.0 < end) {
            double p = m_nextPos;
            double corr = 0.0, en = 0.0;
            for (int k = 0; k < 24; ++k) {
                float s = interpAt(p + k * sps);
                corr += double(s) * double(m_fswTmpl[k]);
                en   += double(s) * s;
            }
            m_stats.symbols++;
            if (en > 1e-9) {
                double norm = corr / (std::sqrt(en) * m_fswTmplNorm);
                if (std::fabs(norm) > 0.75) {
                    m_invert = (norm < 0.0);
                    m_stats.fswHits++;
                    m_fswStartPos = p;
                    m_state = State::WaitLock;
                    break;
                }
            }
            m_nextPos += 0.5;                 // half-sample search step
        }
        if (m_state == State::Hunt && m_nextPos + 24.0 * sps + 1.0 >= end) {
            // Not enough future samples: park at the last usable position.
            double limit = end - 24.0 * sps - 1.0;
            if (limit > m_nextPos) m_nextPos = limit;
        }
    }

    // Once a sync word is detected, wait until the whole frame is buffered, then
    // lock timing by FSW correlation and resample the frame.
    if (m_state == State::WaitLock) tryExtract();

    // Trim consumed history, keeping a margin behind the read cursor (and behind
    // a pending sync word while waiting for the rest of the frame).
    double keepAt = m_nextPos;
    if (m_state == State::WaitLock) keepAt = std::min(keepAt, m_fswStartPos);
    size_t keepFromPos = (keepAt > m_sps * 4.0) ? size_t(keepAt - m_sps * 4.0) : 0;
    if (keepFromPos > m_fmBase && keepFromPos - m_fmBase > 4096 &&
        keepFromPos - m_fmBase <= m_fmHist.size()) {
        size_t drop = keepFromPos - m_fmBase;
        m_fmHist.erase(m_fmHist.begin(), m_fmHist.begin() + drop);
        m_fmBase += drop;
    }
}

uint8_t P25Receiver::sliceValue(float sym, float th)
{
    // Adaptive 4-level slice. `th` is the inner/outer boundary (midway between
    // the inner and outer symbol magnitudes); derived from the running symbol
    // power so it tracks the real signal levels regardless of deviation.
    uint8_t d;
    if (sym > th)         d = 0x1;  // outer +  (01)
    else if (sym >= 0.0f) d = 0x0;  // inner +  (00)
    else if (sym >= -th)  d = 0x2;  // inner -  (10)
    else                  d = 0x3;  // outer -  (11)
    return d;
}

void P25Receiver::sliceSymbol(float sym)
{
    if (sym < m_stats.symMin) m_stats.symMin = sym;
    if (sym > m_stats.symMax) m_stats.symMax = sym;
    {
        int b = int((sym + 6.0f) / 12.0f * 24.0f);
        if (b < 0) b = 0;
        if (b > 23) b = 23;
        m_stats.symHist[b]++;
    }

    // Fixed slice boundary at the midpoint between inner and outer levels for
    // the received (real) signal; the FSW-locked integrate-and-dump extraction
    // below provides the noise rejection.
    (void)m_symSqAvg;
    uint8_t d = sliceValue(sym, 2.0f);
    if (m_invert) d ^= 0x2;
    m_stats.dibitCount[d & 3]++;
    pushDibit(d);
}

void P25Receiver::pushDibit(uint8_t d)
{
    d &= 0x3;
    if (m_state != State::Hunt) return;

    m_fswWin = ((m_fswWin << 2) | d) & 0xFFFFFFFFFFFFull;
    const uint64_t fsw = p25::kFrameSync & 0xFFFFFFFFFFFFull;
    int dist = popcount64(m_fswWin ^ fsw);
    if (dist < m_stats.minFswDist) m_stats.minFswDist = dist;
    int distInv = popcount64(m_fswWin ^ (fsw ^ 0xAAAAAAAAAAAAull));
    if (distInv < m_stats.minFswDistInv) m_stats.minFswDistInv = distInv;
    if (dist <= m_fswMaxDist) {
        // The 24 dibits just sliced are FSW symbols 1..24; the last one is at
        // m_nextPos, so symbol 1 is 23 symbols earlier.
        m_stats.fswHits++;
        m_fswStartPos = m_nextPos - 23.0 * m_spsEst;
        m_state = State::WaitLock;
    }
}

void P25Receiver::tryExtract()
{
    const double sps = m_sps;
    // Need samples through frame dibit 154's on-air position (158) + margin.
    const double endAbs = m_fswStartPos + 159.0 * sps + 4.0;
    if (double(m_fmBase + m_fmHist.size()) < endAbs) return;  // still waiting

    // Expected FSW symbol values (frame dibits 1..24 -> +/-1/3), polarity-aware.
    float tmpl[24];
    double tmplEnergy = 0.0;
    static const float kSymOfDibit[4] = {1.0f, 3.0f, -1.0f, -3.0f};
    for (int k = 0; k < 24; ++k) {
        uint8_t dibit = uint8_t((p25::kFrameSync >> (46 - 2 * k)) & 0x3u);
        float v = kSymOfDibit[dibit & 3];
        if (m_invert) v = -v;
        tmpl[k] = v;
        tmplEnergy += double(v) * v;
    }
    const double tmplNorm = std::sqrt(tmplEnergy) + 1e-9;

    // Correlate the template against the buffered FM over a window around the
    // detected position to find the exact sub-sample symbol phase.
    double bestPos = m_fswStartPos;
    double bestScore = -1e30;
    const double lo = m_fswStartPos - 1.25 * sps;
    const double hi = m_fswStartPos + 1.25 * sps;
    for (double p = lo; p <= hi; p += 0.25) {
        if (p < double(m_fmBase)) continue;
        if (p + 23.0 * sps + 1.0 >= double(m_fmBase + m_fmHist.size())) continue;
        double sc = 0.0, en = 0.0;
        for (int k = 0; k < 24; ++k) {
            float s = interpAt(p + k * sps);
            sc += double(s) * tmpl[k];
            en += double(s) * s;
        }
        if (en <= 1e-9) continue;
        double norm = sc / (std::sqrt(en) * tmplNorm);
        if (norm > bestScore) { bestScore = norm; bestPos = p; }
    }

    if (bestScore < 0.35) {
        // Not a genuine sync (spurious match); resume hunting past this region.
        m_nextPos = m_fswStartPos + 158.0 * sps;
        m_state = State::Hunt;
        m_fswWin = 0;
        return;
    }

    // The Pluto's RX clock and the radio's TX clock are independent, so the
    // uplink can arrive a few hundred Hz to a few kHz off frequency. That adds
    // a DC bias to the discriminator (outer is only +/-1800 Hz, inner +/-600 Hz),
    // which the slow m_dc tracker cannot remove within one short burst. Fit the
    // known 24-symbol FSW (received = amp*template + dc) and remove the residual
    // offset; normalise by amp as well so a different radio deviation still
    // lands on the +/-1/3 slicer.
    double meanR = 0.0, meanT = 0.0;
    float rsw[24];
    for (int k = 0; k < 24; ++k) {
        rsw[k]  = interpAt(bestPos + double(k) * sps);
        meanR  += double(rsw[k]);
        meanT  += double(tmpl[k]);
    }
    meanR /= 24.0; meanT /= 24.0;
    double cov = 0.0, varT = 0.0;
    for (int k = 0; k < 24; ++k) {
        cov  += (double(rsw[k]) - meanR) * (double(tmpl[k]) - meanT);
        varT += (double(tmpl[k]) - meanT) * (double(tmpl[k]) - meanT);
    }
    double amp = (varT > 1e-9) ? cov / varT : 1.0;
    if (!(amp > 0.2)) amp = 0.2;      // also catches NaN
    if (amp > 5.0) amp = 5.0;
    const double dc = meanR - amp * meanT;
    m_stats.lastAmp = float(amp);
    m_stats.lastDc  = float(dc);

    // Frame dibit j sits at on-air symbol j + floor((j-1)/ss); resample it there.
    // Integrate over the symbol (matched filter) for noise rejection rather than
    // sampling a single point; with FSW-locked timing this is stable.
    const int ss = (m_ssInterval > 0) ? m_ssInterval : 0;
    for (int j = 25; j <= 154; ++j) {
        int oa = (ss > 0) ? (j + (j - 1) / ss) : j;   // 1-based on-air symbol index
        float s = integrateDump(bestPos + double(oa - 1) * sps);
        s = float((double(s) - dc) / amp);
        uint8_t d = sliceValue(s, 2.0f);
        if (m_invert) d ^= 0x2;
        m_frameBuf[j - 25] = d;
    }
    processFrame();

    // Resume hunting past the last on-air symbol of this frame.
    m_nextPos = bestPos + 158.0 * sps + sps;
    m_symPrev = 0.0f;
    m_haveSym = false;
    m_state = State::Hunt;
    m_fswWin = 0;
}

void P25Receiver::processFrame()
{
    // NID = frame dibits 25..56  -> frameBuf[0..31]
    // TSBK interleaved = frame dibits 57..154 -> frameBuf[32..129]  (98 dibits)
    uint64_t nidBits = 0;
    for (int i = 0; i < 32; ++i) nidBits = (nidBits << 2) | m_frameBuf[i];

    m_stats.framesTried++;
    uint16_t nac = 0; uint8_t duid = 0;
    if (decodeNid(nidBits, &nac, &duid) && duid == p25::DUID_TSBK &&
        (m_nac == 0 || nac == m_nac)) {
        m_stats.nidOk++;
        for (int i = 0; i < 98; ++i) m_dataDibits[i] = m_frameBuf[32 + i];
        decodeTsbkBlock();
    }
    m_state  = State::Hunt;
    m_fswWin = 0;
}

bool P25Receiver::decodeNid(uint64_t codeword, uint16_t* outNac, uint8_t* outDuid)
{
    // Brute force over 16-bit data with DUID = TSBK (only 4096 candidates).
    int best = std::numeric_limits<int>::max();
    uint16_t bestData = 0;
    for (uint32_t data = 0; data < 0x10000u; ++data) {
        if ((data & 0xF) != p25::DUID_TSBK) continue;
        uint64_t cw = p25::encodeBCH(uint16_t(data));
        int dist = popcount64(cw ^ codeword);
        if (dist < best) { best = dist; bestData = uint16_t(data); if (dist == 0) break; }
    }
    if (best > 16) return false;
    *outNac  = uint16_t(bestData >> 4);
    *outDuid = uint8_t(bestData & 0xF);
    return true;
}

void P25Receiver::decodeTsbkBlock()
{
    // Undo the data interleave (inverse permutation of p25::dataInterleave).
    int fwd[98]; int k = 0;
    for (int j = 0; j < 97; j += 8) { fwd[k++] = j; fwd[k++] = j + 1; }
    for (int i = 2; i < 7; i += 2)
        for (int j = 0; j < 89; j += 8) { fwd[k++] = i + j; fwd[k++] = i + j + 1; }

    uint8_t deint[98];
    for (int j = 0; j < 98; ++j) deint[fwd[j]] = m_dataDibits[j];

    // Viterbi decode (4-state, rate 1/2), 49 trellis steps (48 data + flush).
    const int INF = 1 << 28;
    int met[4] = {0, INF, INF, INF};
    int prevState[49][4];
    int chosen[49][4];
    for (int n = 0; n < 49; ++n) {
        int nm[4] = {INF, INF, INF, INF};
        for (int s = 0; s < 4; ++s) {
            if (met[s] >= INF) continue;
            for (int d = 0; d < 4; ++d) {
                const uint8_t* out = kTrellis[s][d];
                int cost = (out[0] != deint[2 * n]) + (out[1] != deint[2 * n + 1]);
                int ns = d;   // next state = input dibit
                if (met[s] + cost < nm[ns]) {
                    nm[ns] = met[s] + cost;
                    prevState[n][ns] = s;
                    chosen[n][ns] = d;
                }
            }
        }
        for (int s = 0; s < 4; ++s) met[s] = nm[s];
    }

    int state = 0; int bestMet = met[0];
    for (int s = 1; s < 4; ++s) if (met[s] < bestMet) { bestMet = met[s]; state = s; }

    uint8_t dibits[48];
    int cur = state;
    for (int n = 48; n >= 0; --n) {
        int d = chosen[n][cur];
        if (n < 48) dibits[n] = uint8_t(d);
        cur = prevState[n][cur];
    }

    // 48 dibits -> 12 bytes.
    std::array<uint8_t, 12> tsbk{};
    for (int i = 0; i < 12; ++i)
        tsbk[i] = uint8_t((dibits[4 * i] << 6) | (dibits[4 * i + 1] << 4) |
                          (dibits[4 * i + 2] << 2) | dibits[4 * i + 3]);

    uint16_t hi = uint16_t((tsbk[0] << 8) | tsbk[1]);
    uint64_t lo = 0;
    for (int i = 2; i < 10; ++i) lo = (lo << 8) | tsbk[i];
    uint16_t crc = uint16_t((tsbk[10] << 8) | tsbk[11]);
    if (p25::crcCCITT(hi, lo) != crc) return;

    m_stats.crcOk++;
    m_stats.tsbks++;
    if (m_onTsbk) m_onTsbk(tsbk);
}

} // namespace p25rx
