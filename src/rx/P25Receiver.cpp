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
    m_frameIdx = 0;
    m_skipSS   = false;
    m_dc       = 0.0f;
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
    for (long i = i0; i <= i1; ++i) sum += m_fmHist[size_t(i - long(m_fmBase))];
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

    // Symbol timing recovery (Gardner) at fractional samples/symbol.
    const double half = 0.5;
    long long guard = 0;
    while (true) {
        if (++guard > (1LL << 26)) break;
        if (!std::isfinite(m_nextPos) || !std::isfinite(m_spsEst) || m_spsEst <= 0.0) {
            m_spsEst = m_sps;
            m_nextPos = double(m_fmBase + m_fmHist.size());
            break;
        }
        double pos    = m_nextPos;
        double midPos = pos - m_spsEst * half;
        if (pos + 1.0 >= double(m_fmBase + m_fmHist.size())) break;      // need future sample
        if (midPos >= 0.0 && midPos + 1.0 < double(m_fmBase)) break;

        float cur = interpAt(pos);
        float mid = interpAt(midPos);

        if (m_haveSym) {
            float e = mid * (cur - m_symPrev);      // Gardner error
            m_spsEst += 0.00015 * e;
            if (m_spsEst < m_sps * 0.995) m_spsEst = m_sps * 0.995;
            if (m_spsEst > m_sps * 1.005) m_spsEst = m_sps * 1.005;
        }
        m_symPrev = cur;
        m_haveSym = true;

        sliceSymbol(cur);
        m_stats.symbols++;
        m_nextPos += m_spsEst;
    }

    // Trim consumed history, keeping a margin behind the next symbol.
    size_t keepFromPos = (m_nextPos > m_sps * 4.0) ? size_t(m_nextPos - m_sps * 4.0) : 0;
    if (keepFromPos > m_fmBase && keepFromPos - m_fmBase > 4096 &&
        keepFromPos - m_fmBase <= m_fmHist.size()) {
        size_t drop = keepFromPos - m_fmBase;
        m_fmHist.erase(m_fmHist.begin(), m_fmHist.begin() + drop);
        m_fmBase += drop;
    }
}

void P25Receiver::sliceSymbol(float sym)
{
    // sym is already normalized (outer ~ +/-3, inner ~ +/-1) via m_fmScale.
    float v = sym;

    if (sym < m_stats.symMin) m_stats.symMin = sym;
    if (sym > m_stats.symMax) m_stats.symMax = sym;
    {
        int b = int((sym + 6.0f) / 12.0f * 24.0f);
        if (b < 0) b = 0;
        if (b > 23) b = 23;
        m_stats.symHist[b]++;
    }

    uint8_t d;
    if (v > 2.0f)        d = 0x1;  // +3 -> 01
    else if (v >= 0.0f)  d = 0x0;  // +1 -> 00
    else if (v >= -2.0f) d = 0x2;  // -1 -> 10
    else                 d = 0x3;  // -3 -> 11

    if (m_invert) d ^= 0x2;
    m_stats.dibitCount[d & 3]++;
    pushDibit(d);
}

void P25Receiver::pushDibit(uint8_t d)
{
    d &= 0x3;

    switch (m_state) {
    case State::Hunt: {
        m_fswWin = ((m_fswWin << 2) | d) & 0xFFFFFFFFFFFFull;
        uint64_t fsw = p25::kFrameSync & 0xFFFFFFFFFFFFull;
        int dist = popcount64(m_fswWin ^ fsw);
        if (dist < m_stats.minFswDist) m_stats.minFswDist = dist;
        int distInv = popcount64(m_fswWin ^ (fsw ^ 0xAAAAAAAAAAAAull));
        if (distInv < m_stats.minFswDistInv) m_stats.minFswDistInv = distInv;
        if (dist <= 8) {
            // FSW = frame dibits 1..24 (the first status symbol comes only after
            // frame dibit 35), so the 24 dibits just seen are frame 1..24.
            m_stats.fswHits++;
            m_state    = State::Frame;
            m_frameIdx = 24;
            m_skipSS   = false;
        }
        break;
    }
    case State::Frame: {
        // insertStatusSymbols() inserts one status symbol after every 35th
        // FRAME dibit (across the whole frame, so one lands inside the NID too).
        if (m_skipSS) { m_skipSS = false; break; }
        ++m_frameIdx;                                // frame dibit 25..154
        if (m_frameIdx <= 154) {
            m_frameBuf[m_frameIdx - 25] = d;         // buffer 0..129
            if (m_frameIdx % m_ssInterval == 0) m_skipSS = true;
            if (m_frameIdx == 154) processFrame();
        }
        break;
    }
    }
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
