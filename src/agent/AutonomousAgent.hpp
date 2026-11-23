#pragma once

#include <atomic>
#include <functional>
#include <stdexcept>
#include <string>

#include "agent/ChatAgent.hpp"
#include "common/Utils.hpp"

/*
 * AutonomousAgent - runs the agent on a goal with nobody in the loop.
 *
 * Where ChatAgent answers one message and waits for the next, Run() keeps the model working on the goal, step
 * after step, until it calls the finish tool, stops making progress or runs out of steps.
 *
 * One step is one message to the ChatAgent (StartTask, then ContinueTask): the model acts with its tools (up to
 * max_tool_rounds rounds), then writes a note on what it did. Run() answers the note with the next step's message, so the program, not a person, keeps
 * the conversation going. The conversation grows as in a chat, and the harness shortens the results of earlier
 * steps when it nears the context limit.
 *
 * The instructions added to the system prompt are config\autonomous_prompt.txt; the step limit
 * (autonomous_max_steps) and finish's description (tool_finish) are in config\settings.txt.
 *
 * WARNING: a small model drifts in a long loop, so the stopping rules are enforced here, not left to the
 * prompt. Each one is here because the model went wrong without it:
 *
 *   - finish is a final tool: calling it ends the run at once, without another call to the model. A finish
 *     without a summary is refused; if the model then writes its result as a reply instead of calling finish
 *     again, that reply ends the run as the summary. Going on would restate the goal, and the model would do
 *     it all over again.
 *
 *   - A small model does not call finish when the goal is done: it ends the step with a note, and the next
 *     step's message, which restates the goal, sent it to do the whole work again (in another folder) until
 *     the steps ran out. So after a step whose tools did something, Run() asks whether the goal is done,
 *     offering finish as the only tool, so the model cannot start the work over while it answers
 *     (CheckMessage). It calls finish, or writes what is missing, which the next step's message passes on.
 *
 *   - A step that does nothing new gets a reminder to act or call finish; two in a row end the run (Stalled),
 *     with the last reply as the result. Nothing new means no tool call (small models often write "done"
 *     instead of calling finish), or only calls already made in the run (ChatAgent::LastAskRepeats): the same
 *     search, which a goal the tools cannot answer sends the model back to every step, or the same file or
 *     command again.
 *
 *   - Each step's message restates the goal, which a small model loses sight of, and lists what the tools have
 *     done so far (ChatAgent::TaskLog), so it does not redo it; the last one says to wrap up.
 *
 *   - The search order (the knowledge base first, or the internet first when the goal asks for it) holds for
 *     the whole run, not for each step: a step does not search the knowledge base again before the internet,
 *     and a long goal, which later steps do not restate, keeps its request for the internet
 *     (ChatAgent::ContinueTask).
 *
 *   - No reply from the model (Ollama down) ends the run at once: every later step would fail the same way.
 *
 * WARNING: execute_code runs with the limits it has in a chat (code_timeout_ms, 512 MB), but nobody reads the
 * code before it runs, except flagged code, which waits for the user (SetConfirmHandler).
 *
 * NOTE: a goal that asks to create, change, delete or run a file gets no search tools for the whole run
 * (ChatAgent::FindFileTask), so the model does not search the knowledge base and the internet before writing.
 * One that asks to delete keeps write_file from emptying a file in place of deleting it (a small model does
 * that, then reports the file deleted); delete_file deletes it, after the user allows it.
 *
 * NOTE: with a body (SetFeelingHandler), a run is felt as a whole: the goal given, each step with nothing new,
 * and how it ended. Each step's tools are felt by the ChatAgent, and each step goes to the model in the mood
 * the agent is in by then (ChatAgent::SetMoodSource), so a run that keeps failing goes on more carefully.
 */
class AutonomousAgent {
public:
    using ActivityHandler = ChatAgent::ActivityHandler;
    using StreamHandler   = ChatAgent::StreamHandler;
    // The note the model ended step `step` with; see SetStepHandler.
    using StepHandler     = std::function<void(int step, const std::string& note)>;

    enum class Outcome {
        Finished,    // the model called finish
        Stalled,     // two steps in a row with nothing new: no tool call, or only calls already made
        OutOfSteps,  // autonomous_max_steps steps without finish
        Stopped,     // Cancel()
        Failed,      // the model did not reply
    };

    struct Result {
        Outcome     outcome = Outcome::Failed;
        int         steps   = 0;
        // finish's summary; otherwise the last reply the model wrote, or the error when it did not reply.
        std::string summary;
    };

    static const char* Describe(Outcome outcome) {
        switch (outcome) {
        case Outcome::Finished:     return "Finished";
        case Outcome::Stalled:      return "Ended: two steps without progress (no tool used, or only what was already done)";
        case Outcome::OutOfSteps:   return "Ended: out of steps before finish was called";
        case Outcome::Stopped:      return "Stopped by the user";
        case Outcome::Failed:       return "Failed: the model did not reply";
        }
        return "?";
    }

    // `config`: usually ChatAgentConfig::Load(). Activity is reported as in ChatAgent.
    explicit AutonomousAgent(ActivityHandler on_activity = nullptr, ChatAgentConfig config = ChatAgentConfig())
        : on_activity_(on_activity),
          agent_(std::move(on_activity), WithAutonomousPrompt(std::move(config))) {
        const json params = {
            {"type", "object"},
            {"properties", {{"summary", {
                {"type", "string"},
                {"description", "The result for the user: the answer, numbers or text the goal asked for, what "
                                "was done, and anything that failed."}
            }}}},
            {"required", json::array({"summary"})},
        };
        agent_.RegisterTool("finish", agent_.Settings().tool_finish, params,
                            [this](const json& args) { return Finish(args); }, /*ends_prompt=*/true);
    }

    // WARNING: not copyable: the finish tool captures `this`.
    AutonomousAgent(const AutonomousAgent&)             = delete;
    AutonomousAgent& operator=(const AutonomousAgent&)  = delete;

    // Shows each step's reply while the model writes it; see ChatAgent::SetStreamHandler.
    void SetStreamHandler(StreamHandler handler) { agent_.SetStreamHandler(std::move(handler)); }

    /*
     * Receives the note the model ends each step with, so a window can keep it once the stream moves on to the
     * next step. The last step's reply is not sent here: it is the run's result (Result::summary).
     *
     * WARNING: called on the thread that runs Run(). Do not call this while Run() is running.
     */
    void SetStepHandler(StepHandler handler) { on_step_ = std::move(handler); }

    /*
     * Asks the user before flagged code runs; see ChatAgent::SetConfirmHandler. Without a handler, flagged code
     * is not run.
     *
     * WARNING: the run blocks on the answer, so a flagged step needs someone there.
     */
    void SetConfirmHandler(ChatAgent::ConfirmHandler handler) { agent_.SetConfirmHandler(std::move(handler)); }

    /*
     * What happens to the agent in a run: its tools' feelings (ChatAgent::SetFeelingHandler), and the run's
     * own: the goal given, a step with nothing new (Stuck), and how it ended.
     *
     * WARNING: called on the thread that runs Run(). Do not call this while Run() is running.
     */
    void SetFeelingHandler(ChatAgent::FeelingHandler handler) { agent_.SetFeelingHandler(std::move(handler)); }

    // The mood each step goes to the model in; see ChatAgent::SetMoodSource. Same warning.
    void SetMoodSource(ChatAgent::MoodSource source) { agent_.SetMoodSource(std::move(source)); }

    /*
     * See ChatAgent::ApplySettings; the autonomous instructions are added again.
     *
     * WARNING: do not call this while Run() is running.
     */
    void ApplySettings(ChatAgentConfig config) { agent_.ApplySettings(WithAutonomousPrompt(std::move(config))); }

    /*
     * Stops a Run() in progress, from any thread (Ctrl+C, a Stop button). The step under way stops as a chat
     * reply does (a running tool finishes first), and Run() returns Stopped.
     */
    void Cancel() {
        stop_ = true;
        agent_.Cancel();
    }

    /*
     * Works on `goal` until finish is called or a stopping rule ends the run, in a new conversation. The goal is
     * felt as it is given, and the run as it ends (Felt).
     *
     * WARNING: blocks for the whole run, which on a slow machine takes minutes to hours: call it off the UI
     * thread.
     */
    Result Run(const std::string& goal) {
        agent_.Feel(Feeling::GoalGiven);
        const Result result = Work(goal);
        agent_.Feel(Felt(result.outcome));
        return result;
    }

private:
    // Up to this size the goal is repeated in every step's message; a longer one would fill the context.
    static constexpr size_t kRestateGoalBytes = 600;
    // What the model says is missing, as the next step's message repeats it.
    static constexpr size_t kMissingBytes     = 400;

    // Why a step did nothing new, which the next step's message points out.
    enum class Idle {
        None,         // it did something new
        NoTool,       // no tool call
        OnlyRepeats,  // every tool call was one already made in this run
    };

    ActivityHandler     on_activity_;
    StepHandler         on_step_;
    ChatAgent           agent_;
    std::atomic<bool>   stop_{false};
    bool                finished_       = false;  // finish was called in this run
    bool                finish_refused_ = false;  // finish was called without a summary in the current step
    std::string         summary_;                 // its summary

    // How the end of a run feels.
    static Feeling Felt(Outcome outcome) {
        switch (outcome) {
        case Outcome::Finished:   return Feeling::GoalDone;
        case Outcome::Stalled:    return Feeling::GoalMissed;
        case Outcome::OutOfSteps: return Feeling::GoalMissed;
        case Outcome::Stopped:    return Feeling::Stopped;
        case Outcome::Failed:     return Feeling::NoReply;
        }
        return Feeling::GoalMissed;
    }

    // Run() without the feelings of the run as a whole.
    Result Work(const std::string& goal) {
        agent_.NewConversation();
        stop_     = false;
        finished_ = false;
        summary_.clear();

        const int   max_steps = agent_.Settings().autonomous_max_steps;
        Result      result;
        int         idle_steps = 0;  // steps in a row with nothing new; see Idle
        std::string message =
            "Goal: " + goal + "\n\n"
            "Work on this goal on your own, step by step, with your tools. When it is done, call finish.";

        for (int step = 1; step <= max_steps; ++step) {
            if (stop_) {
                result.outcome = Outcome::Stopped;
                return result;
            }
            result.steps = step;
            Report("[Autonomous step " + std::to_string(step) + " of " + std::to_string(max_steps) + "]");

            const std::string done_before = agent_.TaskLog();
            finish_refused_ = false;
            result.summary  = step == 1 ? agent_.StartTask(message) : agent_.ContinueTask(message);
            if (Over(result, result.summary)) {
                return result;
            }

            const int tool_calls = agent_.LastAsk().tool_calls;
            Idle      idle       = Idle::None;
            if (tool_calls == 0) {
                idle = Idle::NoTool;
            } else if (agent_.LastAskRepeats() >= tool_calls) {
                idle = Idle::OnlyRepeats;
            }
            if (idle != Idle::None) {
                agent_.Feel(Feeling::Stuck);
            }
            idle_steps = idle == Idle::None ? 0 : idle_steps + 1;
            if (idle_steps >= 2) {
                result.outcome = Outcome::Stalled;
                return result;
            }
            if (on_step_ && step < max_steps) {
                on_step_(step, result.summary);
            }

            /*
             * The tools did something new and the step ended with a note rather than finish: ask whether that
             * was the goal, with finish as the only tool.
             *
             * WARNING: not after a step whose tools only failed (no path, no code): with nothing done, a small
             * model would answer by finishing.
             */
            std::string missing;
            if (idle == Idle::None && agent_.TaskLog() != done_before) {
                Report("[Autonomous: asking whether the goal is done]");
                finish_refused_ = false;
                const std::string answer = agent_.ContinueTask(CheckMessage(goal, agent_.TaskLog()), {"finish"});
                if (Over(result, answer)) {
                    return result;
                }
                missing = Clip(answer, kMissingBytes);
                Report("[Autonomous: not done yet" + (missing.empty() ? std::string() : ": " + missing) + "]");
            }
            message = NextStep(goal, step + 1, max_steps, idle, agent_.TaskLog(), missing);
        }
        result.outcome = Outcome::OutOfSteps;
        return result;
    }

    static ChatAgentConfig WithAutonomousPrompt(ChatAgentConfig config) {
        config.system_prompt += "\n" + config.autonomous_prompt;
        return config;
    }

    /*
     * After a call to the model that wrote `reply`: true when the run is over, with `result` filled in. It is
     * over when finish was called, when Cancel() stopped it, when the model did not reply, or when finish was
     * refused for want of a summary and the model answered by writing its result as a reply.
     *
     * NOTE: small models answer a refused finish that way rather than calling finish again. The model meant to
     * finish, so the reply is the summary.
     */
    bool Over(Result& result, const std::string& reply) {
        if (finished_) {
            result.outcome = Outcome::Finished;
            result.summary = summary_;
        } else if (stop_) {
            result.outcome = Outcome::Stopped;
            result.summary = reply;
        } else if (agent_.LastAsk().failed) {
            result.outcome = Outcome::Failed;
            result.summary = reply;
        } else if (finish_refused_ && reply.find_first_not_of(" \t\r\n") != std::string::npos) {
            Report("[Autonomous: finish was called without a summary; the reply after it is taken as the result]");
            result.outcome = Outcome::Finished;
            result.summary = reply;
        } else {
            return false;
        }
        return true;
    }

    // The goal as the step messages restate it.
    static std::string GoalLine(const std::string& goal) {
        if (goal.size() <= kRestateGoalBytes) {
            return "The goal: " + goal + "\n\n";
        }
        return "The goal is the one given at the start.\n\n";
    }

    // At most `max_bytes` of `text`, trimmed, cut at a space.
    static std::string Clip(const std::string& text, size_t max_bytes) {
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        std::string s = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        if (s.size() > max_bytes) {
            const size_t cut = s.rfind(' ', max_bytes);
            s = s.substr(0, cut == std::string::npos ? max_bytes : cut) + " ...";
        }
        return s;
    }

    /*
     * The question after a step that did something: is the goal done? Sent with finish as the only tool.
     *
     * WARNING: like NextStep, it must not contain the words of internet_words: ChatAgent::ContinueTask would
     * take them as a request to search the internet first.
     */
    static std::string CheckMessage(const std::string& goal, const std::string& done) {
        return "Before the next step, check the goal. " + GoalLine(goal) +
               "Done so far, by your tools:\n" + done + "\n"
               "Is the goal done? If it is, call finish now with the result: what was done, and the full paths of "
               "the files written. If it is not, do not call any tool: write in one sentence what is still missing.";
    }

    /*
     * The message that starts step `step`, with what the tools have done so far (`done`) and what the model said
     * is missing.
     *
     * WARNING: it must not contain the words of internet_words ("online"...): ChatAgent::ContinueTask would
     * take them as a request to search the internet first.
     */
    static std::string NextStep(const std::string& goal, int step, int max_steps, Idle idle, const std::string& done,
                                const std::string& missing) {
        std::string text = "Step " + std::to_string(step) + " of " + std::to_string(max_steps) + ". " + GoalLine(goal);
        if (!done.empty()) {
            text += "Done so far, by your tools:\n" + done + "\n";
        }
        switch (idle) {
        case Idle::NoTool:
            text += "In the last step you used no tool. If the goal is done, call finish now with the result. "
                    "If it is not, take the next action with a tool.";
            break;
        case Idle::OnlyRepeats:
            text += "In the last step you only repeated what you had already done (the same searches, the same "
                    "files, the same commands), so nothing new came of it. If the goal is done, call finish now "
                    "with the result. If your tools cannot do it, call finish now and say what you tried and what "
                    "is missing. Otherwise do something different.";
            break;
        case Idle::None:
            /*
             * WARNING: no word about searches here: said every step, it sent a model working on files back to
             * searching. The repeats are dealt with in OnlyRepeats.
             */
            if (!missing.empty()) {
                text += "Still missing, as you said: " + missing + "\n\n";
            }
            text += "Do only what is missing, with the tool it needs. Do not redo what is done above. When the goal "
                    "is done, call finish with the result.";
            break;
        }
        if (step == max_steps) {
            text += "\nThis is the last step. Do what you can, then call finish with the result, what was done "
                    "and what is left.";
        }
        return text;
    }

    /*
     * The finish tool. Without a summary it throws: the model reads the error and can call finish again. If it
     * writes the result as a reply instead, Run() takes that reply as the summary (see finish_refused_).
     */
    std::string Finish(const json& args) {
        auto it = args.find("summary");
        const std::string summary = (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string();
        if (summary.find_first_not_of(" \t\r\n") == std::string::npos) {
            finish_refused_ = true;
            throw std::runtime_error("no summary given. Call finish again with the result in summary.");
        }
        finished_ = true;
        summary_  = summary;
        Report("[Autonomous: finish called]");
        return summary;
    }

    void Report(const std::string& text) const {
        if (on_activity_) {
            on_activity_(text);
        } else {
            TextUtilities::print("\n" + text + "\n");
        }
    }
};
