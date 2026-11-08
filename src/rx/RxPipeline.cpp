#include "rx/RxPipeline.hpp"
#include "p25/TSBK.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdint>

#if SITESIM_HAVE_DSDNEO

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/p25_isp_hooks.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
// Map POSIX fd calls to their MSVCRT underscore equivalents.
#define sitesim_pipe(fds)   _pipe((fds), 4096, _O_BINARY)
#define sitesim_read        _read
#define sitesim_close       _close
#define sitesim_dup         _dup
#define sitesim_dup2        _dup2
#define STDERR_FILENO       2
#else
#include <unistd.h>
#include <fcntl.h>
#define sitesim_pipe(fds)   pipe(fds)
#define sitesim_read        read
#define sitesim_close       close
#define sitesim_dup         dup
#define sitesim_dup2        dup2
#endif

// Global pointer for C-style hook callbacks — RxPipeline is a singleton in practice.
static RxPipeline* g_rx_pipeline = nullptr;

// ─────────────────────────────────────────────────────────────────────────────

RxPipeline::RxPipeline(ControlChannel& cc, LogCallback log)
    : m_cc(cc), m_log(std::move(log))
{
    g_rx_pipeline = this;
}

RxPipeline::~RxPipeline()
{
    stop();
    if (g_rx_pipeline == this)
        g_rx_pipeline = nullptr;
}

void RxPipeline::configure(const SiteConfig& cfg)
{
    m_cfg = cfg;
    m_cfg.normalizeChannelPlan();
}

void RxPipeline::useRtlsdrSource()
{
    m_soapy          = false;
    m_shareDevice    = false;
    m_externalDevice = nullptr;
    m_soapyArgs.clear();
    m_soapyProfile.clear();
    m_soapyRate      = 0;
}

void RxPipeline::useSoapySource(const std::string& args, const std::string& profile,
                                uint32_t sampleRateHz)
{
    m_soapy          = true;
    m_shareDevice    = false;
    m_externalDevice = nullptr;
    m_soapyArgs      = args;
    m_soapyProfile   = profile;
    m_soapyRate      = sampleRateHz;
}

void RxPipeline::useSharedSoapyDevice(void* device, const std::string& args,
                                      const std::string& profile, uint32_t sampleRateHz)
{
    m_soapy          = true;
    m_shareDevice    = true;
    m_externalDevice = device;
    m_soapyArgs      = args;
    m_soapyProfile   = profile;
    m_soapyRate      = sampleRateHz;
}

std::string RxPipeline::sourceName() const
{
    if (!m_soapy) return "RTL-SDR";
    std::string text = m_soapyProfile + " " + m_soapyArgs;
    for (auto& ch : text) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (text.find("pluto") != std::string::npos || text.find("adalm") != std::string::npos ||
        text.find("ad936") != std::string::npos)
        return "ADALM-Pluto";
    return "SoapySDR";
}

bool RxPipeline::start()
{
    if (m_running.load()) return true;

    // Allocate and initialise dsd-neo state
    m_opts  = new dsd_opts{};
    m_state = new dsd_state{};
    initOpts(m_opts);
    initState(m_state);

    // ── RTL-SDR input ──────────────────────────────────────────────────────
    m_opts->audio_in_type       = AUDIO_IN_RTL;
    if (m_soapy) {
        if (m_soapyArgs.empty())
            snprintf(m_opts->audio_in_dev, sizeof(m_opts->audio_in_dev), "soapy");
        else
            snprintf(m_opts->audio_in_dev, sizeof(m_opts->audio_in_dev), "soapy:%s", m_soapyArgs.c_str());
        snprintf(m_opts->soapy_profile, sizeof(m_opts->soapy_profile), "%s", m_soapyProfile.c_str());
        m_opts->soapy_sample_rate_hz = static_cast<int>(m_soapyRate);
    } else {
        snprintf(m_opts->audio_in_dev, sizeof(m_opts->audio_in_dev), "rtl");
        m_opts->rtl_dev_index       = 0;
    }
    m_opts->rtlsdr_center_freq  = static_cast<uint32_t>(m_cfg.rxFreqHz());
    m_opts->rtl_gain_value      = 0;     // 0 = AGC (auto-gain)
    m_opts->rtlsdr_ppm_error    = 0;     // no manual PPM correction
    m_opts->rtl_dsp_bw_khz      = 24;    // 24 kHz -> 5 samples/symbol for P25P1

    // Zero-IF Soapy SDRs (Pluto) should tune directly to the signal. dsd-neo's
    // fs/4 fallback moves the LO 384 kHz off-center, which the narrow Soapy RF
    // bandwidth then rejects. Native RTL keeps the default fs/4 behavior.
    disable_fs4_shift = m_soapy ? 1 : 0;

    // On full-duplex radios the transmitter owns the Soapy device; hand that
    // same handle to dsd-neo so RX shares it instead of opening a second one.
    rtl_device_set_external_soapy_device(
        (m_soapy && m_shareDevice) ? m_externalDevice : nullptr);

    // ── P25 Phase 1 only — disable all other protocols to prevent crashes ──
    // (other decoders call private vocoder stubs that can't safely execute)
    m_opts->frame_p25p1         = 1;
    m_opts->frame_p25p2         = 0;
    m_opts->frame_dstar         = 0;
    m_opts->frame_x2tdma        = 0;
    m_opts->frame_nxdn48        = 0;
    m_opts->frame_nxdn96        = 0;
    m_opts->frame_dmr           = 0;
    m_opts->frame_provoice      = 0;
    m_opts->frame_dpmr          = 0;
    m_opts->frame_ysf           = 0;
    m_opts->frame_m17           = 0;
    m_opts->p25_trunk           = 0;     // site input: no trunking follow
    m_opts->p25_isp_mode        = 1;     // interpret TSBKs as ISP (mobile→site)
    m_opts->errorbars           = 1;     // enable Sync:/NAC: status lines
    m_opts->verbose             = 2;     // show SPS and sync detail in log

    // ── Install ISP TSBK hook ─────────────────────────────────────────────
    dsd_p25_isp_hooks isp_hooks{};
    isp_hooks.on_isp_tsbk = &RxPipeline::onIspTsbk;
    isp_hooks.user        = this;
    dsd_p25_isp_hooks_set(isp_hooks);

    // ── Install event log hook ────────────────────────────────────────────
    dsd_p25_optional_hooks opt_hooks{};
    opt_hooks.write_event_to_log_file = &RxPipeline::onWriteEvent;
    dsd_p25_optional_hooks_set(opt_hooks);

    m_running.store(true);
    exitflag = 0;
    startStderrCapture();

    char startMsg[128];
    snprintf(startMsg, sizeof(startMsg), "RX started — %s @ %.4f MHz (P25P1 ISP mode)",
             sourceName().c_str(), m_cfg.rxFreqHz() / 1e6);
    m_log(startMsg);

    m_thread = std::thread(&RxPipeline::engineThread, this);
    return true;
}

void RxPipeline::stop()
{
    m_running.store(false);
    if (m_thread.joinable()) {
        exitflag = 1;      // signals dsd_engine_run() to exit its loop
        m_thread.join();
    }

    // RX is done with the borrowed device; the transmitter still owns it.
    rtl_device_set_external_soapy_device(nullptr);

    stopStderrCapture();

    if (m_opts) {
        // Clear hooks before freeing so dangling calls cannot fire.
        dsd_p25_isp_hooks_set({});
        dsd_p25_optional_hooks_set({});
    }

    if (m_state) { freeState(m_state); delete m_state; m_state = nullptr; }
    if (m_opts)  { delete m_opts;  m_opts  = nullptr; }
}

// ── Static thread entry ───────────────────────────────────────────────────────

void RxPipeline::engineThread(RxPipeline* self)
{
    dsd_engine_run(self->m_opts, self->m_state);
    self->m_running.store(false);
}

// ── ISP TSBK callback (called on engine thread) ───────────────────────────────

void RxPipeline::onIspTsbk(uint8_t opcode, const uint8_t raw12[12], void* user)
{
    auto* self = static_cast<RxPipeline*>(user);
    if (self) self->handleIspTsbk(opcode, raw12);
}

void RxPipeline::handleIspTsbk(uint8_t opcode, const uint8_t raw12[12])
{
    // Convenience field extractors (TIA-102.AABC-E)
    auto tgid   = static_cast<uint32_t>((raw12[5] << 8) | raw12[6]);
    auto src_id = static_cast<uint32_t>((raw12[7] << 16) | (raw12[8] << 8) | raw12[9]);
    uint8_t options = raw12[2];

    char buf[128];

    switch (opcode) {

    case P25_ISP_GRP_VCH_REQ: {
        // Grant the voice channel selected in the site's channel plan.
        bool emrg = (options >> 7) & 1;
        auto grant = p25::BuildGrpVChGrant(
            emrg ? 0x80 : 0x00,
            m_cfg.chanId,
            m_cfg.vChanNum,
            static_cast<uint16_t>(tgid),
            src_id);
        m_cc.queueTSBK(grant);

        snprintf(buf, sizeof(buf),
                 "GRP_VCH_REQ  SRC:%-8u TGID:%-5u%s -> granted VC %.5f MHz (IDEN %u ch %u)",
                 src_id, tgid, emrg ? " [EMRG]" : "",
                 m_cfg.channelFrequency(m_cfg.vChanNum) / 1e6,
                 unsigned(m_cfg.chanId), unsigned(m_cfg.vChanNum));
        m_log(buf);
        break;
    }

    case P25_ISP_GRP_AFF_Q: {
        // Acknowledge affiliation; queue GRP_AFF_RSP
        auto rsp = p25::BuildGrpAffRsp(false, 0, static_cast<uint16_t>(tgid),
                                       static_cast<uint16_t>(tgid), src_id);
        m_cc.queueTSBK(rsp);

        snprintf(buf, sizeof(buf), "GRP_AFF_REQ  SRC:%-8u TGID:%-5u -> queued AFF_RSP", src_id, tgid);
        m_log(buf);
        break;
    }

    case P25_ISP_U_REG_REQ: {
        // Queue Unit Registration Response
        auto rsp = p25::BuildURegRsp(true, m_cfg.sysid, src_id, src_id);
        m_cc.queueTSBK(rsp);

        snprintf(buf, sizeof(buf), "U_REG_REQ    SRC:%-8u -> queued U_REG_RSP", src_id);
        m_log(buf);
        break;
    }

    case P25_ISP_U_DE_REG_ACK: {
        snprintf(buf, sizeof(buf), "U_DE_REG_ACK SRC:%-8u", src_id);
        m_log(buf);
        break;
    }

    case P25_ISP_EMRG_ALRM_REQ: {
        snprintf(buf, sizeof(buf), "EMRG_ALRM    SRC:%-8u TGID:%-5u *** EMERGENCY ***", src_id, tgid);
        m_log(buf);
        break;
    }

    case P25_ISP_UU_VCH_REQ: {
        uint32_t target = static_cast<uint32_t>((raw12[4] << 16) | (raw12[5] << 8) | raw12[6]);
        snprintf(buf, sizeof(buf), "UU_VCH_REQ   SRC:%-8u TARGET:%-8u", src_id, target);
        m_log(buf);
        break;
    }

    default:
        snprintf(buf, sizeof(buf), "ISP TSBK 0x%02X  SRC:%-8u", opcode, src_id);
        m_log(buf);
        break;
    }
}

// ── dsd-neo event log callback ────────────────────────────────────────────────

void RxPipeline::onWriteEvent(const dsd_opts*, dsd_state*, uint8_t, uint8_t swrite,
                              char* event_string)
{
    if (!g_rx_pipeline || !event_string || swrite == 0) return;
    // swrite==1 means "write to log"; strip trailing newlines
    std::string msg(event_string);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
    if (!msg.empty())
        g_rx_pipeline->m_log(msg);
}

// ── stderr capture — routes dsd-neo console output to RX log ─────────────────

void RxPipeline::startStderrCapture()
{
    int fds[2];
    if (sitesim_pipe(fds) != 0) return;

    m_pipeRead  = fds[0];
    m_pipeWrite = fds[1];

    // Save original stderr and redirect it to the pipe write end. The reader
    // thread does a blocking read; closing the write end in stopStderrCapture()
    // yields EOF and unblocks it (no non-blocking polling needed).
    m_stderrSavedFd = sitesim_dup(STDERR_FILENO);
    sitesim_dup2(m_pipeWrite, STDERR_FILENO);
    fflush(stderr);

    m_stderrThread = std::thread(&RxPipeline::stderrReaderThread, this);
}

void RxPipeline::stopStderrCapture()
{
    // Restore stderr before joining so any final output still drains
    if (m_stderrSavedFd >= 0) {
        fflush(stderr);
        sitesim_dup2(m_stderrSavedFd, STDERR_FILENO);
        sitesim_close(m_stderrSavedFd);
        m_stderrSavedFd = -1;
    }
    // Close pipe write end — this makes the read end return EOF
    if (m_pipeWrite >= 0) { sitesim_close(m_pipeWrite); m_pipeWrite = -1; }
    if (m_stderrThread.joinable()) m_stderrThread.join();
    if (m_pipeRead  >= 0) { sitesim_close(m_pipeRead);  m_pipeRead  = -1; }
}

void RxPipeline::stderrReaderThread(RxPipeline* self)
{
    char buf[512];
    std::string line;

    while (true) {
        int n = sitesim_read(self->m_pipeRead, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;   // EOF (write end closed) or error
        buf[n] = '\0';
        for (int i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                // Strip trailing \r
                while (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty() && self->m_log)
                    self->m_log(line);
                line.clear();
            } else {
                line += buf[i];
            }
        }
    }
    // Flush any partial line
    if (!line.empty() && self->m_log)
        self->m_log(line);
}

#else // !SITESIM_HAVE_DSDNEO
// ─────────────────────────────────────────────────────────────────────────────
// Stub RX pipeline — used on platforms without the dsd-neo backend (e.g. Windows
// until native SDR drivers are added).  Keeps the same public interface so the
// rest of SiteSim builds and runs; reception is simply a no-op.
// ─────────────────────────────────────────────────────────────────────────────

RxPipeline::RxPipeline(ControlChannel& cc, LogCallback log)
    : m_cc(cc), m_log(std::move(log))
{
}

RxPipeline::~RxPipeline()
{
    stop();
}

void RxPipeline::configure(const SiteConfig& cfg)
{
    m_cfg = cfg;
    m_cfg.normalizeChannelPlan();
}

void RxPipeline::useRtlsdrSource()
{
    m_soapy = false;
    m_shareDevice = false;
    m_externalDevice = nullptr;
    m_soapyArgs.clear();
    m_soapyProfile.clear();
    m_soapyRate = 0;
}

void RxPipeline::useSoapySource(const std::string& args, const std::string& profile,
                                uint32_t sampleRateHz)
{
    m_soapy = true;
    m_shareDevice = false;
    m_externalDevice = nullptr;
    m_soapyArgs = args;
    m_soapyProfile = profile;
    m_soapyRate = sampleRateHz;
}

void RxPipeline::useSharedSoapyDevice(void* device, const std::string& args,
                                      const std::string& profile, uint32_t sampleRateHz)
{
    m_soapy = true;
    m_shareDevice = true;
    m_externalDevice = device;
    m_soapyArgs = args;
    m_soapyProfile = profile;
    m_soapyRate = sampleRateHz;
}

std::string RxPipeline::sourceName() const
{
    if (!m_soapy) return "RTL-SDR";
    std::string text = m_soapyProfile + " " + m_soapyArgs;
    for (auto& ch : text) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (text.find("pluto") != std::string::npos)
        return "ADALM-Pluto";
    return "SoapySDR";
}

bool RxPipeline::start()
{
    if (m_running.load()) return true;
    if (m_log)
        m_log("RX unavailable — no dsd-neo backend built for this platform");
    return false;
}

void RxPipeline::stop()
{
    m_running.store(false);
}

void RxPipeline::engineThread(RxPipeline*) {}
void RxPipeline::onIspTsbk(uint8_t, const uint8_t[12], void*) {}
void RxPipeline::handleIspTsbk(uint8_t, const uint8_t[12]) {}
void RxPipeline::onWriteEvent(const dsd_opts*, dsd_state*, uint8_t, uint8_t, char*) {}
void RxPipeline::startStderrCapture() {}
void RxPipeline::stopStderrCapture() {}
void RxPipeline::stderrReaderThread(RxPipeline*) {}

#endif // SITESIM_HAVE_DSDNEO
