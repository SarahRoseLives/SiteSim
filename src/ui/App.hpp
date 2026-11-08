#pragma once
#include "tx/SoapyTx.hpp"
#include "tx/ControlChannel.hpp"
#include "rx/RxPipeline.hpp"

#include <atomic>
#include <thread>

struct SDL_Window;
typedef void* SDL_GLContext;

class App {
public:
    App();
    ~App();
    bool init();
    void run();

private:
    void setupDocking();
    void renderMenuBar();
    void renderStatusBar();
    void renderPanels();
    void renderLoadingSplash();
    void applyRadioConfig();
    void startDeviceScan();
    void finishDeviceScan();
    void shutdown();

    SDL_Window*   m_window = nullptr;
    SDL_GLContext m_glCtx  = nullptr;

    SoapyTx        m_tx;
    ControlChannel m_cc;
    RxPipeline     m_rx;

    std::vector<std::string> m_deviceList;
    std::string m_statusMsg;

    // Device enumeration/open happens on a worker so the window is not a black
    // screen while SoapySDR probes USB + network SDRs (can take tens of seconds).
    std::thread             m_scanThread;
    std::atomic<bool>       m_scanDone{false};
    bool                    m_scanActive = false;
    std::vector<std::string> m_pendingDeviceList;
    int                     m_pendingPick    = -1;
    bool                    m_pendingOpenOk  = false;
    std::string             m_pendingError;

    bool m_running        = true;
    bool m_dockLayoutDone = false;
};
