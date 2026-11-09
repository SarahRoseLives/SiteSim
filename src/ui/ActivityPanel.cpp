#include "ActivityPanel.hpp"
#include "imgui.h"
#include <chrono>
#include <cstdio>
#include <vector>

namespace {

double ageSeconds(std::chrono::steady_clock::time_point t)
{
    if (t.time_since_epoch().count() == 0) return 1e9;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

void formatAge(double s, char* out, size_t n)
{
    if (!(s >= 0.0)) s = 0.0;
    if (s < 60.0)        std::snprintf(out, n, "%.1fs", s);
    else if (s < 3600.0) std::snprintf(out, n, "%dm%02ds", int(s) / 60, int(s) % 60);
    else                 std::snprintf(out, n, "%.0fm", s / 60.0);
}

} // namespace

void ActivityPanel::render()
{
    ImGui::Begin("System Activity");

    const std::vector<RadioActivity> radios = m_cc.radioSnapshot();

    int registered = 0, affiliated = 0, keyed = 0, emergency = 0;
    const RadioActivity* active = nullptr;
    const RadioActivity* activeUu = nullptr;
    for (const auto& r : radios) {
        if (r.registered) registered++;
        if (r.affiliated) affiliated++;
        if (r.keyed) {
            keyed++;
            if (r.emergency) emergency++;
            if (!r.unitToUnit && !active) active = &r;
            if (r.unitToUnit && !activeUu) activeUu = &r;
        }
    }

    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Subscribers");
    ImGui::SameLine();
    ImGui::Text("Registered:%d  Affiliated:%d", registered, affiliated);
    ImGui::SameLine();
    ImGui::TextColored(keyed ? ImVec4(0.3f, 1.0f, 0.45f, 1.0f)
                             : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                       "Keyed:%d", keyed);
    if (emergency > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "EMERGENCY:%d", emergency);
    }
    ImGui::Separator();

    if (active || activeUu) {
        const RadioActivity* c = active ? active : activeUu;
        const char* title = c->emergency ? "EMERGENCY CALL" : (c->unitToUnit ? "UNIT-TO-UNIT CALL" : "ACTIVE CALL");
        const ImVec4 col = c->emergency ? ImVec4(1.0f, 0.30f, 0.30f, 1.0f)
                                        : ImVec4(0.25f, 1.0f, 0.40f, 1.0f);
        ImGui::TextColored(col, "%s", title);
        ImGui::Text("TG %-5u   SRC %-8u   VC %.5f MHz  (IDEN %u ch %u)",
                    unsigned(c->talkgroup), unsigned(c->unitId),
                    c->activeFreqHz / 1e6, unsigned(c->activeIden),
                    unsigned(c->activeChan));
        ImGui::TextDisabled("keyed for %.1f s", ageSeconds(c->keyedSince));
    } else {
        ImGui::TextDisabled("No active call - PTT on a subscribed radio to see it here.");
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (radios.empty()) {
        ImGui::TextDisabled("No subscribers seen yet.");
        ImGui::TextWrapped("Waiting for unit registration or group affiliation on the "
                           "site input channel.");
        ImGui::End();
        return;
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("##radios", 4, flags,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Unit",   ImGuiTableColumnFlags_WidthFixed, 84.0f);
        ImGui::TableSetupColumn("TG",     ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Last",   ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableHeadersRow();

        for (const auto& r : radios) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", unsigned(r.unitId));

            ImGui::TableSetColumnIndex(1);
            if (r.talkgroup) ImGui::Text("%u", unsigned(r.talkgroup));
            else             ImGui::TextDisabled("-");

            ImGui::TableSetColumnIndex(2);
            if (r.keyed) {
                const ImVec4 col = r.emergency ? ImVec4(1.0f, 0.30f, 0.30f, 1.0f)
                                               : ImVec4(0.25f, 1.0f, 0.40f, 1.0f);
                ImGui::TextColored(col, "%s",
                                   r.emergency ? "KEYED [EMRG]" :
                                   r.unitToUnit ? "KEYED [UU]" : "KEYED");
            } else if (r.affiliated) {
                ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "Affiliated");
            } else if (r.registered) {
                ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "Registered");
            } else {
                ImGui::TextDisabled("-");
            }

            ImGui::TableSetColumnIndex(3);
            char age[24];
            formatAge(ageSeconds(r.lastSeen), age, sizeof(age));
            ImGui::TextDisabled("%s", age);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}
