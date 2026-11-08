#pragma once
#include "tx/ControlChannel.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <array>

// Forward-declare dsd-neo C types to avoid pulling all headers into App.hpp
struct dsd_opts;
struct dsd_state;

/**
 * RxPipeline — wraps the dsd-neo engine for P25P1 site-input reception.
 *
 * Configures the engine in ISP mode (opts->p25_isp_mode = 1) so that TSBKs
 * received on the RTL-SDR are decoded as mobile-originated messages rather
 * than site-originated.  Decoded events are forwarded to the log callback
 * and TSBK responses are queued onto the ControlChannel.
 */
class RxPipeline {
public:
    using LogCallback = std::function<void(const std::string&)>;

    /**
     * @param cc  ControlChannel to queue TSBK responses onto.
     * @param log Called (from any thread) with a human-readable RX event string.
     */
    explicit RxPipeline(ControlChannel& cc, LogCallback log);
    ~RxPipeline();

    RxPipeline(const RxPipeline&)            = delete;
    RxPipeline& operator=(const RxPipeline&) = delete;

    void configure(const SiteConfig& cfg);
    bool start();
    void stop();
    bool isRunning() const { return m_running.load(); }
    void setLogCallback(LogCallback log) { m_log = std::move(log); }

    // Forward an already-formatted line to the UI log (used by the dsd-neo
    // runtime log sink so engine messages reach the operator regardless of
    // whether this GUI process has a valid stdio stderr handle).
    void logLine(const std::string& s) { if (m_log) m_log(s); }

    // ── RX source selection ────────────────────────────────────────────────
    // Default is the native librtlsdr path (HackRF TX + RTL-SDR RX setup).
    void useRtlsdrSource();
    // Open the RX device through SoapySDR (e.g. an ADALM-Pluto used for RX
    // only, as in uplink_test).
    void useSoapySource(const std::string& args, const std::string& profile,
                        uint32_t sampleRateHz);
    // Share an already-open SoapySDR device owned by the transmitter, so one
    // full-duplex radio serves both TX and RX.
    void useSharedSoapyDevice(void* device, const std::string& args,
                              const std::string& profile, uint32_t sampleRateHz);

    bool isSoapy() const { return m_soapy; }
    std::string sourceName() const;

private:
    static void engineThread(RxPipeline* self);

    // ISP TSBK hook — called from the dsd-neo engine thread.
    static void onIspTsbk(uint8_t opcode, const uint8_t raw12[12], void* user);

    // dsd-neo event log hook — forwarded to m_log callback.
    static void onWriteEvent(const struct dsd_opts*, struct dsd_state*, uint8_t, uint8_t,
                             char* event_string);

    void handleIspTsbk(uint8_t opcode, const uint8_t raw12[12]);

    ControlChannel& m_cc;
    LogCallback     m_log;
    SiteConfig      m_cfg;

    bool         m_soapy          = false;
    bool         m_shareDevice    = false;
    void*        m_externalDevice = nullptr;
    std::string  m_soapyArgs;
    std::string  m_soapyProfile;
    uint32_t     m_soapyRate      = 0;

    dsd_opts* m_opts  = nullptr;
    dsd_state* m_state = nullptr;

    std::thread       m_thread;
    std::thread       m_stderrThread;
    std::atomic<bool> m_running{false};

    int m_stderrSavedFd = -1;  // original stderr fd saved across redirect
    int m_pipeRead      = -1;
    int m_pipeWrite     = -1;

    void startStderrCapture();
    void stopStderrCapture();
    static void stderrReaderThread(RxPipeline* self);

#if defined(_WIN32)
    // ── External decoder (dsd-neo v2.10.0 CLI) ─────────────────────────────
    // The vendored in-process dsd-neo (v2.3.0) mis-demodulates P25P1, so for
    // the RTL-SDR path SiteSim runs the known-good standalone decoder as a
    // child process and bridges its decoded TSBKs back into this pipeline.
    bool        m_useExternal = false;
    void*       m_hProc       = nullptr;   // HANDLE hProcess
    void*       m_hThread     = nullptr;   // HANDLE hThread
    void*       m_hRead       = nullptr;   // HANDLE stderr read end
    std::thread m_reader;
    std::atomic<bool> m_readerRunning{false};
    std::string m_lineBuf;
    bool        m_expectPdu = false;

    static bool externalDecoderAvailable();
    void  startExternalDecoder();
    void  stopExternalDecoder();
    static void decoderReaderThread(RxPipeline* self);
    void  feedDecoderBytes(const char* data, int n);
    void  emitDecodedTsbk(const uint8_t raw12[12]);
#endif
};
