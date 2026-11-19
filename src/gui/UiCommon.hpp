#pragma once

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // GetInputTextState, GetActiveID: SelectableText reads the selection
#include "framework/FileSystem.hpp"

/*
 * What the window's parts (ChatWindow, AgentTab) share: the colors of the conversation and a few helpers for
 * drawing text.
 */
namespace ui {

constexpr ImVec4 kUserColor  {0.60f, 0.50f, 0.85f, 1.0f};
constexpr ImVec4 kAgentColor {0.88f, 0.62f, 0.95f, 1.0f};
constexpr ImVec4 kBusyColor  {1.00f, 0.80f, 0.35f, 1.0f};
constexpr ImVec4 kErrorColor {1.00f, 0.45f, 0.45f, 1.0f};

// Wraps at the window edge. TextUnformatted, so a '%' in a reply is shown as is.
inline void Wrapped(const std::string& text, const ImVec4* color = nullptr) {
    if (color) {
        ImGui::PushStyleColor(ImGuiCol_Text, *color);
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
    ImGui::PopTextWrapPos();
    if (color) {
        ImGui::PopStyleColor();
    }
}

inline void Dim(const std::string& text) {
    const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    Wrapped(text, &dim);
}

/*
 * Text the user can select with the mouse and copy with Ctrl+C, which plain ImGui text cannot do. It is a
 * read-only multi-line input box made to look like text: no frame, no padding, and as tall as its lines, so it
 * has nothing to scroll and the mouse wheel still scrolls the conversation. Double-click selects a word, Ctrl+A
 * all of this text. One object per piece of text: it keeps the wrapped copy of it.
 *
 * WARNING: this Dear ImGui (1.91.8) does not wrap the text of an input box, so the lines are wrapped here,
 * where ImGui's own text would wrap: a line break takes the place of the spaces at each cut, or is added inside
 * a word too long for the line. Ctrl+C copies the original text, without those breaks, so a pasted paragraph
 * is one line again.
 *
 * NOTE: the wrapped copy is redone only when the width or the text changes (an activity block grows).
 */
class SelectableText {
public:
    void Draw(const std::string& text, const ImVec4* color = nullptr) {
        const float width = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize());
        if (width != width_ || text.size() != source_size_) {
            Wrap(text, width - 1.0f);  // 1 pixel spare, so rounding never clips a line's last letter
            width_ = width;
        }
        const ImVec2  size(width, lines_ * ImGui::GetFontSize() + 1.0f);
        const ImGuiID id = ImGui::GetID("##text");
        /*
         * Out of sight, only its space, so a long conversation stays cheap to draw.
         *
         * WARNING: not while it is active: ImGui forgets the selection of a box that is not drawn, and Ctrl+C
         * would then copy nothing.
         */
        if (!ImGui::IsRectVisible(size) && ImGui::GetActiveID() != id) {
            ImGui::Dummy(size);
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        if (color) {
            ImGui::PushStyleColor(ImGuiCol_Text, *color);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        // Read-only: ImGui never writes to the buffer, it only needs a char* to it.
        ImGui::InputTextMultiline("##text", shown_.data(), shown_.size() + 1, size, ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(color ? 2 : 1);

        // ImGui has just copied the selection with the added breaks; this copy takes its place on the clipboard.
        const bool copy = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C) ||
                          ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Insert);
        const ImGuiInputTextState* state = copy ? ImGui::GetInputTextState(id) : nullptr;
        if (state && state->HasSelection()) {
            const int    lo = std::min(state->GetSelectionStart(), state->GetSelectionEnd());
            const int    hi = std::max(state->GetSelectionStart(), state->GetSelectionEnd());
            const size_t a  = std::min(static_cast<size_t>(lo), shown_.size());
            const size_t b  = std::min(static_cast<size_t>(hi), shown_.size());
            ImGui::SetClipboardText(text.substr(source_[a], source_[b] - source_[a]).c_str());
        }
    }

private:
    std::string         shown_;              // the text with its lines wrapped
    std::vector<size_t> source_;             // for each byte of shown_, and its end, where it comes from in the text
    int                 lines_       = 1;
    float               width_       = -1.0f;
    size_t              source_size_ = std::string::npos;

    void Wrap(const std::string& text, float width) {
        source_size_ = text.size();
        shown_.clear();
        source_.clear();
        lines_ = 1;

        ImFont*     font  = ImGui::GetFont();
        const float scale = ImGui::GetFontSize() / font->FontSize;
        const char* begin = text.c_str();
        const char* end   = begin + text.size();
        const auto  keep  = [&](const char* from, const char* to) {
            for (const char* p = from; p < to; ++p) {
                shown_ += *p;
                source_.push_back(static_cast<size_t>(p - begin));
            }
        };
        const auto  line_break = [&](const char* at) {
            shown_ += '\n';
            source_.push_back(static_cast<size_t>(at - begin));
            ++lines_;
        };

        for (const char* s = begin; s < end;) {
            const char* eol = static_cast<const char*>(std::memchr(s, '\n', static_cast<size_t>(end - s)));
            if (!eol) {
                eol = end;
            }
            // One paragraph, cut into lines that fit the width.
            while (true) {
                const char* cut = font->CalcWordWrapPositionA(scale, s, eol, width);
                if (cut >= eol) {
                    keep(s, eol);
                    break;
                }
                // WARNING: when no letter fits, ImGui moves on by one byte, which may split a UTF-8 character.
                while (cut < eol && (static_cast<unsigned char>(*cut) & 0xC0) == 0x80) {
                    ++cut;
                }
                keep(s, cut);
                s = cut;
                while (s < eol && (*s == ' ' || *s == '\t')) {
                    ++s;
                }
                if (s >= eol) {
                    break;  // only spaces were left: no empty line for them
                }
                line_break(cut);
            }
            if (eol == end) {
                break;
            }
            line_break(eol);  // the text's own line break
            s = eol + 1;
        }
        source_.push_back(text.size());
    }
};

inline bool IsBlank(const std::string& s) {
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

inline std::string Trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// A path under the program's folder as the user knows it ("config\agents\Researcher.txt"); others in full.
inline std::string ShortPath(const std::string& path) {
    const std::filesystem::path rel = std::filesystem::u8path(path).lexically_relative(fileSystem.BasePath());
    const std::string s = rel.u8string();
    return (s.empty() || s.compare(0, 2, "..") == 0) ? path : s;
}

}  // namespace ui
