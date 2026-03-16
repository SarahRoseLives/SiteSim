#pragma once
#include "tx/ControlChannel.hpp"
#include "tx/SoapyTx.hpp"
#include "BandSelector.hpp"
#include <string>

class SitePanel {
public:
    SitePanel(ControlChannel& cc, SoapyTx& tx);
    void render();
    const SiteConfig& config() const { return m_cfg; }

private:
    void saveConfig();
    void loadConfig();

    ControlChannel& m_cc;
    SoapyTx&        m_tx;
    SiteConfig      m_cfg;

    char m_nacBuf[16]   = "293";
    char m_wacnBuf[16]  = "BEEF0";
    char m_sysidBuf[16] = "001";
    BandSelector m_bandSel;

    std::string m_statusMsg;
};
