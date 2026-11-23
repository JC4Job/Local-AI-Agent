#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include "agent/AutonomousAgent.hpp"
#include "agent/ChatAgent.hpp"
#include "bio/EndocrineSystem.hpp"
#include "gui/LifePanel.hpp"
#include "gui/UiCommon.hpp"

/*
 * AgentTab - one agent in the window, drawn in its own tab: a ChatAgent for the chat and an AutonomousAgent for
 * Run task, with their conversation, the message being typed and a worker thread. ChatWindow holds one per agent
 * and draws what is no one agent's: the tab bar, Settings, New agent and the code confirmation.
 *
 * ChatAgent::Ask() blocks until the model answers, so each prompt runs on the tab's worker thread and the
 * window keeps repainting meanwhile. Every tab has its own worker, so agents work at the same time.
 *
 * Run task sends the message to the AutonomousAgent instead, as a goal it works on alone, step by step, until
 * it calls finish (AutonomousAgent.hpp). Each step's note is kept as the step ends, and the result comes last.
 * The task agent is a second ChatAgent with its own conversation: the chat does not see the tasks, and a task
 * does not see the chat.
 *
 * WARNING: the worker never touches UI state. It posts events (tool activity, pieces of the reply as the model
 * writes them, step notes, the final reply) to an inbox, and Poll() applies them on the UI thread every frame,
 * for every tab, so a tab in the background still shows when its agent is done.
 *
 * WARNING: code that CLISecurityChecking flags waits for the user: the worker blocks in Session::Confirm until
 * ChatWindow's popup calls Answer(). Stop and closing the window answer no, or the worker would wait forever.
 *
 * Each agent has a body (bio/EndocrineSystem.hpp), shared by its chat and its tasks: both feel what happens as
 * they work and send each message in the mood the body is in. It lives as the window runs (Live, every frame),
 * its feeling is shown above the conversation and its life under a header there (gui/LifePanel.hpp), and it is
 * saved in data\life\<agent name>.txt every half minute, after each turn and when the tab closes, so the next
 * start goes on with the same life.
 *
 * WARNING: the body is the one thing both threads touch directly: it is under the session's mutex. The worker
 * feels and reads the mood there; the UI thread makes it live and draws a copy taken each frame (body_view_).
 */
class AgentTab {
public:
    // The code confirmation the worker waits on, copied for ChatWindow's popup.
    struct Question {
        int                         id = 0;  // counts the tab's questions, so the popup sees a new one
        std::string                 language;
        std::string                 code;
        CLISecurityChecking::Report report;
    };

    // What the user asked for in the tab that ChatWindow carries out.
    enum class Action { None, Delete };

    /*
     * `config`: ChatAgentConfig::Load(), of the default agent or of one made in the window. `id`: unique in the
     * window, for the tab's ImGui ID.
     */
    AgentTab(ChatAgentConfig config, int id) : id_(id), session_(std::make_shared<Session>()) {
        Session* session = session_.get();  // the agents live inside the session, so this never dangles
        const ChatAgent::ActivityHandler on_activity = [session](const std::string& line) {
            session->Post(Event::Activity, line);
        };
        const ChatAgent::StreamHandler on_stream = [session](const std::string& piece, bool first) {
            session->Post(Event::Delta, piece, first);
        };
        const ChatAgent::ConfirmHandler on_confirm =
            [session](const std::string& language, const std::string& code, const CLISecurityChecking::Report& report) {
                return session->Confirm(language, code, report);
            };
        const ChatAgent::FeelingHandler on_feeling = [session](Feeling feeling) { session->Feel(feeling); };
        const ChatAgent::MoodSource     mood       = [session] { return session->CurrentMood(); };

        name_           = config.agent_name;
        task_max_steps_ = config.autonomous_max_steps;
        // No worker yet, so no lock: the life it lived before, rested for the time the program was closed.
        life_file_  = config.LifeFile();
        life_found_ = session_->body.Load(life_file_);
        body_view_  = session_->body;
        // The task agent's copy has no notes: the chat agent shows them already.
        ChatAgent::Config task_config = config;
        task_config.notes.clear();
        config.notes.push_back((life_found_ ? "Life of " + name_ + " from " : "A new life for " + name_ + ", kept in ") +
                               life_file_.u8string());

        session_->agent = std::make_unique<ChatAgent>(on_activity, std::move(config));
        session_->agent->SetStreamHandler(on_stream);
        session_->agent->SetConfirmHandler(on_confirm);
        session_->agent->SetFeelingHandler(on_feeling);
        session_->agent->SetMoodSource(mood);

        session_->task = std::make_unique<AutonomousAgent>(on_activity, std::move(task_config));
        session_->task->SetStreamHandler(on_stream);
        session_->task->SetConfirmHandler(on_confirm);
        session_->task->SetFeelingHandler(on_feeling);
        session_->task->SetMoodSource(mood);
        session_->task->SetStepHandler([session, name = name_](int step, const std::string& note) {
            Event e{Event::StepNote, note};
            e.title = name + " (step " + std::to_string(step) + ")";
            session->Post(std::move(e));
        });
    }

    ~AgentTab() {
        SaveLife();  // as it is now: a worker still busy is let go below, and what it feels after is lost
        session_->Answer(false);  // WARNING: a worker waiting for a confirmation would otherwise wait forever
        if (!worker_.joinable()) {
            return;
        }
        /*
         * NOTE: a worker still waiting on the model can take minutes to return. It holds its own reference to
         * the session, so let it finish on its own; the process is exiting anyway.
         */
        if (busy_) {
            worker_.detach();
        } else {
            worker_.join();
        }
    }

    AgentTab(const AgentTab&)             = delete;
    AgentTab& operator=(const AgentTab&)  = delete;

    int                    Id()       const { return id_; }
    const std::string&     Name()     const { return name_; }
    // The name and the version, for the window's title.
    std::string            Title()    const { return session_->agent->Name(); }
    const ChatAgentConfig& Settings() const { return session_->agent->Settings(); }
    bool                   Busy()     const { return busy_; }
    // Made in the window, with a file of its own; the default agent has none.
    bool                   Made()     const { return !Settings().agent_file.empty(); }
    // Where the agent's life is saved; see ChatAgentConfig::LifeFile.
    const std::filesystem::path& LifeFile() const { return life_file_; }

    // What the agent is doing, for the window's header.
    std::string Status() const {
        if (!busy_) {
            return "Ready";
        }
        if (WaitingQuestion()) {
            return "Waiting for your answer";
        }
        const std::string seconds = std::to_string(BusySeconds()) + " s";
        if (stopping_) {
            return "Stopping... " + seconds;
        }
        return (task_ ? "Working on the task... " : "Thinking... ") + seconds;
    }

    /*
     * Forgets the conversation (New chat).
     *
     * WARNING: not while Busy(): the worker is using the agent.
     */
    void NewConversation() {
        messages_.clear();
        draft_.clear();
        session_->agent->NewConversation();
        focus_input_ = true;
    }

    /*
     * Gives the chat and Run task the settings the Settings window saved; see ChatAgent::ApplySettings.
     *
     * WARNING: not while Busy(): the worker is using the agents.
     */
    void ApplySettings(const ChatAgentConfig& config) {
        session_->agent->ApplySettings(config);
        session_->task->ApplySettings(config);
        task_max_steps_ = config.autonomous_max_steps;
    }

    // Adds a line to the tab's activity log, e.g. that the settings were saved.
    void Note(std::string text) {
        AddActivity(std::move(text));
        scroll_to_bottom_ = true;
    }

    /*
     * Applies what the worker posted, and the agent lives on by the time since the last frame (Live).
     *
     * WARNING: call it every frame, for every tab, shown or not, or a tab in the background never shows that
     * its agent is done, and its agent does not live.
     */
    void Poll() {
        DrainInbox();
        Live();
    }

    // The id of the question the worker waits on, 0 when it waits on none.
    int WaitingQuestion() const {
        std::lock_guard<std::mutex> lock(session_->mu);
        return session_->confirm_waiting ? session_->confirm_id : 0;
    }

    Question CopyQuestion() const {
        std::lock_guard<std::mutex> lock(session_->mu);
        return {session_->confirm_id, session_->confirm_language, session_->confirm_code, session_->confirm_report};
    }

    // Answers the question the worker waits on. Does nothing when it waits on none.
    void Answer(bool allow) { session_->Answer(allow); }

    // Draws the tab's content: which agent it is, the conversation and the input. Inside the tab item.
    Action DrawBody() {
        const ImGuiStyle& style   = ImGui::GetStyle();
        const float       line    = ImGui::GetTextLineHeight();
        const float       input_h = line * 3.0f + style.FramePadding.y * 2.0f;

        const Action action = DrawAbout();
        ui::DrawLife(body_view_, name_);
        DrawHistory(-(input_h + line + style.ItemSpacing.y * 2.0f));
        DrawInput(input_h);
        return action;
    }

private:
    using Clock = std::chrono::steady_clock;

    struct Event {
        enum Kind { Activity, Delta, Reply, StepNote, TaskEnd };
        Kind        kind;
        std::string text;
        bool        first = false;  // Delta: the first piece of a call to the model; it replaces the draft
        std::string title;          // StepNote, TaskEnd: the line shown above the text
        bool        ok    = false;  // TaskEnd: the model called finish
    };

    // Shared between the UI thread and the worker.
    struct Session {
        std::mutex                       mu;
        std::vector<Event>               inbox;
        std::unique_ptr<ChatAgent>       agent;
        std::unique_ptr<AutonomousAgent> task;   // Run task; see RunTask

        void Post(Event event) {
            std::lock_guard<std::mutex> lock(mu);
            inbox.push_back(std::move(event));
        }

        void Post(Event::Kind kind, std::string text, bool first = false) {
            Post(Event{kind, std::move(text), first});
        }

        /*
         * The code confirmation the worker is waiting for. The worker fills it in and waits; the UI thread shows
         * it and answers.
         *
         * WARNING: guarded by `mu`.
         */
        std::condition_variable     confirm_cv;
        int                         confirm_id      = 0;      // counts the questions, so the UI sees a new one
        bool                        confirm_waiting = false;  // a question is waiting for its answer
        bool                        confirm_allowed = false;
        std::string                 confirm_language;
        std::string                 confirm_code;
        CLISecurityChecking::Report confirm_report;

        // Worker thread: blocks until the user answers, Stop is pressed or the window closes (both answer no).
        bool Confirm(const std::string& language, const std::string& code, const CLISecurityChecking::Report& report) {
            std::unique_lock<std::mutex> lock(mu);
            ++confirm_id;
            confirm_waiting  = true;
            confirm_allowed  = false;
            confirm_language = language;
            confirm_code     = code;
            confirm_report   = report;
            confirm_cv.wait(lock, [this] { return !confirm_waiting; });
            return confirm_allowed;
        }

        // Any thread. Does nothing when no question is waiting.
        void Answer(bool allow) {
            {
                std::lock_guard<std::mutex> lock(mu);
                if (!confirm_waiting) {
                    return;
                }
                confirm_waiting = false;
                confirm_allowed = allow;
            }
            confirm_cv.notify_all();
        }

        /*
         * The agent's body: the worker feels what happens as it works and reads the mood of each message; the
         * UI thread makes it live and copies it to draw.
         *
         * WARNING: guarded by `mu`.
         */
        EndocrineSystem body;

        // Worker thread: something happened to the agent.
        void Feel(Feeling feeling) {
            std::lock_guard<std::mutex> lock(mu);
            body.Feel(feeling);
        }

        // Worker thread: the mood the next message goes to the model in.
        Mood CurrentMood() {
            std::lock_guard<std::mutex> lock(mu);
            return body.CurrentMood();
        }
    };

    enum class Role { User, Agent, Step, Activity, Error };
    struct Message {
        Role               role;
        std::string        text;
        std::string        title;      // the line above the text, when it is not the role's usual one
        bool               ok = true;  // Agent: false draws the title as a warning (a task that ended without finish)
        ui::SelectableText view;       // draws `text` so it can be selected and copied
    };

    // WARNING: UI thread only.
    int                      id_;
    std::string              name_;
    std::shared_ptr<Session> session_;
    std::thread              worker_;
    std::vector<Message>     messages_;
    std::string              draft_;  // the reply being written, shown until the final reply arrives
    std::string              input_;
    bool                     busy_             = false;
    bool                     stopping_         = false;  // Stop pressed; waiting for the agent to return
    bool                     task_             = false;  // the turn under way is a task (Run task), not a chat reply
    int                      task_max_steps_   = 0;      // autonomous_max_steps, for the Run task tooltip
    Clock::time_point        busy_since_;
    bool                     scroll_to_bottom_ = false;
    bool                     focus_input_      = true;
    std::filesystem::path    life_file_;                    // see ChatAgentConfig::LifeFile
    bool                     life_found_       = false;     // it was there when the tab opened
    EndocrineSystem          body_view_;                    // the body as of this frame, to draw; see Live
    Clock::time_point        lived_until_      = Clock::now();
    Clock::time_point        life_saved_at_    = Clock::now();
    bool                     save_life_        = true;      // save at the next frame: a turn ended, or a newborn
    bool                     life_save_failed_ = false;     // reported once, not every half minute

    /*
     * The longest time one frame lives. The window draws nothing while it is minimized, so the frame after
     * lives all of that time; past this, it is a clock jump, not time the agent lived.
     */
    static constexpr double               kMaxLiveSeconds = 12.0 * 3600.0;
    static constexpr std::chrono::seconds kSaveLifeEvery{30};

    ChatAgent& Agent() { return *session_->agent; }

    /*
     * The time since the last frame goes by for the agent's body (EndocrineSystem::Live), and body_view_ takes
     * a copy to draw. The life is saved every kSaveLifeEvery, and at the frame after a turn ends.
     */
    void Live() {
        const Clock::time_point now     = Clock::now();
        const double            seconds = std::min(std::chrono::duration<double>(now - lived_until_).count(),
                                                   kMaxLiveSeconds);
        lived_until_ = now;
        {
            std::lock_guard<std::mutex> lock(session_->mu);
            session_->body.Live(seconds, busy_);
            body_view_ = session_->body;
        }
        if (save_life_ || now - life_saved_at_ >= kSaveLifeEvery) {
            SaveLife();
        }
    }

    // Writes the agent's life to its file, as the body is now. A failure is shown once in the activity log.
    void SaveLife() {
        EndocrineSystem body;
        {
            std::lock_guard<std::mutex> lock(session_->mu);
            body = session_->body;
        }
        save_life_     = false;
        life_saved_at_ = Clock::now();
        if (body.Save(life_file_, name_)) {
            life_save_failed_ = false;
        } else if (!life_save_failed_) {
            life_save_failed_ = true;
            Note("[Could not save the life of " + name_ + " in " + life_file_.u8string() + ": it goes on in "
                 "memory, and the program tries again every half minute]");
        }
    }

    int BusySeconds() const {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - busy_since_).count());
    }

    // Consecutive activity lines of one turn share a single collapsible block.
    void AddActivity(std::string text) {
        if (messages_.empty() || messages_.back().role != Role::Activity) {
            messages_.push_back({Role::Activity, std::move(text)});
        } else {
            messages_.back().text += "\n" + text;
        }
    }

    void DrainInbox() {
        std::vector<Event> events;
        {
            std::lock_guard<std::mutex> lock(session_->mu);
            events.swap(session_->inbox);
        }
        for (Event& e : events) {
            if (e.kind == Event::Activity) {
                AddActivity(std::move(e.text));
            } else if (e.kind == Event::Delta) {
                if (e.first) {
                    draft_ = std::move(e.text);
                } else {
                    draft_ += e.text;
                }
            } else if (e.kind == Event::StepNote) {
                // A step of the task ended: its note replaces the draft it was streamed into.
                draft_.clear();
                if (!ui::IsBlank(e.text)) {
                    messages_.push_back({Role::Step, std::move(e.text), std::move(e.title)});
                }
            } else {
                busy_ = false;
                stopping_ = false;
                task_ = false;
                save_life_ = true;  // what the turn made it feel
                draft_.clear();
                if (e.kind == Event::TaskEnd) {
                    messages_.push_back({Role::Agent, ui::IsBlank(e.text) ? "(The task ended without a result.)"
                                                                          : std::move(e.text),
                                         std::move(e.title), e.ok});
                } else if (e.text.rfind("Error:", 0) == 0) {
                    if (e.text.find("Empty or invalid response") != std::string::npos) {
                        e.text += Agent().Name() + "Agent it's running LLM Model" """\nIs Ollama running at " + Agent().Endpoint() + ", and is the model '" +
                                  Agent().Model() + "' pulled (ollama pull " + Agent().Model() + ")?";
                    }
                    messages_.push_back({Role::Error, std::move(e.text)});
                } else {
                    messages_.push_back({Role::Agent, e.text.empty() ? "(The model returned an empty reply.)"
                                                                     : std::move(e.text)});
                }
            }
            scroll_to_bottom_ = true;
        }
    }

    // Sends the input as a chat message or, with `as_task`, as the goal of a task (Run task).
    void Send(bool as_task) {
        std::string prompt = ui::Trim(input_);
        input_.clear();
        messages_.push_back({Role::User, prompt, as_task ? "You (task)" : ""});
        scroll_to_bottom_ = true;
        busy_ = true;
        task_ = as_task;
        busy_since_ = Clock::now();

        if (worker_.joinable()) {
            worker_.join();  // the previous turn already posted its reply
        }
        if (as_task) {
            worker_ = std::thread([session = session_, goal = std::move(prompt)] { RunTask(*session, goal); });
            return;
        }
        worker_ = std::thread([session = session_, prompt = std::move(prompt)] {
            std::string reply;
            try {
                reply = session->agent->Ask(prompt);
            } catch (const std::exception& e) {
                reply = std::string("Error: ") + e.what();
            } catch (...) {
                reply = "Error: the agent failed with an unknown exception.";
            }
            session->Post(Event::Reply, std::move(reply));
        });
    }

    /*
     * Worker thread: works on `goal` with the task agent, then posts the result. A run that failed because the
     * model did not reply is posted as a chat reply's error is, with the same hint about Ollama.
     */
    static void RunTask(Session& session, const std::string& goal) {
        AutonomousAgent::Result result;
        try {
            result = session.task->Run(goal);
        } catch (const std::exception& e) {
            session.Post(Event::Reply, std::string("Error: ") + e.what());
            return;
        } catch (...) {
            session.Post(Event::Reply, "Error: the task failed with an unknown exception.");
            return;
        }
        if (result.outcome == AutonomousAgent::Outcome::Failed) {
            session.Post(Event::Reply, std::move(result.summary));  // the harness's "Error: ..."
            return;
        }
        Event e{Event::TaskEnd, std::move(result.summary)};
        e.title = std::string("Task: ") + AutonomousAgent::Describe(result.outcome) + " after " +
                  std::to_string(result.steps) + " step(s)";
        e.ok    = result.outcome == AutonomousAgent::Outcome::Finished;
        session.Post(std::move(e));
    }

    // Stops the reply or the task in progress. The agent returns what was written so far, which arrives as the reply.
    void Stop() {
        if (!busy_ || stopping_) {
            return;
        }
        if (task_) {
            session_->task->Cancel();
        } else {
            Agent().Cancel();
        }
        session_->Answer(false);
        stopping_ = true;
    }

    /*
     * The line above the conversation: the agent's model and where it comes from, with its instructions on
     * hover, and its feeling. An agent made in the window also has Delete agent there.
     */
    Action DrawAbout() {
        const ChatAgentConfig& config = Settings();
        const std::string      from   = Made() ? ui::ShortPath(config.agent_file)
                                               : std::string("the default agent, config\\settings.txt");
        ImGui::TextDisabled("Model: %s  |  %s", config.model.c_str(), from.c_str());
        if (Made() && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted("Instructions:");
            const std::string own = ui::Trim(config.instructions);
            ImGui::TextUnformatted(own.empty() ? "(none)" : own.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ui::DrawFeeling(body_view_);
        if (!Made()) {
            return Action::None;
        }

        Action action = Action::None;
        ImGui::SameLine();
        ImGui::BeginDisabled(busy_);
        if (ImGui::SmallButton("Delete agent")) {
            action = Action::Delete;
        }
        ImGui::EndDisabled();
        if (busy_ && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Wait for the agent to finish first.");
        }
        return action;
    }

    void DrawHistory(float height) {
        ImGui::BeginChild("##history", ImVec2(0.0f, height), ImGuiChildFlags_Borders);

        if (messages_.empty() && !busy_) {
            ui::Dim("Ask anything. " + name_ + " can search your knowledge base and the internet, and run code on "
                    "this machine. Run task gives it a goal to work on alone, step by step, until it is done.");
        }

        for (size_t i = 0; i < messages_.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            DrawMessage(messages_[i]);
            ImGui::PopID();
        }

        if (busy_ && !draft_.empty()) {
            ImGui::TextColored(ui::kAgentColor, "%s", name_.c_str());
            ui::Wrapped(draft_);
            ImGui::TextColored(ui::kBusyColor, "%s is writing... %d s", name_.c_str(), BusySeconds());
        } else if (busy_) {
            ImGui::TextColored(ui::kBusyColor, task_ ? "%s is working on the task... %d s" : "%s is thinking... %d s",
                               name_.c_str(), BusySeconds());
        }

        if (scroll_to_bottom_) {
            ImGui::SetScrollHereY(1.0f);
            scroll_to_bottom_ = false;
        }
        ImGui::EndChild();
    }

    // The text of every message can be selected with the mouse and copied with Ctrl+C (ui::SelectableText).
    void DrawMessage(Message& m) {
        switch (m.role) {
        case Role::User:
            ImGui::TextColored(ui::kUserColor, "%s", m.title.empty() ? "You" : m.title.c_str());
            m.view.Draw(m.text);
            break;
        case Role::Agent:
            ImGui::TextColored(m.ok ? ui::kAgentColor : ui::kBusyColor, "%s",
                               m.title.empty() ? name_.c_str() : m.title.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) {
                ImGui::SetClipboardText(m.text.c_str());
            }
            m.view.Draw(m.text);
            break;
        case Role::Step:
            ImGui::TextColored(ui::kAgentColor, "%s", m.title.c_str());
            m.view.Draw(m.text);
            break;
        case Role::Activity:
            if (ImGui::TreeNodeEx("Tool activity", ImGuiTreeNodeFlags_DefaultOpen)) {
                const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                m.view.Draw(m.text, &dim);
                ImGui::TreePop();
            }
            break;
        case Role::Error:
            ImGui::TextColored(ui::kErrorColor, "Error");
            m.view.Draw(m.text, &ui::kErrorColor);
            break;
        }
        ImGui::Spacing();
    }

    void DrawInput(float height) {
        const ImGuiStyle& style = ImGui::GetStyle();
        // Wide enough for the longest label the buttons show.
        const float send_w = std::max(ImGui::CalcTextSize("Stopping").x, ImGui::CalcTextSize("Run task").x) +
                             style.FramePadding.x * 4.0f;

        // Enter sends; Ctrl+Enter inserts a new line. Typing stays possible while the agent works.
        const ImGuiInputTextFlags flags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine;
        bool submit = ImGui::InputTextMultiline("##prompt", &input_,
                                                ImVec2(-(send_w + style.ItemSpacing.x), height), flags);
        // Enter takes the focus away from the box; give it back so the next message can be typed at once.
        if (submit || focus_input_) {
            ImGui::SetKeyboardFocusHere(-1);
            focus_input_ = false;
        }

        const bool can_send = !busy_ && !ui::IsBlank(input_);
        bool run_task = false;
        ImGui::SameLine();
        if (busy_) {
            // While the agent works, the button stops it.
            ImGui::BeginDisabled(stopping_);
            // NOTE: no Esc shortcut: in the focused input box, Esc reverts what is being typed.
            if (ImGui::Button(stopping_ ? "Stopping" : "Stop", ImVec2(-FLT_MIN, height))) {
                Stop();
            }
            ImGui::EndDisabled();
        } else {
            // Send above Run task, the two sharing the height of the input box.
            const ImVec2 button(-FLT_MIN, (height - style.ItemSpacing.y) * 0.5f);
            ImGui::BeginGroup();
            ImGui::BeginDisabled(!can_send);
            if (ImGui::Button("Send", button)) {
                submit = true;
                focus_input_ = true;
            }
            if (ImGui::Button("Run task", button)) {
                run_task = true;
                focus_input_ = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("The agent works on this message as a goal, alone, step by step with its tools,\n"
                                  "until it calls finish (at most %d steps). Code that changes this computer still\n"
                                  "asks you first. The chat does not see the task or its result.",
                                  task_max_steps_);
            }
            ImGui::EndGroup();
        }

        ImGui::TextDisabled("%s", stopping_ ? "Stopping: a running tool finishes first (at most a few seconds)."
                                  : task_   ? "The agent is working on the task alone. Stop ends it; you can type "
                                              "your next message, or use the other agents, meanwhile."
                                  : busy_   ? "The agent is working. Stop interrupts it; you can type your next "
                                              "message, or use the other agents, meanwhile."
                                            : "Enter to send  |  Ctrl+Enter for a new line  |  Run task: the agent "
                                              "works on it alone until it is done");

        if ((submit || run_task) && can_send) {
            Send(run_task);
        }
    }
};
