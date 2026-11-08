#include "rx/RxPipeline.hpp"
#include "rx/P25Receiver.hpp"
#include "p25/TSBK.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <complex>
#include <cmath>
#include <vector>
#include <stdexcept>

#include <rtl-sdr.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>

#if SITESIM_HAVE_DSDNEO

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/rtl_demod_config.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/p25_isp_hooks.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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

// Dedicated file for engine diagnostics, opened by fopen (not freopen on the
// inherited stderr, which a windowed process may not have). dsd-neo's runtime
// log messages are mirrored here through the log-sink below so they are
// readable even when the GUI process has no console.
static FILE* g_rx_logfile = nullptr;

static const char* rx_logfile_path()
{
#if defined(_WIN32)
    return "C:\\Users\\Rose\\AppData\\Local\\Temp\\opencode\\sitesim_rx.log";
#else
    return "/tmp/sitesim_rx.log";
#endif
}

// dsd-neo log-sink callback: mirrored to the diagnostics file and the UI log.
static void rx_log_sink_write(dsd_neo_log_level_t /*level*/, const char* message, void* context)
{
    if (!message) return;
    if (g_rx_logfile) {
        fputs(message, g_rx_logfile);
        if (message[0] != '\0' && message[strlen(message) - 1] != '\n')
            fputc('\n', g_rx_logfile);
        fflush(g_rx_logfile);
    }
    auto* self = static_cast<RxPipeline*>(context);
    if (self) {
        std::string s(message);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) self->logLine(s);
    }
}

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
    // Native in-process P25 receiver fed from librtlsdr.
    m_useNative       = true;
    m_nativeRate      = 1536000;
    m_nativeOffsetHz  = 180000;
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
    m_useNative      = true;
    m_nativeRate     = sampleRateHz ? sampleRateHz : 1536000;
    m_nativeOffsetHz = 180000;
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
    m_useNative      = true;
    m_nativeRate     = sampleRateHz ? sampleRateHz : 1536000;
    m_nativeOffsetHz = 180000;
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

    if (m_log) {
        char m[192];
        snprintf(m, sizeof(m), "RX starting: %s @ %.4f MHz (%s)%s",
                 sourceName().c_str(), m_cfg.rxFreqHz() / 1e6,
                 m_soapy ? (m_shareDevice ? "shared Soapy" : "SoapySDR") : "RTL-SDR",
                 m_shareDevice ? " - shares device with TX (may be desensed by TX)" : "");
        m_log(m);
    }

    // Allocate and initialise dsd-neo state
    if (m_useNative) {
        startNative();
        return m_running.load();
    }
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
    // Use RTL AGC (value 0). The reference decoder synced and decoded cleanly
    // (NAC 123, valid TSBK) with AGC; a forced fixed gain left the front end
    // clipping ~40%, which produced corrupt frames (NAC 000 / duid EE). AGC
    // drives the R820T internal LNA/mixer below the manual minimum and yields
    // far less clipping in this strong near-field setup.
    m_opts->rtl_gain_value      = 0;     // 0 => RTL AGC
    m_opts->rtlsdr_ppm_error    = 0;     // no manual PPM correction
    // Run the demod at 48 kHz. The P25P1 decoder derives its symbol rate from
    // the (post-resampler) output rate and assumes ~48 kHz (SPS 10). At 24 kHz
    // the output lands at SPS 5 ("atypical SPS") and the decoder never syncs,
    // which is why the in-process RX produced no decodes. 48 kHz is also what
    // dsd-neo explicitly recommends for P25 ("try 48kHz").
    m_opts->rtl_dsp_bw_khz      = 48;

    // Build the full RTL input spec exactly as the working standalone CLI does
    // ("rtl:dev:freq:gain:ppm:bw:sql:vol"). The engine parses tuning from this
    // string; a bare "rtl" leaves the numeric tokens absent, so populate them
    // explicitly to guarantee identical device configuration.
    if (!m_soapy) {
        snprintf(m_opts->audio_in_dev, sizeof(m_opts->audio_in_dev),
                 "rtl:%d:%.4fM:%d:%d:%d:%d:%d",
                 m_opts->rtl_dev_index, m_cfg.rxFreqHz() / 1e6,
                 m_opts->rtl_gain_value, m_opts->rtlsdr_ppm_error,
                 m_opts->rtl_dsp_bw_khz, 0, 2);
    }

    // Route decoded audio to the null sink (equivalent to the standalone CLI's
    // "-o null"). Without a draining output sink the output ring can fill and
    // stall the demod pipeline, which stops Sync/TSBK decoding even though the
    // RF input is streaming. "null" is recognized as audio_out_type 9.
    snprintf(m_opts->audio_out_dev, sizeof(m_opts->audio_out_dev), "null");

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

    // Open our own diagnostics file and install a dsd-neo log sink so engine
    // messages are captured regardless of stdio validity in a GUI process.
    if (!g_rx_logfile) {
        g_rx_logfile = fopen(rx_logfile_path(), "w");
    }
    {
        dsd_neo_log_sink sink{};
        sink.write         = &rx_log_sink_write;
        sink.context       = this;
        sink.mirror_stderr = 0;
        dsd_neo_log_sink_set(&sink);
    }

    char startMsg[128];
    snprintf(startMsg, sizeof(startMsg), "RX started — %s @ %.4f MHz (P25P1 ISP mode)",
             sourceName().c_str(), m_cfg.rxFreqHz() / 1e6);
    m_log(startMsg);

    m_thread = std::thread(&RxPipeline::engineThread, this);
    return true;
}

void RxPipeline::stop()
{
    if (m_useNative) {
        stopNative();
        m_running.store(false);
        return;
    }
#if defined(_WIN32)
    if (m_useExternal) {
        stopExternalDecoder();
        m_running.store(false);
        return;
    }
#endif
    m_running.store(false);
    if (m_thread.joinable()) {
        exitflag = 1;      // signals dsd_engine_run() to exit its loop
        m_thread.join();
    }

    // RX is done with the borrowed device; the transmitter still owns it.
    rtl_device_set_external_soapy_device(nullptr);

    dsd_neo_log_sink_reset();

    stopStderrCapture();

    if (g_rx_logfile) {
        fflush(g_rx_logfile);
        fclose(g_rx_logfile);
        g_rx_logfile = nullptr;
    }

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
    if (self->m_log)
        self->m_log("RX engine thread started - opening RTL...");
    dsd_engine_run(self->m_opts, self->m_state);
    if (self->m_log)
        self->m_log("RX engine stopped - device open failed or stream ended (is the RTL held by another app like SDRTrunk?)");
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
        // Emit the grant many times so it is on the air continuously across the
        // subscriber's request/retry window. A single grant often missed the
        // radio's short listen window (~6 s bonk or no permit); a burst lets it
        // catch one immediately. The producer drains one queued TSBK per frame.
        for (int i = 0; i < 10; ++i)
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

// ── Native in-process P25 receiver (p25rx::P25Receiver) ──────────────────────

void RxPipeline::onNativeTsbk(const uint8_t tsbk[12])
{
    if (g_rx_logfile) {
        fprintf(g_rx_logfile,
                "TSBK op=0x%02X raw=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                tsbk[0] & 0x3F, tsbk[0], tsbk[1], tsbk[2], tsbk[3], tsbk[4], tsbk[5],
                tsbk[6], tsbk[7], tsbk[8], tsbk[9], tsbk[10], tsbk[11]);
        fflush(g_rx_logfile);
    }
    handleIspTsbk(uint8_t(tsbk[0] & 0x3F), tsbk);
}

void RxPipeline::startNative()
{
    if (m_running.load()) return;

    m_rx = new p25rx::P25Receiver();
    m_rx->configure(double(m_nativeRate), m_cfg.nac);
    m_rx->setMixOffset(double(m_nativeOffsetHz));
    m_rx->setSsInterval(35);
    m_rx->setTsbkCallback([this](const std::array<uint8_t, 12>& t) { onNativeTsbk(t.data()); });

    if (!g_rx_logfile)
        g_rx_logfile = fopen(rx_logfile_path(), "w");

    m_nativeStop.store(false);
    m_running.store(true);
    if (m_log) {
        char m[192];
        snprintf(m, sizeof(m), "RX started (native P25 receiver) %s @ %.4f MHz",
                 sourceName().c_str(), m_cfg.rxFreqHz() / 1e6);
        m_log(m);
    }
    m_nativeThread = std::thread(m_soapy ? &RxPipeline::nativeSoapyThread
                                         : &RxPipeline::nativeRtlThread, this);
}

void RxPipeline::stopNative()
{
    m_nativeStop.store(true);
    if (m_nativeThread.joinable()) m_nativeThread.join();
    delete m_rx;
    m_rx = nullptr;
    if (g_rx_logfile) { fflush(g_rx_logfile); fclose(g_rx_logfile); g_rx_logfile = nullptr; }
    m_running.store(false);
}

void RxPipeline::nativeRtlThread(RxPipeline* self)
{
    rtlsdr_dev_t* dev = nullptr;
    if (rtlsdr_open(&dev, 0) < 0) {
        if (self->m_log) self->m_log("RX: rtlsdr_open failed (device busy?)");
        self->m_running.store(false);
        return;
    }
    rtlsdr_set_sample_rate(dev, uint32_t(self->m_nativeRate));
    long center = long(self->m_cfg.rxFreqHz()) + self->m_nativeOffsetHz;
    if (center < 0) center = 0;
    rtlsdr_set_center_freq(dev, uint32_t(center));
    rtlsdr_set_tuner_gain_mode(dev, 1);   // AGC
    rtlsdr_reset_buffer(dev);

    std::vector<uint8_t> buf(32768);
    std::vector<float> f;
    while (!self->m_nativeStop.load() && self->m_running.load()) {
        int n = 0;
        if (rtlsdr_read_sync(dev, buf.data(), int(buf.size()), &n) != 0 || n <= 0) break;
        int ns = n / 2;
        f.resize(size_t(ns) * 2);
        for (int i = 0; i < ns; ++i) {
            f[2 * i]     = (float(buf[2 * i])     - 127.5f) / 127.5f;
            f[2 * i + 1] = (float(buf[2 * i + 1]) - 127.5f) / 127.5f;
        }
        self->m_rx->process(f.data(), size_t(ns));
    }
    rtlsdr_close(dev);
}

void RxPipeline::nativeSoapyThread(RxPipeline* self)
{
    auto diag = [&](const std::string& s) {
        if (g_rx_logfile) { fprintf(g_rx_logfile, "DIAG %s\n", s.c_str()); fflush(g_rx_logfile); }
        if (self->m_log) self->m_log(std::string("RX: ") + s);
    };
    SoapySDR::Device* dev = static_cast<SoapySDR::Device*>(self->m_externalDevice);
    bool own = false;
    if (!dev && !self->m_soapyArgs.empty()) {
        try { dev = SoapySDR::Device::make(self->m_soapyArgs); own = true; }
        catch (...) { dev = nullptr; }
    }
    if (!dev) {
        diag("Soapy RX device unavailable");
        self->m_running.store(false);
        return;
    }
    SoapySDR::Stream* stream = nullptr;
    try {
        dev->setSampleRate(SOAPY_SDR_RX, 0, double(self->m_nativeRate));
        long center = long(self->m_cfg.rxFreqHz()) + self->m_nativeOffsetHz;
        dev->setFrequency(SOAPY_SDR_RX, 0, double(center));
        try { dev->setGainMode(SOAPY_SDR_RX, 0, true); } catch (...) {}
        stream = dev->setupStream(SOAPY_SDR_RX, "CF32");
        if (!stream) throw std::runtime_error("setupStream failed");
        dev->activateStream(stream);
        char rb[160];
        double actRate = double(self->m_nativeRate);
        try { actRate = dev->getSampleRate(SOAPY_SDR_RX, 0); } catch (...) {}
        snprintf(rb, sizeof(rb), "Soapy RX active req=%.0f actual=%.0f center=%.4f MHz off=%ld",
                 double(self->m_nativeRate), actRate, double(center) / 1e6, self->m_nativeOffsetHz);
        diag(rb);
    } catch (const std::exception& e) {
        diag(std::string("Soapy RX setup failed: ") + e.what());
        if (stream) dev->closeStream(stream);
        if (own) SoapySDR::Device::unmake(dev);
        self->m_running.store(false);
        return;
    }

    // Optional raw IQ dump for offline analysis: set SITESIM_RX_RECORD=<path>.
    FILE* rec = nullptr;
    if (const char* recp = std::getenv("SITESIM_RX_RECORD"))
        rec = fopen(recp, "wb");
    if (rec) diag(std::string("Soapy RX recording to ") + std::getenv("SITESIM_RX_RECORD"));

    std::vector<std::complex<float>> buff(16384);
    std::vector<float> f;
    double rmsAcc = 0; uint64_t rmsN = 0; int chunks = 0;
    while (!self->m_nativeStop.load() && self->m_running.load()) {
        void* bufs[1] = { buff.data() };
        int flags = 0; long long ts = 0;
        int n = dev->readStream(stream, bufs, buff.size(), flags, ts, 200000);
        if (n > 0) {
            f.resize(size_t(n) * 2);
            for (int i = 0; i < n; ++i) {
                f[2 * i]     = buff[i].real();
                f[2 * i + 1] = buff[i].imag();
                rmsAcc += double(buff[i].real()) * buff[i].real() +
                          double(buff[i].imag()) * buff[i].imag();
                ++rmsN;
            }
            self->m_rx->process(f.data(), size_t(n));
            if (rec) { fwrite(f.data(), sizeof(float), f.size(), rec); fflush(rec); }
            if (++chunks >= 200 && rmsN) {
                char b[160];
                const auto& st = self->m_rx->stats();
                snprintf(b, sizeof(b),
                         "rx rms=%.4f syms=%llu fsw=%llu nid=%llu tsbk=%llu fswD=%d invD=%d smin=%.1f smax=%.1f",
                         std::sqrt(rmsAcc / double(rmsN)),
                         (unsigned long long)st.symbols,
                         (unsigned long long)st.fswHits,
                         (unsigned long long)st.nidOk,
                         (unsigned long long)st.tsbks,
                         st.minFswDist, st.minFswDistInv, st.symMin, st.symMax);
                diag(b);
                chunks = 0; rmsAcc = 0; rmsN = 0;
            }
        }
    }
    if (rec) fclose(rec);
    dev->deactivateStream(stream);
    dev->closeStream(stream);
    if (own) SoapySDR::Device::unmake(dev);
}

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
    // Discard dsd-neo's console output to the null device. A pipe drained by a
    // reader thread can stall the in-process engine (verbose output fills the
    // 4 KB pipe faster than the reader drains it, blocking the engine on stderr
    // writes and stopping all decoding). Decoded ISP events still reach the RX
    // log through the p25_isp / optional hooks.
    fflush(stderr);
    // freopen is reliable even when a GUI process starts with an invalid
    // stderr handle (dup2 onto fd 2 can silently fail there, losing all output).
    // NOTE: this targets a separate file from the log-sink diagnostics file so
    // the two writers never share a path/offset.
#if defined(_WIN32)
    (void)::freopen("C:\\Users\\Rose\\AppData\\Local\\Temp\\opencode\\sitesim_stderr.log", "w", stderr);
#else
    (void)::freopen("/tmp/sitesim_stderr.log", "w", stderr);
#endif
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

#if defined(_WIN32)
// ── External decoder bridge (dsd-neo v2.10.0) ─────────────────────────────────

static const char* kExtDecoderExe =
    "C:\\Users\\Rose\\Documents\\dsd-neo-msvc-x86_64-native-v2.10.0\\bin\\dsd-neo.exe";
static const char* kExtDecoderDir =
    "C:\\Users\\Rose\\Documents\\dsd-neo-msvc-x86_64-native-v2.10.0\\bin";

bool RxPipeline::externalDecoderAvailable()
{
    return GetFileAttributesA(kExtDecoderExe) != INVALID_FILE_ATTRIBUTES;
}

void RxPipeline::startExternalDecoder()
{
    if (m_running.load()) return;

    if (!g_rx_logfile)
        g_rx_logfile = fopen(rx_logfile_path(), "w");

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -i rtl:0:%.4fM:0:0:48:0:2 -o null --frontend none -Z",
             kExtDecoderExe, m_cfg.rxFreqHz() / 1e6);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        if (m_log) m_log("RX: failed to create pipe for external decoder");
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput  = wr;
    si.hStdError   = wr;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, kExtDecoderDir, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        if (m_log) m_log("RX: failed to start external dsd-neo decoder");
        return;
    }
    CloseHandle(wr);  // parent keeps only the read end

    m_hProc   = (void*)pi.hProcess;
    m_hThread = (void*)pi.hThread;
    m_hRead   = (void*)rd;
    m_lineBuf.clear();
    m_expectPdu = false;
    m_readerRunning.store(true);
    m_running.store(true);

    if (m_log) {
        char m[256];
        snprintf(m, sizeof(m), "RX started (external dsd-neo v2.10.0) @ %.4f MHz",
                 m_cfg.rxFreqHz() / 1e6);
        m_log(m);
    }
    m_reader = std::thread(decoderReaderThread, this);
}

void RxPipeline::stopExternalDecoder()
{
    m_readerRunning.store(false);
    if (m_hProc)
        TerminateProcess((HANDLE)m_hProc, 0);
    if (m_reader.joinable())
        m_reader.join();
    if (m_hRead)   { CloseHandle((HANDLE)m_hRead);   m_hRead = nullptr; }
    if (m_hThread) { CloseHandle((HANDLE)m_hThread); m_hThread = nullptr; }
    if (m_hProc)   { CloseHandle((HANDLE)m_hProc);   m_hProc = nullptr; }
    m_lineBuf.clear();
    m_expectPdu = false;
    if (g_rx_logfile) { fflush(g_rx_logfile); fclose(g_rx_logfile); g_rx_logfile = nullptr; }
    m_running.store(false);
}

void RxPipeline::decoderReaderThread(RxPipeline* self)
{
    char buf[1024];
    while (self->m_readerRunning.load()) {
        DWORD n = 0;
        if (!ReadFile((HANDLE)self->m_hRead, buf, sizeof(buf), &n, nullptr) || n == 0)
            break;
        self->feedDecoderBytes(buf, (int)n);
    }
}

void RxPipeline::feedDecoderBytes(const char* data, int n)
{
    auto hexv = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    };

    for (int i = 0; i < n; ++i) {
        char c = data[i];
        if (c == '\r') continue;
        if (c != '\n') {
            if (m_lineBuf.size() < 8192) m_lineBuf.push_back(c);
            continue;
        }

        // Complete line: strip ANSI SGR sequences (ESC ... 'm').
        std::string clean;
        clean.reserve(m_lineBuf.size());
        for (size_t j = 0; j < m_lineBuf.size(); ++j) {
            if (m_lineBuf[j] == 0x1b) {
                ++j;
                while (j < m_lineBuf.size() && m_lineBuf[j] != 'm') ++j;
                continue;
            }
            clean.push_back(m_lineBuf[j]);
        }
        m_lineBuf.clear();

        if (g_rx_logfile &&
            (clean.find("Sync:") != std::string::npos || clean.find("NAC") != std::string::npos ||
             clean.find("RF Level") != std::string::npos || clean.find("RTL") != std::string::npos ||
             clean.find("Starting RTL") != std::string::npos || clean.find("Found") != std::string::npos)) {
            fprintf(g_rx_logfile, "DBG: %s\n", clean.c_str());
            fflush(g_rx_logfile);
        }

        if (clean.find("PDU Payload") != std::string::npos) {
            m_expectPdu = true;
            continue;
        }
        if (!m_expectPdu) continue;

        uint8_t bytes[64];
        int cnt = 0;
        for (size_t j = 0; j + 3 < clean.size() && cnt < 64;) {
            if (clean[j] == '[' && clean[j + 3] == ']') {
                int a = hexv(clean[j + 1]);
                int b = hexv(clean[j + 2]);
                if (a >= 0 && b >= 0) {
                    bytes[cnt++] = (uint8_t)((a << 4) | b);
                    j += 4;
                    continue;
                }
            }
            ++j;
        }
        if (cnt >= 12) {
            uint8_t raw12[12];
            memcpy(raw12, bytes, sizeof(raw12));
            m_expectPdu = false;
            emitDecodedTsbk(raw12);
        }
        // else: not enough bytes yet (e.g. continuation line) - keep waiting
    }
}

void RxPipeline::emitDecodedTsbk(const uint8_t raw12[12])
{
    uint8_t opcode = raw12[1] & 0x3F;
    if (g_rx_logfile) {
        fprintf(g_rx_logfile,
                "TSBK opcode=0x%02X raw=[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]\n",
                opcode, raw12[0], raw12[1], raw12[2], raw12[3], raw12[4], raw12[5],
                raw12[6], raw12[7], raw12[8], raw12[9], raw12[10], raw12[11]);
        fflush(g_rx_logfile);
    }
    handleIspTsbk(opcode, raw12);
}
#endif // _WIN32

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

