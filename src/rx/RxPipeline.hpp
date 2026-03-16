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

private:
    static void engineThread(RxPipeline* self);

    // ISP TSBK hook — called from the dsd-neo engine thread.
    static void onIspTsbk(uint8_t opcode, const uint8_t raw12[12], void* user);

    // dsd-neo event log hook — forwarded to m_log callback.
    static void onWriteEvent(struct dsd_opts*, struct dsd_state*, uint8_t, uint8_t,
                             char* event_string);

    void handleIspTsbk(uint8_t opcode, const uint8_t raw12[12]);

    ControlChannel& m_cc;
    LogCallback     m_log;
    SiteConfig      m_cfg;

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
};
