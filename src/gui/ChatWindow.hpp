#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "agent/AgentConfig.hpp"
#include "gui/AgentTab.hpp"
#include "gui/UiCommon.hpp"
#include "llm/OllamaClient.hpp"

/*
 * ChatWindow - the chat screen, drawn with Dear ImGui: one tab per agent (AgentTab), and what is no one agent's.
 * It and AgentTab are the only places that know about both the agents and ImGui.
 *
 * The first tab is the default agent (config\settings.txt). The + at the end of the tab bar makes another, with
 * its own name, model and instructions (New agent); it is saved as config\agents\<name>.txt and opens again at
 * the next start. Every agent has its own conversation and worker, so they work at the same time. Delete agent,
 * in an agent's tab, removes its file (a copy goes to config\backup\).
 *
 * Settings, in the header, opens the settings of the agent in the tab shown: its model, the size of its
 * context and replies, its tool rounds and Run task steps, how much a knowledge base search returns, and, for
 * an agent made in the window, its instructions. The default agent's are every agent's base, saved in this
 * computer's settings file (ChatAgentConfig::SaveMachineSettings) with what is the same for every agent (the
 * embedding context). An agent made in the window saves in its own file only what differs from the default
 * agent's (SaveAgentSettings), so it follows the default agent in the rest. Save gives the new settings to
 * the agents they change, keeping their conversations; Cancel changes nothing.
 *
 * Code that an agent wants to run and CLISecurityChecking flags waits in a popup here, one question at a time,
 * naming the agent that asks.
 *
 * WARNING: so that no agent is changed under its worker, Settings waits for that agent to finish, and for every
 * agent when it is the default one.
 *
 * WARNING: the confirmation popup waits while another dialog is open: opening it would close that dialog.
 */
class ChatWindow {
public:
    // Opens the default agent and every agent made in the window before (config\agents\).
    ChatWindow() {
        AddTab(ChatAgentConfig::Load());
        for (const std::filesystem::path& file : ChatAgentConfig::AgentFiles()) {
            AddTab(ChatAgentConfig::Load(file));
        }
    }

    ChatWindow(const ChatWindow&)             = delete;
    ChatWindow& operator=(const ChatWindow&)  = delete;

    // Call once per frame, between ImGui::NewFrame() and ImGui::Render(). Fills the whole viewport.
    void Draw() {
        for (const auto& tab : tabs_) {
            tab->Poll();
        }
        PollModels();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("##chat", nullptr, flags);
        DrawHeader();
        ImGui::Separator();
        DrawTabs();
        ImGui::End();

        DrawConfirmation();
        DrawSettings();
        DrawNewAgent();
        DrawDeleteAgent();
    }

private:
    // The Settings window's fields, copied when it opens from the settings of the agent in the tab shown.
    struct SettingsForm {
        int         tab_id               = 0;      // the agent they belong to
        bool        base                 = false;  // it is the default agent, whose settings are every agent's base
        std::string model;
        int         num_ctx              = 0;
        int         num_predict          = 0;
        int         max_tool_rounds      = 0;
        int         autonomous_max_steps = 0;
        int         kb_max_passages      = 0;
        int         kb_max_result_chars  = 0;
        std::string instructions;                  // an agent made in the window
        int         embed_num_ctx        = 0;      // the same for every agent: shown with the default agent
        bool        saved_before         = false;  // this computer's settings file existed when the window opened
        std::string error;                         // why the last Save did not save

        static SettingsForm Of(const ChatAgentConfig& config) {
            SettingsForm f;
            f.model                = config.model;
            f.num_ctx              = config.num_ctx;
            f.num_predict          = config.num_predict;
            f.max_tool_rounds      = config.max_tool_rounds;
            f.autonomous_max_steps = config.autonomous_max_steps;
            f.kb_max_passages      = static_cast<int>(config.kb_max_passages);
            f.kb_max_result_chars  = static_cast<int>(config.kb_max_result_chars);
            f.instructions         = ui::Trim(config.instructions);
            f.embed_num_ctx        = config.embed_num_ctx;
            return f;
        }

        // The settings an agent can have its own of, as its settings file holds them. The model is first.
        ChatAgentConfig::Values AgentValues() const {
            return {
                {"model",                ui::Trim(model)},
                {"num_ctx",              std::to_string(num_ctx)},
                {"num_predict",          std::to_string(num_predict)},
                {"max_tool_rounds",      std::to_string(max_tool_rounds)},
                {"autonomous_max_steps", std::to_string(autonomous_max_steps)},
                {"kb_max_passages",      std::to_string(kb_max_passages)},
                {"kb_max_result_chars",  std::to_string(kb_max_result_chars)},
            };
        }
    };

    // The models pulled in Ollama, asked for when New agent or Settings opens.
    struct ModelList {
        std::vector<std::string> names;
        std::string              error;
    };

    // The New agent window's fields.
    struct NewAgentForm {
        std::string name;
        std::string model;
        std::string instructions;
        std::string error;      // why the last Create did not create
    };

    // WARNING: UI thread only.
    std::vector<std::unique_ptr<AgentTab>> tabs_;           // the default agent first; never empty
    size_t                                 current_ = 0;    // the tab shown
    int                                    next_id_ = 1;
    AgentTab::Question                     confirm_;        // the question in the confirmation popup
    int                                    confirm_tab_ = 0;  // the id of the tab that asks it
    std::string                            confirm_name_;     // and its agent's name
    SettingsForm                           settings_;
    SettingsForm                           settings_base_;  // the default agent's, to compare an agent's with
    bool                                   open_settings_ = false;
    NewAgentForm                           new_agent_;
    bool                                   open_new_agent_ = false;
    ModelList                              models_;         // for the model lists of New agent and Settings
    std::future<ModelList>                 model_list_;
    bool                                   listing_models_ = false;  // asked for, and not come back yet
    int                                    delete_id_ = 0;  // the tab whose agent Delete agent asks about
    std::string                            delete_error_;
    bool                                   open_delete_ = false;

    static constexpr const char* kConfirmPopup  = "Run this code?###confirm";
    static constexpr const char* kSettingsPopup = "Settings###settings";
    static constexpr const char* kNewAgentPopup = "New agent###new_agent";
    static constexpr const char* kDeletePopup   = "Delete agent###delete_agent";

    void AddTab(ChatAgentConfig config) {
        tabs_.push_back(std::make_unique<AgentTab>(std::move(config), next_id_++));
    }

    AgentTab& Current() { return *tabs_[std::min(current_, tabs_.size() - 1)]; }

    AgentTab* Find(int id) {
        for (const auto& tab : tabs_) {
            if (tab->Id() == id) {
                return tab.get();
            }
        }
        return nullptr;
    }

    int Working() const {
        return static_cast<int>(std::count_if(tabs_.begin(), tabs_.end(), [](const auto& tab) { return tab->Busy(); }));
    }

    // Places a modal popup in the middle of the window, `width` wide.
    static void CenterNextPopup(float width) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(viewport->WorkSize.x * 0.85f, width), 0.0f), ImGuiCond_Appearing);
    }

    void DrawHeader() {
        const ImGuiStyle& style   = ImGui::GetStyle();
        AgentTab&         tab     = Current();
        const int         working = Working();

        const std::string title = "Agent Chat " + tab.Title();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(title.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s  |  %d agent(s), %d working", tab.Settings().endpoint.c_str(),
                            static_cast<int>(tabs_.size()), working);

        const std::string status   = tab.Status();
        const char*       new_chat = "New chat";
        const char*       settings = "Settings";
        const float right_w = ImGui::CalcTextSize(status.c_str()).x + style.ItemSpacing.x * 2.0f +
                              ImGui::CalcTextSize(new_chat).x + ImGui::CalcTextSize(settings).x +
                              style.FramePadding.x * 4.0f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - right_w));
        ImGui::TextColored(tab.Busy() ? ui::kBusyColor : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s",
                           status.c_str());
        ImGui::SameLine();

        ImGui::BeginDisabled(tab.Busy());
        if (ImGui::Button(new_chat)) {
            tab.NewConversation();
        }
        ImGui::EndDisabled();
        if (tab.Busy() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Wait for the current reply first.");
        }
        ImGui::SameLine();
        // The default agent's settings are every agent's base, so they wait for all of them.
        const bool wait = tab.Made() ? tab.Busy() : working > 0;
        ImGui::BeginDisabled(wait);
        if (ImGui::Button(settings)) {
            OpenSettings();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", !wait       ? "The settings of this agent."
                                    : tab.Made() ? "Wait for this agent to finish first."
                                                 : "Wait for every agent to finish first: the default agent's "
                                                   "settings are the base of every agent.");
        }
    }

    // One tab per agent, and + for a new one.
    void DrawTabs() {
        const ImGuiTabBarFlags flags = ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll;
        if (!ImGui::BeginTabBar("##agents", flags)) {
            return;
        }
        for (size_t i = 0; i < tabs_.size(); ++i) {
            AgentTab&         tab   = *tabs_[i];
            const std::string state = tab.WaitingQuestion() ? " (asks you)" : tab.Busy() ? " (working)" : "";
            const std::string label = tab.Name() + state + "###agent" + std::to_string(tab.Id());
            if (ImGui::BeginTabItem(label.c_str())) {
                current_ = i;
                if (tab.DrawBody() == AgentTab::Action::Delete) {
                    delete_id_   = tab.Id();
                    open_delete_ = true;
                }
                ImGui::EndTabItem();
            }
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
            OpenNewAgent();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("New agent");
        }
        ImGui::EndTabBar();
    }

    /*
     * The question an agent's worker is waiting on (Session::Confirm), as a modal popup: the rest of the window
     * is locked until the user answers. With several agents asking, the popup shows one question at a time, in
     * tab order.
     *
     * NOTE: "Don't run" ("Don't write" for a file) has the keyboard focus, so Enter refuses, and so does Esc.
     */
    void DrawConfirmation() {
        AgentTab*  shown   = Find(confirm_tab_);
        const bool showing = ImGui::IsPopupOpen(kConfirmPopup) && shown && shown->WaitingQuestion() == confirm_.id;
        const bool other   = ImGui::IsPopupOpen(kSettingsPopup) || ImGui::IsPopupOpen(kNewAgentPopup) ||
                             ImGui::IsPopupOpen(kDeletePopup);
        if (!showing && !other) {
            for (const auto& tab : tabs_) {
                const int question = tab->WaitingQuestion();
                if (question && (tab->Id() != confirm_tab_ || question != confirm_.id)) {
                    confirm_      = tab->CopyQuestion();
                    confirm_tab_  = tab->Id();
                    confirm_name_ = tab->Name();
                    shown         = tab.get();
                    ImGui::OpenPopup(kConfirmPopup);
                    break;
                }
            }
        }

        CenterNextPopup(ImGui::GetMainViewport()->WorkSize.x);
        if (!ImGui::BeginPopupModal(kConfirmPopup, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }
        if (!shown || shown->WaitingQuestion() != confirm_.id) {
            ImGui::CloseCurrentPopup();  // answered elsewhere: Stop, or the agent was closed
            ImGui::EndPopup();
            return;
        }

        const bool   danger = confirm_.report.AnyDanger();
        const bool   file    = !confirm_.report.file.empty();  // write_file: the "code" is the file's content
        const bool   deletes = file && confirm_.report.deletes;  // delete_file: no content
        const ImVec4 dim     = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        std::string  head;
        if (file) {
            head = "The agent " + confirm_name_ + " wants to " + (deletes ? "delete" : "write") + " the file " +
                   confirm_.report.file + (danger ? ", which could HARM this computer." : ".");
        } else {
            head = danger ? "The agent " + confirm_name_ + " wants to run " + confirm_.language +
                                " code that could HARM this computer."
                          : "The agent " + confirm_name_ + " wants to run " + confirm_.language +
                                " code that changes this computer.";
        }
        ui::Wrapped(head, danger ? &ui::kErrorColor : &ui::kBusyColor);
        ImGui::Spacing();

        ImGui::TextUnformatted("Why it was flagged:");
        for (const CLISecurityChecking::Finding& f : confirm_.report.findings) {
            const bool is_danger = f.severity == CLISecurityChecking::Severity::Danger;
            ImGui::TextColored(is_danger ? ui::kErrorColor : ui::kBusyColor, "%s",
                               CLISecurityChecking::SeverityName(f.severity));
            ImGui::SameLine();
            std::string harm = f.harm;
            if (std::string(f.ecosystem) != "Any") {
                harm += std::string(" (") + f.ecosystem + ")";
            }
            ui::Wrapped(harm);
            std::string where = (f.line > 0 ? "line " + std::to_string(f.line) + ": " : std::string()) + f.evidence;
            if (!f.more_lines.empty()) {
                where += " (and " + std::to_string(f.more_lines.size()) + " more)";
            }
            ImGui::Indent();
            ui::Wrapped(where, &dim);
            ImGui::Unindent();
        }
        ImGui::Spacing();

        if (!deletes) {
            ImGui::TextUnformatted(file ? "The file's content:" : "The code:");
            const float line = ImGui::GetTextLineHeight();
            ImGui::InputTextMultiline("##code", &confirm_.code, ImVec2(-FLT_MIN, line * 12.0f), ImGuiInputTextFlags_ReadOnly);
            ImGui::Spacing();
        }

        bool answered = false;
        bool allow    = false;
        if (ImGui::Button(deletes ? "Don't delete" : file ? "Don't write" : "Don't run")) {
            answered = true;
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, danger ? ImVec4(0.55f, 0.15f, 0.15f, 1.0f) : ImVec4(0.45f, 0.35f, 0.10f, 1.0f));
        if (ImGui::Button(deletes ? "Delete it" : file ? "Write it" : "Run it")) {
            answered = true;
            allow    = true;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            answered = true;
            allow    = false;
        }

        if (answered) {
            shown->Answer(allow);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Fills the Settings window with the settings of the agent in the tab shown, and opens it.
    void OpenSettings() {
        const AgentTab& tab = Current();
        std::error_code ec;
        settings_              = SettingsForm::Of(tab.Settings());
        settings_.tab_id       = tab.Id();
        settings_.base         = !tab.Made();
        settings_.saved_before = std::filesystem::exists(std::filesystem::u8path(tab.Settings().machine_settings), ec);
        settings_base_         = SettingsForm::Of(tabs_.front()->Settings());
        ListModels();
        open_settings_ = true;
    }

    /*
     * The Settings window, as a modal popup, for the agent in the tab shown. Save checks the values, saves them
     * and gives them to the agents they change, from their next message on (SaveSettings); Cancel and Esc close
     * it and change nothing. For an agent made in the window, each number says whether it is the agent's own
     * or the default agent's.
     */
    void DrawSettings() {
        if (open_settings_) {
            ImGui::OpenPopup(kSettingsPopup);
            open_settings_ = false;
        }
        CenterNextPopup(ImGui::GetFontSize() * 48.0f);
        if (!ImGui::BeginPopupModal(kSettingsPopup, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }
        AgentTab* tab = Find(settings_.tab_id);
        if (!tab) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const ChatAgentConfig& config = tab->Settings();
        SettingsForm&          f      = settings_;
        const SettingsForm*    base   = f.base ? nullptr : &settings_base_;  // what the agent follows

        ui::Wrapped("Settings of " + tab->Name());
        if (f.base) {
            ui::Dim("The default agent. Its settings are the base of every agent: an agent made in the window uses "
                    "them, except the ones it has its own of. " +
                    (f.saved_before
                         ? "Saved for this computer (" + config.machine_name + ") in " + config.machine_settings +
                               "; a setting not saved there comes from config\\settings.txt."
                         : "Nothing saved for this computer (" + config.machine_name + ") yet: these come from "
                               "config\\settings.txt, or the defaults built into the program. Save writes " +
                               config.machine_settings + "."));
        } else {
            ui::Dim("Saved in " + ui::ShortPath(config.agent_file) + ". A number left the same as the default "
                    "agent's is not saved there, so this agent follows the default agent's when it changes.");
        }

        ImGui::SeparatorText("Model");
        DrawModelChoice(f.model);
        ui::Dim("When agents with different models, or the same model with a different context window, take "
                "turns, Ollama loads the model again, which takes a while on a small machine.");
        ImGui::Spacing();

        ImGui::SeparatorText("Context and replies");
        NumberField("Context window (num_ctx)", f.num_ctx, 1024,
                    "Tokens the model reads at once: system prompt, tools, conversation, search results and the "
                    "reply. Bigger keeps more of the conversation, but needs more memory and is slower.",
                    base ? &base->num_ctx : nullptr);
        NumberField("Longest reply (num_predict)", f.num_predict, 256,
                    "Tokens one reply may take, kept free of the conversation. Smaller than the context window.",
                    base ? &base->num_predict : nullptr);

        ImGui::SeparatorText("Tools");
        NumberField("Tool rounds per message (max_tool_rounds)", f.max_tool_rounds, 1,
                    "Rounds of tool calls for one message before the agent must answer with what it found.",
                    base ? &base->max_tool_rounds : nullptr);
        NumberField("Run task steps (autonomous_max_steps)", f.autonomous_max_steps, 1,
                    "Most steps Run task takes on one goal. Each step is up to the rounds above; on a slow machine "
                    "a step takes about a minute.",
                    base ? &base->autonomous_max_steps : nullptr);

        ImGui::SeparatorText("Knowledge base");
        NumberField("Passages per search (kb_max_passages)", f.kb_max_passages, 1,
                    "Passages one knowledge base search returns, at most.",
                    base ? &base->kb_max_passages : nullptr);
        NumberField("Characters per search (kb_max_result_chars)", f.kb_max_result_chars, 500,
                    "Room one search may take in the context (about 3 characters per token). More passages need "
                    "more room.",
                    base ? &base->kb_max_result_chars : nullptr);

        if (f.base) {
            ImGui::SeparatorText("Every agent");
            NumberField("Embedding context (embed_num_ctx)", f.embed_num_ctx, 256,
                        "Context of the embedding model that searches the knowledge base by meaning, the same for "
                        "every agent: Ollama loads it again when it changes. Passages are about 300 tokens.");
        } else {
            ImGui::SeparatorText("Instructions");
            ui::Dim("What this agent is for and how it should answer. They are added to its system prompt, after "
                    "the general rules and its name.");
            ImGui::InputTextMultiline("##instructions", &f.instructions,
                                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 6.0f));
        }
        ImGui::Spacing();
        ui::Dim(f.base ? "Applies from each agent's next message, keeping the conversations."
                       : "Applies from this agent's next message, keeping the conversation.");

        if (!f.error.empty()) {
            ImGui::Spacing();
            ui::Wrapped(f.error, &ui::kErrorColor);
        }
        ImGui::Spacing();

        bool close = false;
        if (ImGui::Button("Save")) {
            close = SaveSettings(*tab);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            close = true;
        }
        if (close) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /*
     * One number of the Settings window, with what it means under it. With `base` (an agent made in the window),
     * also whether it is the agent's own or the default agent's.
     */
    static void NumberField(const char* label, int& value, int step, const char* help, const int* base = nullptr) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::InputInt(label, &value, step, step * 4);
        std::string text = help;
        if (base) {
            text += value == *base ? " Same as the default agent's: it follows it."
                                   : " This agent's own; the default agent has " + std::to_string(*base) + ".";
        }
        ui::Dim(text);
        ImGui::Spacing();
    }

    /*
     * Checks the Settings window's values with the rules Load() uses and saves them. The default agent's go to
     * this computer's settings file and to every agent, since they are the others' base; an agent made in the
     * window keeps in its file the model, its instructions and only the numbers that differ from the default
     * agent's. Every agent changed then reads its settings back from the files, as at the next start. False,
     * with the reason in settings_.error, when it did not save.
     */
    bool SaveSettings(AgentTab& tab) {
        SettingsForm& f = settings_;
        f.model = ui::Trim(f.model);
        if (f.model.empty() || f.model.find_first_of(" \t") != std::string::npos) {
            f.error = "Choose the model the agent uses.";
            return false;
        }
        ChatAgentConfig::Values values = f.AgentValues();
        if (f.base) {
            values.push_back({"embed_num_ctx", std::to_string(f.embed_num_ctx)});
        }
        ChatAgentConfig check = tab.Settings();
        for (const auto& kv : values) {
            const std::string error = check.Apply(kv.first, kv.second);
            if (!error.empty()) {
                f.error = kv.first + ": " + error + ".";
                return false;
            }
        }
        if (f.num_predict >= f.num_ctx) {
            f.error = "The longest reply (num_predict) must be smaller than the context window (num_ctx).";
            return false;
        }
        for (const auto& t : tabs_) {
            if ((f.base || t.get() == &tab) && t->Busy()) {
                f.error = "Wait for " + t->Name() + " to finish first.";
                return false;
            }
        }

        std::string error;
        std::string where;
        if (f.base) {
            error = tab.Settings().SaveMachineSettings(values);
            where = tab.Settings().machine_settings;
        } else {
            const ChatAgentConfig::Values base = settings_base_.AgentValues();  // in the same order
            ChatAgentConfig::Values       own;
            std::vector<std::string>      follow;
            for (size_t i = 0; i < values.size(); ++i) {
                // The model is always the agent's own, as New agent wrote it.
                if (values[i].first == "model" || values[i].second != base[i].second) {
                    own.push_back(values[i]);
                } else {
                    follow.push_back(values[i].first);
                }
            }
            error = tab.Settings().SaveAgentSettings(own, follow, f.instructions);
            where = ui::ShortPath(tab.Settings().agent_file);
        }
        if (!error.empty()) {
            f.error = "Not saved: " + error + ".";
            return false;
        }

        for (const auto& t : tabs_) {
            if (f.base || t.get() == &tab) {
                t->ApplySettings(ChatAgentConfig::Load(std::filesystem::u8path(t->Settings().agent_file)));
            }
        }
        tab.Note("[Settings of " + tab.Name() + " saved in " + where +
                 (f.base ? ", the base of every agent" : "") + ". They apply from the next message.]");
        return true;
    }

    // Asks Ollama for its models in the background, for the model lists of New agent and Settings.
    void ListModels() {
        if (listing_models_) {
            return;  // the answer to the last request is still coming
        }
        listing_models_ = true;
        model_list_ = std::async(std::launch::async, [endpoint = tabs_.front()->Settings().endpoint] {
            ModelList list;
            list.error = OllamaClient(endpoint).ListModels(list.names);
            return list;
        });
    }

    // Takes the list of models once it has come back. Every frame.
    void PollModels() {
        if (listing_models_ && model_list_.valid() &&
            model_list_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            models_         = model_list_.get();
            listing_models_ = false;
        }
    }

    // Empties the New agent window, asks Ollama for its models, and opens the window.
    void OpenNewAgent() {
        new_agent_       = NewAgentForm();
        new_agent_.model = tabs_.front()->Settings().model;
        ListModels();
        open_new_agent_ = true;
    }

    /*
     * The New agent window, as a modal popup: a name, a model and instructions. Create saves the agent in
     * config\agents\ and opens its tab; Cancel and Esc close it and make nothing.
     */
    void DrawNewAgent() {
        if (open_new_agent_) {
            ImGui::OpenPopup(kNewAgentPopup);
            open_new_agent_ = false;
        }
        CenterNextPopup(ImGui::GetFontSize() * 48.0f);
        if (!ImGui::BeginPopupModal(kNewAgentPopup, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }

        ui::Dim("A new agent has its own name, model and instructions, its own conversation and its own Run task, "
                "and works at the same time as the others. It is saved in config\\agents\\ and opens again at the "
                "next start. It starts with the default agent's settings; its Settings changes them for it alone.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
        ImGui::InputTextWithHint("Name", "e.g. Researcher", &new_agent_.name);
        ImGui::Spacing();
        DrawModelChoice(new_agent_.model);
        ImGui::Spacing();

        ImGui::TextUnformatted("Instructions");
        ui::Dim("What this agent is for and how it should answer. They are added to its system prompt, after the "
                "general rules and its name.");
        const float line = ImGui::GetTextLineHeight();
        ImGui::InputTextMultiline("##instructions", &new_agent_.instructions, ImVec2(-FLT_MIN, line * 8.0f));

        if (!new_agent_.error.empty()) {
            ImGui::Spacing();
            ui::Wrapped(new_agent_.error, &ui::kErrorColor);
        }
        ImGui::Spacing();

        bool close = false;
        if (ImGui::Button("Create")) {
            close = CreateAgent();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            close = true;
        }
        if (close) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The model: a list of the models pulled in Ollama when it answered, a text box otherwise.
    void DrawModelChoice(std::string& model) {
        const std::string embed = tabs_.front()->Settings().embed_model;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
        if (models_.names.empty()) {
            ImGui::InputText("Model", &model);
            ui::Dim(listing_models_ ? "Looking for the models installed in Ollama..."
                    : models_.error.empty()
                        ? "Ollama has no models pulled. Type the name of one to pull (ollama pull <model>)."
                        : "Could not list the models installed in Ollama (" + models_.error + "). Type the name of "
                          "one, as ollama list shows it.");
            return;
        }
        if (ImGui::BeginCombo("Model", model.c_str())) {
            for (const std::string& name : models_.names) {
                if (SameModel(name, embed)) {
                    continue;  // the embedding model cannot chat
                }
                const bool selected = SameModel(name, model);
                if (ImGui::Selectable(name.c_str(), selected)) {
                    model = name;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ui::Dim("The models installed in Ollama. The agent's tools need a model with the 'tools' capability.");
    }

    // "llama3.2" and "llama3.2:latest" are the same model in Ollama.
    static bool SameModel(const std::string& a, const std::string& b) {
        const auto full = [](const std::string& m) { return m.find(':') == std::string::npos ? m + ":latest" : m; };
        return full(a) == full(b);
    }

    static bool SameName(const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                   return std::tolower(x) == std::tolower(y);
               });
    }

    // Checks the New agent window, saves the agent and opens its tab. False, with the reason, when it did not.
    bool CreateAgent() {
        const std::string name  = ui::Trim(new_agent_.name);
        const std::string model = ui::Trim(new_agent_.model);
        std::string&      error = new_agent_.error;
        if (name.empty()) {
            error = "Give the agent a name.";
            return false;
        }
        if (name.size() > 40 || name.find("##") != std::string::npos) {
            error = "Use a shorter name, without ##.";
            return false;
        }
        for (const auto& tab : tabs_) {
            if (SameName(tab->Name(), name)) {
                error = "There is already an agent named " + tab->Name() + ".";
                return false;
            }
        }
        if (model.empty() || model.find_first_of(" \t") != std::string::npos) {
            error = "Choose the model the agent uses.";
            return false;
        }
        std::string why;
        const std::filesystem::path file = ChatAgentConfig::CreateAgentFile(name, model, new_agent_.instructions, why);
        if (file.empty()) {
            error = "Not created: " + why + ".";
            return false;
        }
        AddTab(ChatAgentConfig::Load(file));  // ImGuiTabBarFlags_AutoSelectNewTabs shows it
        return true;
    }

    /*
     * Asks before deleting an agent made in the window: Delete removes its file (a copy goes to config\backup\)
     * and closes its tab; Cancel and Esc keep it.
     */
    void DrawDeleteAgent() {
        if (open_delete_) {
            ImGui::OpenPopup(kDeletePopup);
            open_delete_ = false;
            delete_error_.clear();
        }
        CenterNextPopup(ImGui::GetFontSize() * 40.0f);
        if (!ImGui::BeginPopupModal(kDeletePopup, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }
        AgentTab* tab = Find(delete_id_);
        if (!tab) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        ui::Wrapped("Delete the agent " + tab->Name() + "?");
        ui::Dim("Its file, " + ui::ShortPath(tab->Settings().agent_file) + ", and its life, " +
                ui::ShortPath(tab->LifeFile().u8string()) + ", are removed, and copies go to config\\backup\\. Its "
                "conversation is lost. The other agents are not changed.");
        if (!delete_error_.empty()) {
            ImGui::Spacing();
            ui::Wrapped(delete_error_, &ui::kErrorColor);
        }
        ImGui::Spacing();

        bool close = false;
        ImGui::BeginDisabled(tab->Busy());
        if (ImGui::Button("Delete")) {
            close = DeleteAgent(delete_id_);  // WARNING: `tab` dangles when it returns true
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            close = true;
        }
        if (close) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Removes the agent's file and its tab. False, with the reason in delete_error_, when it did not.
    bool DeleteAgent(int id) {
        AgentTab* tab = Find(id);
        if (!tab || !tab->Made() || tab->Busy()) {
            return false;
        }
        const std::string error = ChatAgentConfig::DeleteAgentFile(std::filesystem::u8path(tab->Settings().agent_file));
        if (!error.empty()) {
            delete_error_ = "Not deleted: " + error + ".";
            return false;
        }
        const std::filesystem::path life = tab->LifeFile();
        tabs_.erase(std::remove_if(tabs_.begin(), tabs_.end(), [id](const auto& t) { return t->Id() == id; }),
                    tabs_.end());
        /*
         * After the tab is gone, since it saves the life on its way out; a new agent given the same name then
         * starts a life of its own.
         *
         * NOTE: a failure is not reported: the agent is deleted already, and the life left behind only means a
         * new agent of the same name would go on with it.
         */
        ChatAgentConfig::MoveToBackup(life, "life");
        current_ = 0;
        return true;
    }
};
