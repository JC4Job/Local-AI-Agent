#pragma once

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

#include "imgui.h"
#include "bio/EndocrineSystem.hpp"
#include "gui/UiCommon.hpp"

/*
 * LifePanel - draws an agent's body (bio/EndocrineSystem.hpp) with Dear ImGui: its feeling, on the line above
 * the conversation (DrawFeeling), and its life, under a header the user can fold (DrawLife): the hormones now,
 * the share of its life it spent in each state, and how its work has gone.
 *
 * AgentTab draws them for its agent, from the copy of the body it takes under its session's lock each frame.
 */
namespace ui {

inline ImVec4 LifeStateColor(LifeState state) {
    switch (state) {
    case LifeState::Homeostasis:    return {0.55f, 0.85f, 0.60f, 1.0f};
    case LifeState::FightOrFlight:  return kErrorColor;
    case LifeState::ChronicBurnout: return {0.85f, 0.55f, 0.30f, 1.0f};
    case LifeState::DeepRest:       return {0.50f, 0.65f, 1.00f, 1.0f};
    case LifeState::HighMotivation: return kBusyColor;
    case LifeState::Lethargic:      return {0.62f, 0.62f, 0.68f, 1.0f};
    case LifeState::Count:          break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

// The bar colors, muted so the number drawn over a bar stays readable.
inline ImVec4 HormoneColor(HormoneType type) {
    static constexpr ImVec4 kColors[kHormoneCount] = {
        {0.85f, 0.50f, 0.25f, 0.75f},  // cortisol
        {0.85f, 0.30f, 0.30f, 0.75f},  // adrenaline
        {0.40f, 0.42f, 0.85f, 0.75f},  // melatonin
        {0.30f, 0.70f, 0.62f, 0.75f},  // serotonin
        {0.80f, 0.40f, 0.78f, 0.75f},  // dopamine
        {0.80f, 0.70f, 0.30f, 0.75f},  // insulin
        {0.40f, 0.70f, 0.35f, 0.75f},  // testosterone
    };
    return kColors[EnumIndex(type)];
}

// "45 s", "12 min", "3 h 05 min", "2 d 4 h".
inline std::string Duration(double seconds) {
    const long long s = static_cast<long long>(seconds);
    char text[48];
    if (s < 60) {
        std::snprintf(text, sizeof(text), "%lld s", s);
    } else if (s < 3600) {
        std::snprintf(text, sizeof(text), "%lld min", s / 60);
    } else if (s < 86400) {
        std::snprintf(text, sizeof(text), "%lld h %02lld min", s / 3600, (s / 60) % 60);
    } else {
        std::snprintf(text, sizeof(text), "%lld d %lld h", s / 86400, (s / 3600) % 24);
    }
    return text;
}

// "3 of 5 (60%)"; "none yet" when there is nothing to count.
inline std::string Ratio(long long part, long long whole) {
    if (whole <= 0) {
        return "none yet";
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%lld of %lld (%.0f%%)", part, whole, 100.0 * part / whole);
    return text;
}

/*
 * The agent's feeling, after what is already on the line: its life state, in its color, with what the state
 * means and what it does to the chat on hover.
 */
inline void DrawFeeling(const EndocrineSystem& body) {
    const Mood           mood = body.CurrentMood();
    const LifeStateInfo& info = Info(mood.state);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextColored(LifeStateColor(mood.state), "Feeling: %s", info.name);
    if (!ImGui::IsItemHovered()) {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(info.meaning);
    ImGui::Spacing();
    if (*mood.note) {
        ImGui::TextDisabled("Put in front of each message to the model:");
        ImGui::TextUnformatted(mood.note);
    } else {
        ImGui::TextDisabled("The messages go to the model as they are.");
    }
    if (mood.temperature > 0.0) {
        ImGui::TextDisabled("Sampling temperature %.1f, below the model's own.", mood.temperature);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Its life so far is under the header below.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/*
 * A row of the life panel: `label` in the first `label_w` pixels, then a bar filled to `fraction` with `text`
 * over it. Returns true when the mouse is over the row, for its tooltip.
 *
 * WARNING: not SameLine(label_w): in a table cell its offset starts at the window's edge, before the padding,
 * and the bars touched their labels.
 */
inline bool LifeBar(const char* label, const ImVec4& label_color, float label_w, float fraction, const char* text,
                    const ImVec4& bar_color) {
    const float height = ImGui::GetTextLineHeight();
    const float x      = ImGui::GetCursorPosX();
    ImGui::TextColored(label_color, "%s", label);
    bool hovered = ImGui::IsItemHovered();
    ImGui::SameLine();
    ImGui::SetCursorPosX(x + label_w);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, bar_color);
    ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, height), text);
    ImGui::PopStyleColor();
    return hovered || ImGui::IsItemHovered();
}

inline void RecordLine(const char* label, const std::string& value) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    ImGui::TextUnformatted(value.c_str());
}

/*
 * The agent's life under a header, open until the user folds it to give the conversation more room: three
 * columns, the hormones now, the share of its life spent in each state, and its record.
 */
inline void DrawLife(const EndocrineSystem& body, const std::string& name) {
    const std::string header = "Life of " + name + "###life";
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 dim  = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const float  gap  = ImGui::GetStyle().ItemSpacing.x;

    if (!ImGui::BeginTable("##life", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        return;
    }

    ImGui::TableNextColumn();
    ImGui::SeparatorText("Hormones now");
    float hormone_w = 0.0f;
    for (const HormoneInfo& info : kHormoneInfo) {
        hormone_w = std::max(hormone_w, ImGui::CalcTextSize(info.name).x + gap);
    }
    for (size_t i = 0; i < kHormoneCount; ++i) {
        const Hormone& h = body.Get(static_cast<HormoneType>(i));
        char level[16];
        std::snprintf(level, sizeof(level), "%.0f", h.Level());
        const bool hovered = LifeBar(h.Name(), text, hormone_w, static_cast<float>(h.Level() / 100.0), level,
                                     HormoneColor(h.Type()));
        // The resting level, as a mark on the bar.
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        const float  x = a.x + (b.x - a.x) * static_cast<float>(h.Baseline() / 100.0);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), ImGui::GetColorU32(ImGuiCol_Text, 0.7f),
                                            2.0f);
        if (hovered) {
            ImGui::SetTooltip("%s: %.1f of 100. At rest it settles to %.0f, the mark on the bar.", h.Name(),
                              h.Level(), h.Baseline());
        }
    }

    ImGui::TableNextColumn();
    ImGui::SeparatorText("Share of its life in each state");
    float state_w = 0.0f;
    for (const LifeStateInfo& info : kLifeStateInfo) {
        state_w = std::max(state_w, ImGui::CalcTextSize(info.name).x + gap);
    }
    for (size_t i = 0; i < kLifeStateCount; ++i) {
        const LifeState      state = static_cast<LifeState>(i);
        const LifeStateInfo& info  = Info(state);
        const double         share = body.ShareOf(state);
        const bool           now   = body.State() == state;
        char percent[16];
        std::snprintf(percent, sizeof(percent), "%.1f%%", share * 100.0);
        ImVec4 bar = LifeStateColor(state);
        bar.w      = 0.6f;
        // The state it is in now is in its own color; the others are dim.
        if (LifeBar(info.name, now ? LifeStateColor(state) : dim, state_w, static_cast<float>(share), percent, bar)) {
            ImGui::SetTooltip("%s%s: %s of its life.\n%s", info.name, now ? " (now)" : "",
                              Duration(body.SecondsIn(state)).c_str(), info.meaning);
        }
    }
    ImGui::TextDisabled("Lived %s with its window open.", Duration(body.AliveSeconds()).c_str());

    ImGui::TableNextColumn();
    ImGui::SeparatorText("Record");
    char              born[32] = "?";
    const std::time_t when     = body.Born();
    if (const std::tm* local = std::localtime(&when)) {
        std::strftime(born, sizeof(born), "%Y-%m-%d %H:%M", local);
    }
    const long long worked = body.Count(Feeling::Did) + body.Count(Feeling::Found);
    RecordLine("Born", born);
    RecordLine("Messages answered", Ratio(body.Count(Feeling::Answered), body.Count(Feeling::Asked)));
    RecordLine("Tasks finished", Ratio(body.Count(Feeling::GoalDone), body.Count(Feeling::GoalGiven)));
    RecordLine("Tool calls that worked", Ratio(worked, worked + body.Count(Feeling::Failed)));
    RecordLine("Not allowed by you", std::to_string(body.Count(Feeling::Refused)));
    RecordLine("Calls made again", std::to_string(body.Count(Feeling::Repeated)));
    RecordLine("Stopped by you", std::to_string(body.Count(Feeling::Stopped)));
    RecordLine("No reply from the model", std::to_string(body.Count(Feeling::NoReply)));

    ImGui::EndTable();
    ImGui::Spacing();
}

}  // namespace ui
