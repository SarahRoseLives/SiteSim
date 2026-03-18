#include "rx/RxPipeline.hpp"
#include "p25/TSBK.hpp"

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/p25_isp_hooks.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>

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
    snprintf(m_opts->audio_in_dev, sizeof(m_opts->audio_in_dev), "rtl");
    m_opts->rtlsdr_center_freq  = static_cast<uint32_t>(m_cfg.rxFreqHz());
    m_opts->rtl_gain_value      = 0;     // 0 = AGC (auto-gain)
    m_opts->rtlsdr_ppm_error    = 0;     // no manual PPM correction
    m_opts->rtl_dsp_bw_khz      = 24;    // 24 kHz -> 5 samples/symbol for P25P1
    m_opts->rtl_dev_index       = 0;

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
    snprintf(startMsg, sizeof(startMsg), "RX started — RTL-SDR @ %.4f MHz (P25P1 ISP mode)",
             m_cfg.rxFreqHz() / 1e6);
    m_log(startMsg);

    m_thread = std::thread(&RxPipeline::engineThread, this);
    return true;
}

void RxPipeline::stop()
{
    if (!m_running.load()) return;
    m_running.store(false);
    exitflag = 1;          // signals dsd_engine_run() to exit its loop
    if (m_thread.joinable())
        m_thread.join();

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
        // Queue a Group Voice Channel Grant response
        bool emrg = (options >> 7) & 1;
        snprintf(buf, sizeof(buf), "GRP_VCH_REQ  SRC:%-8u TGID:%-5u%s",
                 src_id, tgid, emrg ? " [EMRG]" : "");
        m_log(buf);
        // TODO: allocate a voice channel and queue GRP_VCH_GRANT TSBK
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

void RxPipeline::onWriteEvent(dsd_opts*, dsd_state*, uint8_t, uint8_t swrite,
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
    if (pipe(fds) != 0) return;

    m_pipeRead  = fds[0];
    m_pipeWrite = fds[1];

    // Make pipe read end non-blocking so reader thread can poll for stop
    int flags = fcntl(m_pipeRead, F_GETFL, 0);
    fcntl(m_pipeRead, F_SETFL, flags | O_NONBLOCK);

    // Save original stderr and redirect it to the pipe write end
    m_stderrSavedFd = dup(STDERR_FILENO);
    dup2(m_pipeWrite, STDERR_FILENO);
    fflush(stderr);

    m_stderrThread = std::thread(&RxPipeline::stderrReaderThread, this);
}

void RxPipeline::stopStderrCapture()
{
    // Restore stderr before joining so any final output still drains
    if (m_stderrSavedFd >= 0) {
        fflush(stderr);
        dup2(m_stderrSavedFd, STDERR_FILENO);
        close(m_stderrSavedFd);
        m_stderrSavedFd = -1;
    }
    // Close pipe write end — this makes the read end return EOF
    if (m_pipeWrite >= 0) { close(m_pipeWrite); m_pipeWrite = -1; }
    if (m_stderrThread.joinable()) m_stderrThread.join();
    if (m_pipeRead  >= 0) { close(m_pipeRead);  m_pipeRead  = -1; }
}

void RxPipeline::stderrReaderThread(RxPipeline* self)
{
    char buf[512];
    std::string line;

    while (true) {
        ssize_t n = read(self->m_pipeRead, buf, sizeof(buf) - 1);
        if (n <= 0) {
            if (n == 0) break;                           // EOF — pipe closed
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!self->m_running.load()) break;
                usleep(50'000);                          // 50 ms poll
                continue;
            }
            break;                                       // real error
        }
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
