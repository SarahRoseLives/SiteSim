#include "rx/RxPipeline.hpp"

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
    m_opts->rtlsdr_center_freq  = static_cast<uint32_t>(m_cfg.rxFreqHz());
    m_opts->rtl_gain_value      = 300;   // 30.0 dB (tenths of dB)
    m_opts->rtl_dsp_bw_khz      = 12;
    m_opts->rtl_dev_index       = 0;

    // ── P25 Phase 1 mode ──────────────────────────────────────────────────
    m_opts->frame_p25p1         = 1;
    m_opts->p25_trunk           = 0;     // site input: no trunking follow
    m_opts->p25_isp_mode        = 1;     // interpret TSBKs as ISP (mobile→site)

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
        std::array<uint8_t, 12> rsp{};
        // GRP_AFF_RSP (OSP 0x28): [0]=0x28, [1]=0x00(MFID), [2]=options,
        //   [3]=0, [4]=0, [5-6]=TGID, [7-9]=SRC, [10-11]=CRC (filled by TSBK layer)
        rsp[0] = 0x28;
        rsp[2] = 0x00;                                   // announce group
        rsp[5] = (tgid >> 8) & 0xFF;
        rsp[6] =  tgid       & 0xFF;
        rsp[7] = (src_id >> 16) & 0xFF;
        rsp[8] = (src_id >>  8) & 0xFF;
        rsp[9] =  src_id        & 0xFF;
        m_cc.queueTSBK(rsp);

        snprintf(buf, sizeof(buf), "GRP_AFF_REQ  SRC:%-8u TGID:%-5u -> queued AFF_RSP", src_id, tgid);
        m_log(buf);
        break;
    }

    case P25_ISP_U_REG_REQ: {
        // Queue Unit Registration Response
        std::array<uint8_t, 12> rsp{};
        // U_REG_RSP (OSP 0x2C): [0]=0x2C, [1]=0x00, [2]=0(result), [3-4]=SysID,
        //   [7-9]=Source ID
        rsp[0] = 0x2C;
        rsp[2] = 0x00;                                   // registration accepted
        rsp[3] = (m_cfg.sysid >> 4) & 0xFF;
        rsp[4] = (m_cfg.sysid & 0x0F) << 4;
        rsp[7] = (src_id >> 16) & 0xFF;
        rsp[8] = (src_id >>  8) & 0xFF;
        rsp[9] =  src_id        & 0xFF;
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
