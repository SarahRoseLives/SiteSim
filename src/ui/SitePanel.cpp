#include "SitePanel.hpp"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

SitePanel::SitePanel(ControlChannel& cc, SoapyTx& tx) : m_cc(cc), m_tx(tx) {
    // Initialise input buffers from default config
    snprintf(m_nacBuf,   sizeof(m_nacBuf),  "%X", m_cfg.nac);
    snprintf(m_wacnBuf,  sizeof(m_wacnBuf), "%X", m_cfg.wacn);
    snprintf(m_sysidBuf, sizeof(m_sysidBuf),"%X", m_cfg.sysid);
}

void SitePanel::render() {
    ImGui::Begin("Site Config");

    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "System Identity");
    ImGui::Separator();
    ImGui::Spacing();

    // NAC
    ImGui::Text("NAC (hex, 12-bit)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##nac", m_nacBuf, sizeof(m_nacBuf));

    // WACN
    ImGui::Text("WACN (hex, 20-bit)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##wacn", m_wacnBuf, sizeof(m_wacnBuf));

    // SYSID
    ImGui::Text("SYSID (hex, 12-bit)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##sysid", m_sysidBuf, sizeof(m_sysidBuf));

    // RFSS ID
    ImGui::Text("RFSS ID");
    ImGui::SetNextItemWidth(-1);
    int rfssid = m_cfg.rfssid;
    if (ImGui::InputInt("##rfssid", &rfssid)) {
        if (rfssid < 0)   rfssid = 0;
        if (rfssid > 255) rfssid = 255;
        m_cfg.rfssid = uint8_t(rfssid);
    }

    // Site ID
    ImGui::Text("Site ID");
    ImGui::SetNextItemWidth(-1);
    int siteid = m_cfg.siteid;
    if (ImGui::InputInt("##siteid", &siteid)) {
        if (siteid < 0)   siteid = 0;
        if (siteid > 255) siteid = 255;
        m_cfg.siteid = uint8_t(siteid);
    }

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "RF Parameters");
    ImGui::Separator();
    ImGui::Spacing();

    // CC Freq — band-locked to 2m / 70cm amateur bands only
    m_bandSel.render(m_cfg.ccFreqHz);

    // Gain
    ImGui::Text("TX Gain (dB)");
    float gain = float(m_cfg.gainDb);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##gain", &gain, 0.0f, 47.0f, "%.0f dB"))
        m_cfg.gainDb = double(gain);

    // Amp
    ImGui::Checkbox("RF Amplifier", &m_cfg.ampEnabled);

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Simulation");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Activity Simulation", &m_cfg.simEnabled);

    ImGui::Text("Voice Channel #");
    ImGui::SetNextItemWidth(-1);
    int vchan = m_cfg.vChanNum;
    if (ImGui::InputInt("##vchan", &vchan)) {
        if (vchan < 0) vchan = 0;
        m_cfg.vChanNum = uint16_t(vchan);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Apply button
    if (ImGui::Button("Apply", ImVec2(-1, 0))) {
        // Parse hex fields
        try {
            m_cfg.nac   = uint16_t(std::stoul(m_nacBuf,  nullptr, 16)) & 0xFFF;
            m_cfg.wacn  = uint32_t(std::stoul(m_wacnBuf, nullptr, 16)) & 0xFFFFF;
            m_cfg.sysid = uint16_t(std::stoul(m_sysidBuf,nullptr, 16)) & 0xFFF;
        } catch (...) {
            // Keep existing values on parse error
        }

        bool wasRunning = m_cc.isRunning();
        if (wasRunning) m_cc.stop();

        m_tx.setFrequency(m_cfg.ccFreqHz);
        m_tx.setGain(m_cfg.gainDb);
        m_tx.setAmpEnabled(m_cfg.ampEnabled);

        m_cc.configure(m_cfg);

        if (wasRunning) m_cc.start();
    }

    ImGui::End();
}
