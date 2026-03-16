#include "LogPanel.hpp"
#include "imgui.h"

LogPanel::LogPanel(ControlChannel& cc) : m_cc(cc) {}

void LogPanel::render() {
    ImGui::Begin("Activity Log");

    auto entries = m_cc.getLog(200);
    bool newEntries = entries.size() != m_lastCount;
    m_lastCount = entries.size();

    if (ImGui::Button("Clear")) {
        // Can't clear from here (no clear method), just reset display
        m_lastCount = 0;
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%zu entries", entries.size());

    ImGui::Separator();

    ImGui::BeginChild("##log_scroll", ImVec2(0, 0), false,
                       ImGuiWindowFlags_HorizontalScrollbar);

    for (const auto& e : entries) {
        // Colour-code by prefix
        if (e.msg.find("[sim]") != std::string::npos)
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", e.msg.c_str());
        else if (e.msg.find("SYS") != std::string::npos)
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), "%s", e.msg.c_str());
        else
            ImGui::TextUnformatted(e.msg.c_str());
    }

    // Auto-scroll to bottom when new entries arrive
    if (newEntries)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}
