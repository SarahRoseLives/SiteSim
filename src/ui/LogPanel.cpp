#include "LogPanel.hpp"
#include "imgui.h"

LogPanel::LogPanel(ControlChannel& cc) : m_cc(cc) {}

void LogPanel::addRxEntry(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_rxMutex);
    m_rxLog.push_back(msg);
    if (m_rxLog.size() > kMaxRxEntries)
        m_rxLog.pop_front();
}

void LogPanel::render() {
    ImGui::Begin("Activity Log");

    float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    float panelH = ImGui::GetContentRegionAvail().y;

    // ── TX Log ────────────────────────────────────────────────────────────
    ImGui::BeginChild("##tx_panel", ImVec2(halfW, panelH), true);

    ImGui::TextColored(ImVec4(0.5f, 0.75f, 1.0f, 1.0f), "TX Log");
    ImGui::SameLine();
    auto entries = m_cc.getLog(200);
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(%zu)", entries.size());
    ImGui::Separator();

    bool txNew = entries.size() != m_lastTxCount;
    m_lastTxCount = entries.size();

    ImGui::BeginChild("##tx_scroll", ImVec2(0, 0), false,
                       ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& e : entries) {
        if (e.msg.find("ACT") != std::string::npos)
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", e.msg.c_str());
        else if (e.msg.find("SYS") != std::string::npos)
            ImGui::TextColored(ImVec4(0.5f, 0.75f, 1.0f, 1.0f), "%s", e.msg.c_str());
        else
            ImGui::TextUnformatted(e.msg.c_str());
    }
    if (txNew) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::EndChild(); // tx_panel

    ImGui::SameLine();

    // ── RX Log ────────────────────────────────────────────────────────────
    ImGui::BeginChild("##rx_panel", ImVec2(halfW, panelH), true);

    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "RX Log");
    {
        std::lock_guard<std::mutex> lock(m_rxMutex);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(%zu)", m_rxLog.size());
    }
    ImGui::Separator();

    ImGui::BeginChild("##rx_scroll", ImVec2(0, 0), false,
                       ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lock(m_rxMutex);
        bool rxNew = m_rxLog.size() != m_lastRxCount;
        m_lastRxCount = m_rxLog.size();

        if (m_rxLog.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "Waiting for RTL-SDR / dsd-neo...");
        } else {
            for (const auto& msg : m_rxLog) {
                if (msg.find("REG") != std::string::npos || msg.find("AFF") != std::string::npos)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s", msg.c_str());
                else if (msg.find("GRANT") != std::string::npos)
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", msg.c_str());
                else if (msg.find("EMRG") != std::string::npos || msg.find("EMERGENCY") != std::string::npos)
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", msg.c_str());
                else if (msg.find("WARNING") != std::string::npos || msg.find("ERR") != std::string::npos)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", msg.c_str());
                else if (msg.find("RX started") != std::string::npos)
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s", msg.c_str());
                else
                    ImGui::TextUnformatted(msg.c_str());
            }
        }
        if (rxNew) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::EndChild(); // rx_panel

    ImGui::End();
}

