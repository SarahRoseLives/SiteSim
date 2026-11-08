#include "SitePanel.hpp"
#include "BandSelector.hpp"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
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

SitePanel::SitePanel(ControlChannel& cc, SoapyTx& tx, RxPipeline& rx)
    : m_cc(cc), m_tx(tx), m_rx(rx) {
    loadConfig();
    if (m_cfg.channels.empty())
        m_cfg.resetDefaultChannelPlan();
    m_cfg.normalizeChannelPlan();
    syncIdentityBuffers();
}

void SitePanel::syncIdentityBuffers() {
    snprintf(m_nacBuf,   sizeof(m_nacBuf),   "%X", m_cfg.nac);
    snprintf(m_wacnBuf,  sizeof(m_wacnBuf),  "%X", m_cfg.wacn);
    snprintf(m_sysidBuf, sizeof(m_sysidBuf), "%X", m_cfg.sysid);
}

void SitePanel::saveConfig() {
    try {
        json j;
        j["configVersion"]   = 2;
        j["nac"]             = m_cfg.nac;
        j["wacn"]            = m_cfg.wacn;
        j["sysid"]           = m_cfg.sysid;
        j["rfssid"]          = m_cfg.rfssid;
        j["siteid"]          = m_cfg.siteid;
        j["txOffsetMHz"]     = m_cfg.txOffsetMHz;
        j["txDeviationPeakHz"] = m_cfg.txDeviationPeakHz;
        j["txFreqTrimHz"]    = m_cfg.txFreqTrimHz;
        j["gainDb"]          = m_cfg.gainDb;
        j["plutoGainDb"]     = m_cfg.plutoGainDb;
        j["ampEnabled"]      = m_cfg.ampEnabled;
        j["minimalCc"]       = m_cfg.minimalCc;
        j["rxUseRtlsdr"]     = m_cfg.rxUseRtlsdr;
        j["chanId"]          = m_cfg.chanId;
        j["bwUnits"]         = m_cfg.bwUnits;
        j["chSpacUnits"]     = m_cfg.chSpacUnits;
        j["controlIndex"]    = m_cfg.controlIndex;
        j["voiceIndex"]      = m_cfg.voiceIndex;

        j["channels"] = json::array();
        for (const auto& c : m_cfg.channels)
            j["channels"].push_back({ {"name", c.name}, {"freqHz", c.freqHz} });

        // Keep the derived values for older builds/tools that read them.
        j["ccFreqHz"]        = m_cfg.ccFreqHz;
        j["vchanBaseFreqHz"] = m_cfg.vchanBaseFreqHz;
        j["vChanNum"]        = m_cfg.vChanNum;

        std::ofstream f(configPath());
        f << j.dump(2);
        m_statusMsg     = "Config saved.";
        m_statusIsError = false;
    } catch (...) {
        m_statusMsg     = "Save failed.";
        m_statusIsError = true;
    }
}

void SitePanel::loadConfig() {
    try {
        std::ifstream f(configPath());
        if (!f.is_open()) return;
        json j = json::parse(f);

        const int version = j.value("configVersion", 1);

        m_cfg.nac             = j.value("nac",             m_cfg.nac);
        m_cfg.wacn            = j.value("wacn",            m_cfg.wacn);
        m_cfg.sysid           = j.value("sysid",           m_cfg.sysid);
        m_cfg.rfssid          = j.value("rfssid",          m_cfg.rfssid);
        m_cfg.siteid          = j.value("siteid",          m_cfg.siteid);
        m_cfg.txOffsetMHz     = j.value("txOffsetMHz",     m_cfg.txOffsetMHz);
        m_cfg.txDeviationPeakHz = j.value("txDeviationPeakHz", m_cfg.txDeviationPeakHz);
        m_cfg.txFreqTrimHz    = j.value("txFreqTrimHz",    m_cfg.txFreqTrimHz);
        m_cfg.gainDb          = j.value("gainDb",          m_cfg.gainDb);
        m_cfg.plutoGainDb     = j.value("plutoGainDb",     m_cfg.plutoGainDb);
        m_cfg.ampEnabled      = j.value("ampEnabled",      m_cfg.ampEnabled);
        m_cfg.minimalCc       = j.value("minimalCc",       m_cfg.minimalCc);
        m_cfg.rxUseRtlsdr     = j.value("rxUseRtlsdr",     m_cfg.rxUseRtlsdr);
        m_cfg.chanId          = j.value("chanId",          m_cfg.chanId);
        m_cfg.bwUnits         = j.value("bwUnits",         m_cfg.bwUnits);
        m_cfg.chSpacUnits     = j.value("chSpacUnits",     m_cfg.chSpacUnits);

        // Legacy configs (v1) can be migrated without losing their old CC.
        double legacyCcFreqHz   = j.value("ccFreqHz", m_cfg.ccFreqHz);
        double legacyVchanBase  = j.value("vchanBaseFreqHz", m_cfg.vchanBaseFreqHz);
        uint16_t legacyVchanNum = j.value("vChanNum", m_cfg.vChanNum);

        if (version < 2 && m_cfg.sysid == 0x001)
            m_cfg.sysid = 0x123;

        if (j.contains("channels") && j["channels"].is_array() && !j["channels"].empty()) {
            m_cfg.channels.clear();
            for (const auto& item : j["channels"]) {
                SiteConfig::Channel c{};
                std::string name = item.value("name", std::string("CH"));
                snprintf(c.name, sizeof(c.name), "%s", name.c_str());
                c.freqHz = item.value("freqHz", 0.0);
                if (c.freqHz > 0.0)
                    m_cfg.channels.push_back(c);
            }
        } else if (version < 2) {
            m_cfg.channels.clear();
            SiteConfig::Channel cc{};
            snprintf(cc.name, sizeof(cc.name), "CC");
            cc.freqHz = legacyCcFreqHz;
            m_cfg.channels.push_back(cc);

            double spacing = double(m_cfg.chSpacUnits ? m_cfg.chSpacUnits : 100) * 125.0;
            double vf = legacyVchanBase + double(legacyVchanNum) * spacing;
            if (std::fabs(vf - cc.freqHz) > 1.0) {
                SiteConfig::Channel vc{};
                snprintf(vc.name, sizeof(vc.name), "VC");
                vc.freqHz = vf;
                m_cfg.channels.push_back(vc);
            }
            m_cfg.controlIndex = 0;
            m_cfg.voiceIndex   = m_cfg.channels.size() > 1 ? 1 : 0;
        }

        m_cfg.controlIndex = j.value("controlIndex", m_cfg.controlIndex);
        m_cfg.voiceIndex   = j.value("voiceIndex",   m_cfg.voiceIndex);

        if (m_cfg.channels.empty())
            m_cfg.resetDefaultChannelPlan();
        m_cfg.normalizeChannelPlan();
    } catch (...) {
        // Use defaults on any parse error.
    }
}

void SitePanel::addChannel() {
    SiteConfig::Channel c{};
    snprintf(c.name, sizeof(c.name), "CH %d", int(m_cfg.channels.size()) + 1);
    c.freqHz = 446.000e6;

    if (!m_cfg.channels.empty()) {
        double maxF = m_cfg.channels.front().freqHz;
        for (const auto& ch : m_cfg.channels)
            maxF = std::max(maxF, ch.freqHz);
        int band = bands::findBand(maxF);
        if (band >= 0) {
            double next = maxF + 12.5e3;
            if (next > bands::kBands[band].maxHz)
                next = bands::kBands[band].minHz;
            c.freqHz = next;
        }
    }
    m_cfg.channels.push_back(c);
    m_cfg.normalizeChannelPlan();
}

void SitePanel::removeChannel(int index) {
    if (index < 0 || index >= int(m_cfg.channels.size()))
        return;
    if (m_cfg.channels.size() <= 1)
        return;

    m_cfg.channels.erase(m_cfg.channels.begin() + index);

    if (m_cfg.controlIndex == index)      m_cfg.controlIndex = 0;
    else if (m_cfg.controlIndex > index)  --m_cfg.controlIndex;
    if (m_cfg.voiceIndex == index)        m_cfg.voiceIndex = 0;
    else if (m_cfg.voiceIndex > index)    --m_cfg.voiceIndex;

    m_cfg.normalizeChannelPlan();
}

void SitePanel::renderChannelPlan() {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Channel Plan");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.75f, 0.2f, 1.0f));
    ImGui::TextWrapped("AMATEUR RADIO USE ONLY - Transmit only on frequencies "
                       "you are licensed to use.");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Pick one row for the control channel (CC) and one for "
                        "the voice channel (VC).");

    ImGui::Spacing();

    int removeIndex = -1;

    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;

    if (ImGui::BeginTable("##channelplan", 5, tableFlags)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn("MHz",  ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("CC",   ImGuiTableColumnFlags_WidthFixed, 26.0f);
        ImGui::TableSetupColumn("VC",   ImGuiTableColumnFlags_WidthFixed, 26.0f);
        ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, 22.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < int(m_cfg.channels.size()); ++i) {
            SiteConfig::Channel& ch = m_cfg.channels[size_t(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();

            if (i == m_cfg.controlIndex)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(30, 90, 45, 90));
            else if (i == m_cfg.voiceIndex)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(35, 65, 110, 90));

            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##name", ch.name, sizeof(ch.name));

            ImGui::TableSetColumnIndex(1);
            double freqMHz = ch.freqHz / 1e6;
            const bool inBand = bands::findBand(ch.freqHz) >= 0;
            if (!inBand)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputDouble("##freq", &freqMHz, 0.0, 0.0, "%.5f"))
                ch.freqHz = freqMHz * 1e6;
            if (!inBand)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Channel frequency in MHz (2m/70cm only)");

            ImGui::TableSetColumnIndex(2);
            if (ImGui::RadioButton("##cc", m_cfg.controlIndex == i))
                m_cfg.controlIndex = i;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Use as control channel");

            ImGui::TableSetColumnIndex(3);
            if (ImGui::RadioButton("##vc", m_cfg.voiceIndex == i))
                m_cfg.voiceIndex = i;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Use as voice channel");

            ImGui::TableSetColumnIndex(4);
            ImGui::BeginDisabled(m_cfg.channels.size() <= 1);
            if (ImGui::SmallButton("x"))
                removeIndex = i;
            ImGui::EndDisabled();

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::Button("+ Add Channel", ImVec2(-1, 0)))
        addChannel();

    if (removeIndex >= 0)
        removeChannel(removeIndex);

    // Spacing used by IDEN_UP to map channel numbers to frequencies.
    int spacingIdx = (m_cfg.chSpacUnits == 50) ? 0
                   : (m_cfg.chSpacUnits == 200) ? 2 : 1;
    const char* kSpacing[] = { "6.25 kHz", "12.5 kHz", "25 kHz" };
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Spacing");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##spacing", &spacingIdx, kSpacing, 3)) {
        m_cfg.chSpacUnits = (spacingIdx == 0) ? 50 : (spacingIdx == 2) ? 200 : 100;
        m_cfg.normalizeChannelPlan();
    }

    // Recompute derived values from the edited plan.
    m_cfg.normalizeChannelPlan();

    std::string planError;
    bool valid = m_cfg.channelPlanValid(&planError);

    ImGui::Spacing();
    ImGui::Text("CC: %.5f MHz  (IDEN %u, ch %u)",
                m_cfg.ccFreqHz / 1e6,
                unsigned(m_cfg.chanId), unsigned(m_cfg.ccChanNum));
    ImGui::Text("VC: %.5f MHz  (IDEN %u, ch %u -> %.5f MHz)",
                m_cfg.voiceFreqHz() / 1e6,
                unsigned(m_cfg.chanId), unsigned(m_cfg.vChanNum),
                m_cfg.channelFrequency(m_cfg.vChanNum) / 1e6);
    ImGui::TextDisabled("IDEN base %.5f MHz, %.2f kHz spacing",
                        m_cfg.vchanBaseFreqHz / 1e6,
                        m_cfg.spacingHz() / 1e3);

    if (!valid) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", planError.c_str());
        ImGui::PopStyleColor();
    } else {
        bool offGrid = false;
        for (const auto& c : m_cfg.channels) {
            long long n = llround((c.freqHz - m_cfg.vchanBaseFreqHz) /
                                  m_cfg.spacingHz());
            double snapped = m_cfg.vchanBaseFreqHz + double(n) * m_cfg.spacingHz();
            if (std::fabs(snapped - c.freqHz) > 1.0)
                offGrid = true;
        }
        if (offGrid) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.25f, 1.0f));
            ImGui::TextWrapped("Some frequencies are not on the spacing grid; "
                               "IDEN_UP grants will snap to the nearest channel.");
            ImGui::PopStyleColor();
        }
    }

    if (ImGui::CollapsingHeader("Advanced IDEN")) {
        int iden = m_cfg.chanId;
        ImGui::Text("IDEN # (0-15)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputInt("##chanid", &iden))
            m_cfg.chanId = uint8_t(std::min(15, std::max(0, iden)));
        ImGui::TextDisabled("Base is auto-set to the lowest plan frequency.");
    }
}

void SitePanel::applyConfig() {
    try {
        m_cfg.nac   = uint16_t(std::stoul(m_nacBuf,   nullptr, 16)) & 0xFFF;
        m_cfg.wacn  = uint32_t(std::stoul(m_wacnBuf,  nullptr, 16)) & 0xFFFFF;
        m_cfg.sysid = uint16_t(std::stoul(m_sysidBuf, nullptr, 16)) & 0xFFF;
    } catch (...) {}

    m_cfg.normalizeChannelPlan();

    std::string planError;
    if (!m_cfg.channelPlanValid(&planError)) {
        m_statusMsg     = "Cannot apply: " + planError;
        m_statusIsError = true;
        return;
    }

    const bool ccRunning = m_cc.isRunning();
    const bool rxRunning = m_rx.isRunning();
    if (ccRunning) m_cc.stop();
    if (rxRunning) m_rx.stop();

    m_tx.setFrequencyTrimHz(m_cfg.txFreqTrimHz);
    m_tx.setFrequency(m_cfg.ccFreqHz);
    m_tx.setGain(m_tx.isPluto() ? m_cfg.plutoGainDb : m_cfg.gainDb);
    m_tx.setAmpEnabled(m_cfg.ampEnabled);
    m_cc.configure(m_cfg);
    m_rx.configure(m_cfg);

    if (m_tx.isOpen() && !m_tx.frequencyValid()) {
        m_statusMsg     = m_tx.lastError();
        m_statusIsError = true;
        return;
    }

    if (rxRunning) m_rx.start();
    if (ccRunning) m_cc.start();

    m_statusMsg     = "Applied.";
    m_statusIsError = false;
}

void SitePanel::render() {
    ImGui::Begin("Site Config");

    // When the radio opens (initial scan or Device menu switch), program it
    // from the saved plan immediately so the TX readout is truthful and the
    // open-time default frequency does not leave a spurious error.
    m_cfg.normalizeChannelPlan();
    if (!m_tx.isOpen())
        m_primedDevice.clear();
    if (m_tx.isOpen() && m_tx.argsString() != m_primedDevice &&
        m_cfg.channelPlanValid()) {
        m_primedDevice = m_tx.argsString();
        m_tx.setFrequencyTrimHz(m_cfg.txFreqTrimHz);
        m_tx.setFrequency(m_cfg.ccFreqHz);
        m_tx.setGain(m_tx.isPluto() ? m_cfg.plutoGainDb : m_cfg.gainDb);
        m_tx.setAmpEnabled(m_cfg.ampEnabled);
    }

    // Keep Apply/Save visible at the bottom of the docked panel; the rest of
    // the configuration scrolls inside the child region.
    const float footerH = ImGui::GetFrameHeightWithSpacing() +
                          ImGui::GetTextLineHeightWithSpacing() + 8.0f;
    ImGui::BeginChild("##site_content", ImVec2(0, -footerH), false);

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

    // ── Uplink / Site Input ───────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Uplink / Site Input");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Radio TX / Site RX (MHz)");
    ImGui::SetNextItemWidth(-1);
    double uplinkMHz = m_cfg.rxFreqHz() / 1e6;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.55f, 1.0f));
    if (ImGui::InputDouble("##uplink", &uplinkMHz, 0.0, 0.0, "%.5f"))
        m_cfg.txOffsetMHz = (uplinkMHz * 1e6 - m_cfg.ccFreqHz) / 1e6;
    ImGui::PopStyleColor();
    ImGui::TextDisabled("TX offset: %+.4f MHz (sent in IDEN_UP)", m_cfg.txOffsetMHz);

    const double uplink = m_cfg.rxFreqHz();
    const bool up2m = (uplink >= 144.0e6 && uplink <= 148.0e6);
    const bool up70 = (uplink >= 420.0e6 && uplink <= 450.0e6);
    if (!up2m && !up70) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("Uplink is outside the 2m/70cm amateur bands.");
        ImGui::PopStyleColor();
    }
    ImGui::TextDisabled("Radios transmit here; the RX radio tunes here.");
    ImGui::TextDisabled("(affiliations, registrations, voice requests)");

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

    // ── Channel Plan ──────────────────────────────────────────────────────
    renderChannelPlan();

    if (m_tx.isPluto() && (m_cfg.ccFreqHz < 325.0e6 || m_cfg.rxFreqHz() < 325.0e6)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("ADALM-Pluto cannot tune below ~325 MHz. The 2m band "
                           "will not transmit/receive; use 70cm (420-450 MHz).");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Text("TX Gain (dB)");
    // HackRF exposes 0-47 dB; the Pluto's SoapySDR gain range is 0-89 where
    // 89 dB means minimum attenuation (maximum output).
    const bool  pluto   = m_tx.isPluto();
    const float maxGain = pluto ? 89.0f : 47.0f;
    float gain = float(pluto ? m_cfg.plutoGainDb : m_cfg.gainDb);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##gain", &gain, 0.0f, maxGain, "%.0f dB")) {
        if (pluto) m_cfg.plutoGainDb = double(gain);
        else       m_cfg.gainDb      = double(gain);
    }
    if (pluto)
        ImGui::TextDisabled("Pluto: 89 dB = max output (0 = max attenuation)");

    ImGui::BeginDisabled(!m_tx.isHackRf());
    ImGui::Checkbox("RF Amplifier", &m_cfg.ampEnabled);
    ImGui::EndDisabled();

    if (ImGui::Checkbox("Minimal CC (IDEN_UP/NET_STS/RFSS_STS/SYNC only)",
                        &m_cfg.minimalCc))
        applyConfig();

    ImGui::Checkbox("Uplink RX on RTL-SDR (free it from SDRTrunk)", &m_cfg.rxUseRtlsdr);
    if (m_cfg.rxUseRtlsdr)
        ImGui::TextDisabled("Takes effect on next START TX; stop other RTL apps first.");

    ImGui::EndChild();

    // ── Apply / Save footer ───────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::Button("Apply", ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0)))
        applyConfig();
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(-1, 0)))
        saveConfig();

    if (!m_statusMsg.empty()) {
        ImVec4 col = m_statusIsError ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                                     : ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
        ImGui::TextColored(col, "%s", m_statusMsg.c_str());
    }

    ImGui::End();
}
