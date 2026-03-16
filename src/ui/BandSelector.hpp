#pragma once
#include "imgui.h"
#include <cstdint>
#include <cstdio>
#include <cmath>

// ── Amateur radio band definitions ───────────────────────────────────────────
// Frequencies are restricted to these ranges to prevent accidental or
// intentional transmission on public-safety / commercial P25 frequencies.
// Only amateur radio bands are permitted per the project's intended use.

namespace bands {

struct Band {
    const char* name;
    double      minHz;
    double      maxHz;
    const char* note;  // suggested use / common P25 simplex freqs
};

static constexpr Band kBands[] = {
    { "2m  — 144–148 MHz",  144.0e6, 148.0e6, "Common: 145.050, 146.520" },
    { "70cm — 420–450 MHz", 420.0e6, 450.0e6, "Common: 446.000, 446.500" },
};
static constexpr int kNumBands = 2;

// Returns the band index containing freqHz, or -1 if out of all bands.
inline int findBand(double freqHz) {
    for (int i = 0; i < kNumBands; i++) {
        if (freqHz >= kBands[i].minHz && freqHz <= kBands[i].maxHz)
            return i;
    }
    return -1;
}

// Clamp freqHz to the limits of band[bandIdx].
inline double clampToBand(double freqHz, int bandIdx) {
    if (bandIdx < 0 || bandIdx >= kNumBands) return freqHz;
    if (freqHz < kBands[bandIdx].minHz) return kBands[bandIdx].minHz;
    if (freqHz > kBands[bandIdx].maxHz) return kBands[bandIdx].maxHz;
    return freqHz;
}

} // namespace bands

// ── BandSelector widget ───────────────────────────────────────────────────────
// Self-contained Dear ImGui widget.  Call render() each frame.
// freqHz is read/written; returns true if the frequency changed.
//
// Usage:
//   static BandSelector bs;
//   if (bs.render(myCfg.ccFreqHz)) { /* freq changed */ }

class BandSelector {
public:
    BandSelector() {
        // Initialise band index from default frequency
        m_bandIdx = bands::findBand(m_freqHz);
        if (m_bandIdx < 0) m_bandIdx = 1; // default: 2m
        syncBuf();
    }

    // Render the widget.  freqHz is modified in place; returns true on change.
    bool render(double& freqHz) {
        bool changed = false;

        // Sync internal state if caller changed freqHz externally
        if (std::abs(freqHz - m_freqHz) > 0.5) {
            m_freqHz  = freqHz;
            int bi    = bands::findBand(freqHz);
            if (bi >= 0) m_bandIdx = bi;
            syncBuf();
        }

        // ── Amateur band disclaimer ───────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.75f, 0.2f, 1.0f));
        ImGui::TextWrapped("AMATEUR RADIO USE ONLY — Transmit only on "
                           "frequencies you are licensed to use.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // ── Band selector dropdown ────────────────────────────────────────
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##band", bands::kBands[m_bandIdx].name)) {
            for (int i = 0; i < bands::kNumBands; i++) {
                bool sel = (i == m_bandIdx);
                if (ImGui::Selectable(bands::kBands[i].name, sel)) {
                    m_bandIdx = i;
                    // Clamp current freq to new band (move to band min if outside)
                    m_freqHz  = bands::clampToBand(m_freqHz, m_bandIdx);
                    freqHz    = m_freqHz;
                    syncBuf();
                    changed   = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Band");
        ImGui::Spacing();

        // ── Frequency input (MHz, clamped to band) ────────────────────────
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.45f, 1.0f));
        bool edited = ImGui::InputText("##freq", m_buf, sizeof(m_buf),
                                       ImGuiInputTextFlags_CharsDecimal |
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor();

        if (edited || ImGui::IsItemDeactivatedAfterEdit()) {
            double entered = std::atof(m_buf) * 1e6;
            const auto& b  = bands::kBands[m_bandIdx];
            if (entered < b.minHz || entered > b.maxHz) {
                // Out of band — reject and show warning
                m_outOfBand = true;
                syncBuf(); // revert display
            } else {
                m_outOfBand = false;
                m_freqHz    = entered;
                freqHz      = m_freqHz;
                changed     = true;
            }
        }

        // Band range hint / out-of-band warning
        const auto& b = bands::kBands[m_bandIdx];
        if (m_outOfBand) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("Out of band! Valid range: %.3f – %.3f MHz",
                               b.minHz / 1e6, b.maxHz / 1e6);
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Range: %.3f – %.3f MHz",
                                b.minHz / 1e6, b.maxHz / 1e6);
        }
        ImGui::TextDisabled("Frequency (MHz)");
        ImGui::Spacing();

        // ── Common frequencies quick-pick ─────────────────────────────────
        renderQuickPick(freqHz, changed);

        return changed;
    }

private:
    double m_freqHz   = 145.050e6;
    int    m_bandIdx  = 0;          // 2m default
    bool   m_outOfBand = false;
    char   m_buf[32]  = {};

    void syncBuf() {
        std::snprintf(m_buf, sizeof(m_buf), "%.6f", m_freqHz / 1e6);
    }

    // Per-band quick-pick buttons for common P25 simplex/repeater frequencies
    void renderQuickPick(double& freqHz, bool& changed) {
        struct Preset { const char* label; double hz; };

        // 2m — index 0
        static constexpr Preset k2m[] = {
            {"145.050", 145.050e6},
            {"146.520", 146.520e6},
            {"147.000", 147.000e6},
            {"146.000", 146.000e6},
        };
        // 70cm — index 1
        static constexpr Preset k70cm[] = {
            {"446.000", 446.000e6},
            {"446.500", 446.500e6},
            {"445.000", 445.000e6},
            {"440.000", 440.000e6},
        };

        const Preset* presets = nullptr;
        int            count  = 0;
        switch (m_bandIdx) {
            case 0: presets = k2m;   count = 4; break;
            case 1: presets = k70cm; count = 4; break;
        }
        if (!presets || count == 0) return;

        ImGui::TextDisabled("Quick-pick:");
        float btnW = (ImGui::GetContentRegionAvail().x - (count - 1) * ImGui::GetStyle().ItemSpacing.x) / count;
        for (int i = 0; i < count; i++) {
            if (i > 0) ImGui::SameLine();
            bool active = (std::abs(freqHz - presets[i].hz) < 500.0);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.75f, 1.0f));
            if (ImGui::Button(presets[i].label, ImVec2(btnW, 0))) {
                m_freqHz    = presets[i].hz;
                freqHz      = m_freqHz;
                m_bandIdx   = bands::findBand(m_freqHz);
                m_outOfBand = false;
                syncBuf();
                changed     = true;
            }
            if (active) ImGui::PopStyleColor();
        }
    }
};
