#include "rx/VoiceRepeater.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace p25rx {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
inline uint64_t nowMs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

bool VoiceRepeater::configure(double sampleRateHz, double mixOffsetHz)
{
    m_enabled.store(false, std::memory_order_relaxed);
    if (!(sampleRateHz > 0.0)) return false;

    // Demodulate at ~96 kHz: high enough that the talker's already-shaped C4FM
    // waveform survives linear up-sampling to the transmit rate, low enough to
    // keep the per-sample atan2 and FIR cost reasonable.
    m_decim     = std::max(1, int(std::lround(sampleRateHz / 96000.0)));
    m_demodRate = sampleRateHz / double(m_decim);
    if (m_demodRate / 4800.0 < 4.0) return false;

    m_sampleRate  = sampleRateHz;
    m_mixOffsetHz = mixOffsetHz;
    // Normalise so an outer symbol (+/-1800 Hz) becomes +/-3, matching the
    // transmitter's symbol units and preserving the talker's deviation.
    m_fmScale = float(3.0 * m_demodRate / (2.0 * M_PI * 1800.0));

    // Windowed-sinc low-pass applied at the SDR rate before decimation. This is
    // what rejects the (much stronger) control-channel uplink, which sits a few
    // hundred kHz away from the voice uplink after mixing.
    m_firLen = std::max(31, 16 * m_decim);
    if ((m_firLen & 1) == 0) ++m_firLen;
    m_firTaps.assign(m_firLen, 0.0f);
    const double fc     = 0.40 / double(m_decim);
    const double center = (m_firLen - 1) / 2.0;
    double sum = 0.0;
    for (int i = 0; i < m_firLen; ++i) {
        double n = double(i) - center;
        double s = (n == 0.0) ? 1.0 : std::sin(2.0 * M_PI * fc * n) / (2.0 * M_PI * fc * n);
        double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / double(m_firLen - 1));  // Hann
        m_firTaps[i] = float(2.0 * fc * s * w);
        sum += m_firTaps[i];
    }
    if (sum != 0.0)
        for (auto& t : m_firTaps) t = float(double(t) / sum);

    m_hangMax = 30;   // 30 x 5 ms = 150 ms
    reset();
    m_enabled.store(true, std::memory_order_relaxed);
    return true;
}

void VoiceRepeater::disable()
{
    // Do not touch the sample ring here: the control-channel mixer may still be
    // draining it. configure()/reset() clears it for the next start.
    m_enabled.store(false, std::memory_order_relaxed);
    m_squelch.store(false, std::memory_order_relaxed);
}

void VoiceRepeater::reset()
{
    m_mixPhase = 0.0;
    m_decimN   = 0;
    m_firPos   = 0;
    m_firI.assign(size_t(std::max(1, m_firLen)), 0.0f);
    m_firQ.assign(size_t(std::max(1, m_firLen)), 0.0f);
    m_havePrev = false;
    m_prevI = m_prevQ = 0.0f;
    m_dc      = 0.0f;
    m_pwrAcc  = 0.0;
    m_pwrN    = 0;
    m_nf      = 0.0f;
    m_hang    = 0;
    m_openRun = 0;
    m_calBlocks = 100;   // ~500 ms at the 5 ms block rate
    m_openBlocks = 0;
    m_coolDown   = 0;
    m_lastFrameMs.store(0, std::memory_order_relaxed);
    m_squelch.store(false, std::memory_order_relaxed);
    m_stats = Stats{};
    if (m_ring.size() != kRingSize)
        m_ring.assign(kRingSize, 0.0f);
    else
        std::fill(m_ring.begin(), m_ring.end(), 0.0f);
    m_wpos.store(0, std::memory_order_relaxed);
    m_rpos.store(0, std::memory_order_relaxed);
}

void VoiceRepeater::noteVoiceFrame()
{
    m_lastFrameMs.store(nowMs(), std::memory_order_relaxed);
}

void VoiceRepeater::writeFm(float v)
{
    size_t w = m_wpos.load(std::memory_order_relaxed);
    size_t r = m_rpos.load(std::memory_order_acquire);
    if (w - r >= kRingSize) {
        // Consumer is not keeping up (should not happen with matched clocks):
        // drop this sample rather than block the RX thread.
        return;
    }
    m_ring[w % kRingSize] = v;
    m_wpos.store(w + 1, std::memory_order_release);
}

bool VoiceRepeater::readFm(float& out)
{
    size_t r = m_rpos.load(std::memory_order_relaxed);
    size_t w = m_wpos.load(std::memory_order_acquire);
    if (r == w) return false;
    out = m_ring[r % kRingSize];
    m_rpos.store(r + 1, std::memory_order_release);
    return true;
}

size_t VoiceRepeater::available() const
{
    size_t w = m_wpos.load(std::memory_order_acquire);
    size_t r = m_rpos.load(std::memory_order_relaxed);
    return w - r;
}

void VoiceRepeater::process(const float* iq, size_t n)
{
    if (!iq || n == 0 || !m_enabled.load(std::memory_order_relaxed) || m_firLen <= 0) return;

    const double mixInc = 2.0 * M_PI * m_mixOffsetHz / m_sampleRate;
    const int    L      = m_firLen;
    const int    pwrBlock = std::max(1, int(m_demodRate / 200.0));  // 5 ms

    for (size_t k = 0; k < n; ++k) {
        float I = iq[2 * k];
        float Q = iq[2 * k + 1];

        // Down-convert the voice uplink to DC.
        if (m_mixOffsetHz != 0.0) {
            const double c = std::cos(m_mixPhase);
            const double s = std::sin(m_mixPhase);
            const float mI = float(double(I) * c - double(Q) * s);
            const float mQ = float(double(I) * s + double(Q) * c);
            I = mI;
            Q = mQ;
            m_mixPhase += mixInc;
            if (m_mixPhase > 2.0 * M_PI)       m_mixPhase -= 2.0 * M_PI;
            else if (m_mixPhase < -2.0 * M_PI) m_mixPhase += 2.0 * M_PI;
        }

        m_firI[m_firPos] = I;
        m_firQ[m_firPos] = Q;

        if (++m_decimN < m_decim) {
            m_firPos = (m_firPos + 1) % L;
            continue;
        }
        m_decimN = 0;

        double sI = 0.0, sQ = 0.0;
        int idx = m_firPos;
        for (int t = 0; t < L; ++t) {
            sI += double(m_firTaps[t]) * m_firI[idx];
            sQ += double(m_firTaps[t]) * m_firQ[idx];
            idx = (idx - 1 + L) % L;
        }
        m_firPos = (m_firPos + 1) % L;

        const float dI = float(sI);
        const float dQ = float(sQ);
        m_stats.samples++;

        // FM discriminator (angle between consecutive complex samples).
        float fm = 0.0f;
        if (m_havePrev) {
            const float re = dI * m_prevI + dQ * m_prevQ;
            const float im = dQ * m_prevI - dI * m_prevQ;
            fm = std::atan2(im, re);
        }
        m_prevI = dI;
        m_prevQ = dQ;
        m_havePrev = true;

        // Remove carrier offset / LO error so the repeated signal is centred.
        m_dc += (fm - m_dc) * 0.00035f;
        writeFm((fm - m_dc) * m_fmScale);

        // Squelch on the decimated voice-channel power.
        m_pwrAcc += double(dI) * dI + double(dQ) * dQ;
        if (++m_pwrN >= pwrBlock) {
            const float block = float(m_pwrAcc / double(m_pwrN));
            m_pwrAcc = 0.0;
            m_pwrN   = 0;

            constexpr float kFloor = 1e-6f;

            // Calibrate the noise floor for the first ~500 ms after start.
            // Without this, a silent voice channel (filtered power near zero)
            // makes a plain relative test chatter open on numerical noise.
            if (!m_frameGate && m_calBlocks > 0) {
                --m_calBlocks;
                if (m_nf <= 0.0f)     m_nf = block;
                else if (block < m_nf) m_nf += (block - m_nf) * 0.5f;
                else                   m_nf += (block - m_nf) * 0.05f;
                if (m_nf < kFloor) m_nf = kFloor;
                m_openRun = 0;
                m_hang    = 0;
                m_squelch.store(false, std::memory_order_relaxed);
                m_stats.power      = block;
                m_stats.noiseFloor = m_nf;
            } else if (m_frameGate) {
                // Frame-gated squelch: driven by validated P25 voice frames
                // (see noteVoiceFrame). A 1000 ms hang covers the ~180 ms frame
                // cadence (even with a few missed NIDs), and the 30 s time-out
                // timer still breaks a latch.
                const uint64_t last = m_lastFrameMs.load(std::memory_order_relaxed);
                bool active = (last != 0) && (nowMs() >= last) &&
                              (nowMs() - last < 1000);
                if (m_coolDown > 0) {
                    --m_coolDown;
                    active = false;
                } else if (active) {
                    if (++m_openBlocks > m_totBlocks) {
                        m_openBlocks = 0;
                        m_coolDown   = 200;
                        active       = false;
                    }
                } else {
                    m_openBlocks = 0;
                }
                m_squelch.store(active, std::memory_order_relaxed);
                m_stats.power      = block;
                m_stats.noiseFloor = m_nf;
            } else {
                // Hysteresis: open at 6x the floor, stay open above 2x. A
                // minimum absolute power keeps an empty channel closed. This
                // prevents the call display from being held open by noise.
                const bool overOpen  = (block > kFloor) && (block > m_nf * 6.0f);
                const bool overClose = (block > kFloor) && (block > m_nf * 2.0f);

                if (overOpen) { if (m_openRun < 10) ++m_openRun; }
                else            m_openRun = 0;

                bool openNow = (m_openRun >= 2);          // ~10 ms of signal
                if (m_squelch.load(std::memory_order_relaxed))
                    openNow = openNow && overClose;

                // Time-out timer: cut a transmission (or a self-sustaining
                // squelch / feedback latch) after 30 s of continuous audio and
                // hold closed for 1 s. With our own voice TX muted, the noise
                // floor can then re-learn the true channel instead of the TX
                // leakage that was keeping the squelch open.
                if (m_coolDown > 0) {
                    --m_coolDown;
                    openNow = false;
                } else if (openNow) {
                    if (++m_openBlocks > m_totBlocks) {
                        m_openBlocks = 0;
                        m_coolDown   = 200;
                        m_openRun    = 0;
                        openNow      = false;
                    }
                } else {
                    m_openBlocks = 0;
                }

                if (openNow)         m_hang = m_hangMax;
                else if (m_hang > 0) --m_hang;
                const bool active = openNow || m_hang > 0;
                m_squelch.store(active, std::memory_order_relaxed);

                // Learn the noise floor only while no voice is present, so a
                // long transmission cannot drag the threshold up.
                if (!active) {
                    if (block < m_nf) m_nf += (block - m_nf) * 0.5f;
                    else              m_nf += (block - m_nf) * 0.002f;
                    if (m_nf < kFloor) m_nf = kFloor;
                }
                m_stats.power      = block;
                m_stats.noiseFloor = m_nf;
            }
        }
    }
}

} // namespace p25rx
