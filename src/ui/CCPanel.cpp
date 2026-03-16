#include "CCPanel.hpp"
#include "imgui.h"
#include <cstdio>
#include <cmath>

CCPanel::CCPanel(ControlChannel& cc, SoapyTx& tx) : m_cc(cc), m_tx(tx) {}

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
    ImGui::Text("TX (HackRF):  %.6f MHz", cfg.ccFreqHz / 1e6);
    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.55f, 1.0f),
                       "RX (RTL-SDR): %.6f MHz", cfg.rxFreqHz() / 1e6);

    ImGui::Spacing();

    // Frames sent
    ImGui::Text("Frames TX: %llu", (unsigned long long)m_cc.frameCount());

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
            m_cc.stop();
            m_tx.stopTx();
        }
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.05f, 0.45f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.60f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("  [>]  START TX  ", ImVec2(-1, 48))) {
            m_tx.startTx();
            m_cc.start();
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
