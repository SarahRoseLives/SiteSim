#pragma once
#include "tx/SoapyTx.hpp"
#include "tx/ControlChannel.hpp"
#include "rx/RxPipeline.hpp"

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
    void shutdown();

    SDL_Window*   m_window = nullptr;
    SDL_GLContext m_glCtx  = nullptr;

    SoapyTx        m_tx;
    ControlChannel m_cc;
    RxPipeline     m_rx;

    std::vector<std::string> m_deviceList;
    std::string m_statusMsg;

    bool m_running        = true;
    bool m_dockLayoutDone = false;
};
