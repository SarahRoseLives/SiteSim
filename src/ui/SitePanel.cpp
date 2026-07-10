#include "SitePanel.hpp"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;
using json   = nlohmann::json;

static fs::path configPath() {
    const char* home = std::getenv("HOME");
    fs::path dir = home ? fs::path(home) / ".config" / "sitesim" : fs::path(".");
    fs::create_directories(dir);
    return dir / "config.json";
}

SitePanel::SitePanel(ControlChannel& cc, SoapyTx& tx) : m_cc(cc), m_tx(tx) {
    loadConfig();
    snprintf(m_nacBuf,   sizeof(m_nacBuf),  "%X", m_cfg.nac);
    snprintf(m_wacnBuf,  sizeof(m_wacnBuf), "%X", m_cfg.wacn);
    snprintf(m_sysidBuf, sizeof(m_sysidBuf),"%X", m_cfg.sysid);
}

void SitePanel::saveConfig() {
    try {
        json j;
        j["nac"]             = m_cfg.nac;
        j["wacn"]            = m_cfg.wacn;
        j["sysid"]           = m_cfg.sysid;
        j["rfssid"]          = m_cfg.rfssid;
        j["siteid"]          = m_cfg.siteid;
        j["ccFreqHz"]        = m_cfg.ccFreqHz;
        j["txOffsetMHz"]     = m_cfg.txOffsetMHz;
        j["gainDb"]          = m_cfg.gainDb;
        j["ampEnabled"]      = m_cfg.ampEnabled;
        j["chanId"]          = m_cfg.chanId;
        j["bwUnits"]         = m_cfg.bwUnits;
        j["chSpacUnits"]     = m_cfg.chSpacUnits;
        j["vchanBaseFreqHz"] = m_cfg.vchanBaseFreqHz;
        j["vChanNum"]        = m_cfg.vChanNum;
        std::ofstream f(configPath());
        f << j.dump(2);
        m_statusMsg = "Config saved.";
    } catch (...) {
        m_statusMsg = "Save failed.";
    }
}

void SitePanel::loadConfig() {
    try {
        std::ifstream f(configPath());
        if (!f.is_open()) return;
        json j = json::parse(f);
        m_cfg.nac             = j.value("nac",             m_cfg.nac);
        m_cfg.wacn            = j.value("wacn",            m_cfg.wacn);
        m_cfg.sysid           = j.value("sysid",           m_cfg.sysid);
        m_cfg.rfssid          = j.value("rfssid",          m_cfg.rfssid);
        m_cfg.siteid          = j.value("siteid",          m_cfg.siteid);
        m_cfg.ccFreqHz        = j.value("ccFreqHz",        m_cfg.ccFreqHz);
        m_cfg.txOffsetMHz     = j.value("txOffsetMHz",     m_cfg.txOffsetMHz);
        m_cfg.gainDb          = j.value("gainDb",          m_cfg.gainDb);
        m_cfg.ampEnabled      = j.value("ampEnabled",      m_cfg.ampEnabled);
        m_cfg.chanId          = j.value("chanId",          m_cfg.chanId);
        m_cfg.bwUnits         = j.value("bwUnits",         m_cfg.bwUnits);
        m_cfg.chSpacUnits     = j.value("chSpacUnits",     m_cfg.chSpacUnits);
        m_cfg.vchanBaseFreqHz = j.value("vchanBaseFreqHz", m_cfg.vchanBaseFreqHz);
        m_cfg.vChanNum        = j.value("vChanNum",        m_cfg.vChanNum);
    } catch (...) {
        // Use defaults on any parse error
    }
}

void SitePanel::render() {
    ImGui::Begin("Site Config");

    // ── System Identity ───────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "System Identity");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("NAC (hex, 12-bit)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##nac", m_nacBuf, sizeof(m_nacBuf));

    ImGui::Text("WACN (hex, 20-bit)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##wacn", m_wacnBuf, sizeof(m_wacnBuf));

    ImGui::Text("SYSID (hex, 12-bit)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##sysid", m_sysidBuf, sizeof(m_sysidBuf));

    ImGui::Text("RFSS ID");
    ImGui::SetNextItemWidth(-1);
    int rfssid = m_cfg.rfssid;
    if (ImGui::InputInt("##rfssid", &rfssid))
        m_cfg.rfssid = uint8_t(std::min(255, std::max(0, rfssid)));

    ImGui::Text("Site ID");
    ImGui::SetNextItemWidth(-1);
    int siteid = m_cfg.siteid;
    if (ImGui::InputInt("##siteid", &siteid))
        m_cfg.siteid = uint8_t(std::min(255, std::max(0, siteid)));

    // ── CC Frequency + TX/RX offset ───────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "CC Frequency");
    ImGui::Separator();
    ImGui::Spacing();

    m_bandSel.render(m_cfg.ccFreqHz);

    // TX offset (repeater input/output split; 0 = simplex)
    ImGui::Text("TX Offset (MHz, 0=simplex)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputDouble("##txoffset", &m_cfg.txOffsetMHz, 0.0, 0.0, "%.4f");

    // Common amateur presets
    ImGui::TextDisabled("Presets:");
    ImGui::SameLine();
    if (ImGui::SmallButton("Simplex"))  m_cfg.txOffsetMHz =  0.0;
    ImGui::SameLine();
    if (ImGui::SmallButton("+600kHz"))  m_cfg.txOffsetMHz =  0.6;
    ImGui::SameLine();
    if (ImGui::SmallButton("-600kHz"))  m_cfg.txOffsetMHz = -0.6;
    ImGui::SameLine();
    if (ImGui::SmallButton("+5MHz"))    m_cfg.txOffsetMHz =  5.0;
    ImGui::SameLine();
    if (ImGui::SmallButton("-5MHz"))    m_cfg.txOffsetMHz = -5.0;

    ImGui::Spacing();

    // Site input channel — this is where radios transmit requests back;
    // also where the RTL-SDR must be tuned to receive affiliations/grants
    double rxFreq = m_cfg.rxFreqHz();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Site Input Channel");
    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.55f, 1.0f),
                       "  %.6f MHz", rxFreq / 1e6);
    ImGui::TextDisabled("  Radios TX here  |  RTL-SDR tunes here");
    ImGui::TextDisabled("  (affiliations, registrations, voice requests)");

    ImGui::Text("TX Gain (dB)");
    float gain = float(m_cfg.gainDb);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##gain", &gain, 0.0f, 47.0f, "%.0f dB"))
        m_cfg.gainDb = double(gain);

    ImGui::Checkbox("RF Amplifier", &m_cfg.ampEnabled);

    // ── Channel Plan (IDEN_UP) ────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Channel Plan (IDEN_UP)");
    ImGui::Separator();
    ImGui::Spacing();

    // Chan ID
    ImGui::Text("Channel ID (0-15)");
    ImGui::SetNextItemWidth(-1);
    int chanId = m_cfg.chanId;
    if (ImGui::InputInt("##chanid", &chanId))
        m_cfg.chanId = uint8_t(std::min(15, std::max(0, chanId)));

    // Voice channel base frequency
    ImGui::Text("Base Freq (MHz)");
    double baseFreqMHz = m_cfg.vchanBaseFreqHz / 1e6;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputDouble("##basefreq", &baseFreqMHz, 0.0, 0.0, "%.6f"))
        m_cfg.vchanBaseFreqHz = baseFreqMHz * 1e6;

    // Channel spacing units (125 Hz/unit; 100 = 12.5 kHz)
    ImGui::Text("Ch Spacing (125 Hz units, 100=12.5kHz)");
    ImGui::SetNextItemWidth(-1);
    int chSpac = m_cfg.chSpacUnits;
    if (ImGui::InputInt("##chspac", &chSpac))
        m_cfg.chSpacUnits = uint16_t(std::min(1023, std::max(1, chSpac)));

    // TX offset units (250 kHz/unit) — derived from TX Offset MHz above
    ImGui::TextDisabled("TX Offset units (250kHz/unit): %d  =>  %.3f MHz",
                        int(m_cfg.txOffsetUnits),
                        m_cfg.txOffsetUnits * 0.25);

    // Voice channel number
    ImGui::Text("Voice Channel #");
    ImGui::SetNextItemWidth(-1);
    int vchan = m_cfg.vChanNum;
    if (ImGui::InputInt("##vchan", &vchan))
        m_cfg.vChanNum = uint16_t(std::max(0, vchan));

    // Helper: show resolved voice channel frequency
    double vcFreq = m_cfg.vchanBaseFreqHz + m_cfg.vChanNum * (m_cfg.chSpacUnits * 125.0);
    ImGui::TextDisabled("Voice ch %d => %.4f MHz", m_cfg.vChanNum, vcFreq / 1e6);

    // ── Apply / Save ──────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Apply", ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0))) {
        try {
            m_cfg.nac   = uint16_t(std::stoul(m_nacBuf,  nullptr, 16)) & 0xFFF;
            m_cfg.wacn  = uint32_t(std::stoul(m_wacnBuf, nullptr, 16)) & 0xFFFFF;
            m_cfg.sysid = uint16_t(std::stoul(m_sysidBuf,nullptr, 16)) & 0xFFF;
        } catch (...) {}

        bool wasRunning = m_cc.isRunning();
        if (wasRunning) m_cc.stop();

        m_tx.setFrequency(m_cfg.ccFreqHz);
        m_tx.setGain(m_cfg.gainDb);
        m_tx.setAmpEnabled(m_cfg.ampEnabled);
        m_cc.configure(m_cfg);

        if (wasRunning) m_cc.start();
        m_statusMsg = "Applied.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(-1, 0)))
        saveConfig();

    if (!m_statusMsg.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "%s", m_statusMsg.c_str());
    }

    ImGui::End();
}

