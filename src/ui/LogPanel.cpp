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

    if (ImGui::BeginTabBar("##log_tabs")) {

        // ── TX Log ────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("TX")) {
            auto entries = m_cc.getLog(200);
            bool newEntries = entries.size() != m_lastTxCount;
            m_lastTxCount = entries.size();

            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "%zu entries", entries.size());
            ImGui::Separator();

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

            if (newEntries)
                ImGui::SetScrollHereY(1.0f);

            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ── RX Log ────────────────────────────────────────────────────────
        if (ImGui::BeginTabItem("RX")) {
            std::lock_guard<std::mutex> lock(m_rxMutex);
            bool newEntries = m_rxLog.size() != m_lastRxCount;
            m_lastRxCount = m_rxLog.size();

            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "%zu entries", m_rxLog.size());
            ImGui::Separator();

            ImGui::BeginChild("##rx_scroll", ImVec2(0, 0), false,
                               ImGuiWindowFlags_HorizontalScrollbar);

            if (m_rxLog.empty()) {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                   "Waiting for RTL-SDR / dsd-neo...");
            } else {
                for (const auto& msg : m_rxLog) {
                    // Colour-code decoded P25 message types
                    if (msg.find("REG") != std::string::npos || msg.find("AFF") != std::string::npos)
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s", msg.c_str());
                    else if (msg.find("GRANT") != std::string::npos)
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", msg.c_str());
                    else if (msg.find("ERR") != std::string::npos || msg.find("FAIL") != std::string::npos)
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", msg.c_str());
                    else
                        ImGui::TextUnformatted(msg.c_str());
                }
            }

            if (newEntries)
                ImGui::SetScrollHereY(1.0f);

            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

