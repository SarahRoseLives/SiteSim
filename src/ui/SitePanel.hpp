#pragma once
#include "tx/ControlChannel.hpp"
#include "tx/SoapyTx.hpp"
#include "rx/RxPipeline.hpp"
#include <string>

class SitePanel {
public:
    SitePanel(ControlChannel& cc, SoapyTx& tx, RxPipeline& rx);
    void render();
    const SiteConfig& config() const { return m_cfg; }

private:
    void saveConfig();
    void loadConfig();
    void syncIdentityBuffers();
    void renderChannelPlan();
    void applyConfig();
    void addChannel();
    void removeChannel(int index);

    ControlChannel& m_cc;
    SoapyTx&        m_tx;
    RxPipeline&     m_rx;
    SiteConfig      m_cfg;

    char m_nacBuf[16]   = "293";
    char m_wacnBuf[16]  = "BEEF0";
    char m_sysidBuf[16] = "123";

    std::string m_statusMsg;
    bool        m_statusIsError = false;
    std::string m_primedDevice;   // device whose LO was initialized from the plan
};
