#include "CCPanel.hpp"
#include "imgui.h"
#include <cstdio>
#include <cmath>

CCPanel::CCPanel(ControlChannel& cc, SoapyTx& tx, RxPipeline& rx)
    : m_cc(cc), m_tx(tx), m_rx(rx) {}

void CCPanel::render(const SiteConfig& cfg) {
    ImGui::Begin("Control Channel");

    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "System Status");
    ImGui::Separator();
    ImGui::Spacing();

    // TX status indicator
    bool running = m_cc.isRunning();
    if (running) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[*] TX ACTIVE");
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[ ] Stopped");
    }

    ImGui::Spacing();

    // TX / Site-input frequencies
    ImGui::Text("TX (%s):  %.6f MHz @ %.0f dB", m_tx.displayName().c_str(),
                m_tx.frequency() / 1e6, m_tx.gain());
    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.55f, 1.0f),
                       "RX (%s): %.6f MHz",
                       m_rx.sourceName().c_str(), cfg.rxFreqHz() / 1e6);

    if (m_tx.isOpen() && m_rx.isSoapy() && m_tx.isPluto()) {
        ImGui::TextDisabled("Single-radio full duplex (shared %s)", m_tx.displayName().c_str());
    }
    if (m_tx.isPluto() && (cfg.ccFreqHz < 325.0e6 || cfg.rxFreqHz() < 325.0e6)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("This Pluto cannot tune below ~325 MHz. Use a 70cm "
                           "frequency (420-450 MHz) or an upconverter.");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // Frames sent
    ImGui::Text("Frames TX: %llu", (unsigned long long)m_cc.frameCount());

    ImGui::Spacing();
    ImGui::Text("CC: %.5f MHz  (IDEN %u, ch %u)",
                cfg.ccFreqHz / 1e6, unsigned(cfg.chanId), unsigned(cfg.ccChanNum));
    ImGui::Text("VC: %.5f MHz  (IDEN %u, ch %u)",
                cfg.voiceFreqHz() / 1e6, unsigned(cfg.chanId), unsigned(cfg.vChanNum));

    if (!m_startError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", m_startError.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Big TX button with pulsing colour
    if (running) {
        float t     = float(ImGui::GetTime());
        float pulse = 0.5f + 0.5f * std::sin(t * 5.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImVec4(0.55f + 0.25f * pulse, 0.05f, 0.05f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("  [X]  STOP TX  ", ImVec2(-1, 48))) {
            m_startError.clear();
            m_cc.stop();
            m_rx.stop();
            m_tx.stopTx();
        }
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.05f, 0.45f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.60f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("  [>]  START TX  ", ImVec2(-1, 48))) {
            // Always program the radio from the values shown in the panel, even
            // if the user never pressed Apply. Otherwise the UI can claim a
            // frequency the hardware was never tuned to.
            SiteConfig plan = cfg;
            plan.normalizeChannelPlan();
            std::string planError;
            if (!plan.channelPlanValid(&planError)) {
                m_startError = planError;
            } else {
                m_tx.setFrequency(plan.ccFreqHz);
                m_tx.setGain(m_tx.isPluto() ? plan.plutoGainDb : plan.gainDb);
                m_tx.setAmpEnabled(plan.ampEnabled);
                m_cc.configure(plan);

                // Never key the radio if the hardware rejected the frequency:
                // the LO would stay where it was and transmit somewhere
                // unintended.
                if (!m_tx.isOpen() || m_tx.frequencyValid()) {
                    // A dedicated RTL-SDR decodes the uplink reliably; the
                    // Pluto's own full-duplex RX does not deliver decodable
                    // samples in practice, so allow running RX on the RTL.
                    const bool sharedPluto = m_tx.isPluto() && !plan.rxUseRtlsdr;
                    if (sharedPluto) {
                        m_rx.useSharedSoapyDevice(
                            m_tx.device(), m_tx.argsString(), "pluto",
                            static_cast<uint32_t>(m_tx.sampleRate() > 0.0 ? m_tx.sampleRate()
                                                                           : 1536000.0));
                    } else {
                        m_rx.useRtlsdrSource();
                    }
                    if (sharedPluto) {
                        // Full duplex on one radio: bring RX up first so
                        // dsd-neo can share the already-open device, then start
                        // the TX stream.
                        m_rx.configure(plan);
                        m_rx.start();
                        m_tx.startTx();
                    } else {
                        m_tx.startTx();
                        m_rx.configure(plan);
                        m_rx.start();
                    }
                    m_cc.start();
                    m_startError.clear();
                } else {
                    m_startError = m_tx.lastError();
                }
            }
        }
        ImGui::PopStyleColor(3);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Device status
    ImGui::Text("SDR Device:");
    ImGui::SameLine();
    if (m_tx.isOpen()) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "Connected");
    } else {
        ImGui::TextColored(ImVec4(0.8f, 0.5f, 0.1f, 1.0f), "Not connected (no hardware)");
    }

    if (!m_tx.lastError().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", m_tx.lastError().c_str());
    }

    ImGui::End();
}
