#include "CCPanel.hpp"
#include "imgui.h"
#include <cstdio>
#include <cmath>

CCPanel::CCPanel(ControlChannel& cc, SoapyTx& tx) : m_cc(cc), m_tx(tx) {}

void CCPanel::render() {
    ImGui::Begin("Control Channel");

    const auto& cfg = [this]() -> const SiteConfig& {
        // We need to access config — stored in ControlChannel
        // We'll just read what we know from the tx side
        static SiteConfig dummy;
        return dummy;
    };
    (void)cfg;

    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "System Status");
    ImGui::Separator();
    ImGui::Spacing();

    // TX status indicator
    bool running = m_cc.isRunning();
    if (running) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "\xe2\x97\x8f TX ACTIVE");
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "\xe2\x97\x8b Stopped");
    }

    ImGui::Spacing();

    // Frames sent
    ImGui::Text("Frames sent: %llu", (unsigned long long)m_cc.frameCount());

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
        if (ImGui::Button("  \xe2\x96\xa0  STOP TX  ", ImVec2(-1, 48))) {
            m_cc.stop();
            m_tx.stopTx();
        }
        ImGui::PopStyleColor(3);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.05f, 0.45f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.60f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (ImGui::Button("  \xe2\x96\xb6  START TX  ", ImVec2(-1, 48))) {
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
