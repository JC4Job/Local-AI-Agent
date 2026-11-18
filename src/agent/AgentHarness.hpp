#pragma once
#include "llm/OllamaClient.hpp"
#include "common/Utils.hpp"
#include <io.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

/*
    Like other providers GPT, Gemini or Claude
    Shows the elapsed time while the model works, so a long generation doesn't look like a hang.
    Appears only after the first second and only on a console, and erases itself when done.
*/
class WaitIndicator {
public:
    explicit WaitIndicator(std::string label, bool enabled = true) : label_(std::move(label)) {
        if (enabled && _isatty(_fileno(stdout))) {
            worker_ = std::thread([this] { Run(); });
        }
    }
    ~WaitIndicator() { Stop(); }

    // Erases the indicator and stops it, e.g. when the reply starts appearing. Safe to call more than once.
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            done_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
    }
    WaitIndicator(const WaitIndicator&) = delete;
    WaitIndicator& operator=(const WaitIndicator&) = delete;

private:
    void Run() {
        const auto start = std::chrono::steady_clock::now();
        size_t width = 0;
        std::unique_lock<std::mutex> lock(mu_);
        while (!cv_.wait_for(lock, std::chrono::seconds(1), [this] { return done_; })) {
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            const std::string text = "[" + label_ + "... " + std::to_string(secs) + " s]";
            width = text.size();
            TextUtilities::print("\r" + text);
        }
        if (width) {
            TextUtilities::print("\r" + std::string(width, ' ') + "\r");
        }
    }

    std::string             label_;
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    done_ = false;
    std::thread             worker_;  // last, so everything above exists before it starts
};

class AgentHarness {
public:
    using ToolHandler   = std::function<std::string(const json&)>;
    // Receives one status line at a time (tool dispatched, reply cut off...), with no surrounding newlines.
    using EventHandler  = std::function<void(const std::string&)>;
    /*
        Receives the model's reply as it is written, a piece at a time. `first` marks the first piece of each call
        to the model: whatever came before belonged to an earlier call, which ended in tool calls (text a model
        writes before calling a tool), so it is not part of this reply. The final reply ProcessPrompt returns is
        the text streamed since the last `first`.
    */
    using StreamHandler = std::function<void(const std::string& piece, bool first)>;

    // What the last ProcessPrompt did, for a caller that drives the model in a loop (AutonomousAgent).
    struct PromptStats {
        int  tool_calls = 0;      // tool calls the model made, including any not run
        bool failed     = false;  // no reply from the model (Ollama down, unknown model): the reply is the error
    };

    AgentHarness(OllamaClient client, std::string model_name, std::string system_prompt)
        : client_(std::move(client)), model_(std::move(model_name)) {

        messages_ = json::array({
            {{"role", "system"}, {"content", std::move(system_prompt)}}
        });
    }

    /*
        Without a handler, status goes to the console (and the wait indicator runs there). A GUI installs one
        to show the status in its window instead. Called on the thread that runs ProcessPrompt.
    */
    void SetEventHandler(EventHandler handler) { on_event_ = std::move(handler); }

    // Shows the reply while the model writes it. Called on the thread that runs ProcessPrompt.
    void SetStreamHandler(StreamHandler handler) { on_stream_ = std::move(handler); }

    // Forgets the conversation and keeps the system prompt.
    void ResetConversation() {
        if (messages_.size() > 1) {
            messages_.erase(messages_.begin() + 1, messages_.end());
        }
        prompt_start_ = messages_.size();
    }

    /*
        Rounds of tool calls allowed per prompt. After that the model is asked to answer without tools, so a
        model that keeps calling tools (the same search again and again) cannot loop forever.
    */
    void SetMaxToolRounds(int rounds) { max_tool_rounds_ = rounds > 0 ? rounds : 1; }

    /*
        Tokens the conversation sent to the model may take: Ollama's num_ctx minus the room kept for the reply
        (num_predict). Above it, the oldest tool results are shortened (see FitContext). 0: never shortened.
    */
    void SetContextBudget(int tokens) { context_budget_ = tokens > 0 ? static_cast<size_t>(tokens) : 0; }

    // The options sent to Ollama with every call (num_ctx, num_predict); see OllamaClient::SetOptions.
    void SetModelOptions(json options) { client_.SetOptions(std::move(options)); }

    // The model of the next calls. It is asked again whether it supports tools.
    void SetModel(std::string model_name) {
        if (model_name != model_) {
            model_           = std::move(model_name);
            tools_supported_ = true;
        }
    }

    // Replaces the system prompt, keeping the conversation; the next call reads the new one.
    void SetSystemPrompt(std::string system_prompt) { messages_[0]["content"] = std::move(system_prompt); }

    /*
        Stops the prompt in progress (a Stop button). A call to the model is aborted at once, even before its first
        word; a tool already running finishes first (within its own time limit) and the remaining ones are skipped.
        ProcessPrompt then returns what the model had written. Safe to call from any thread.
    */
    void Cancel() { cancel_.Cancel(); }

    /*
        Registers a native function as a tool.
        `ends_prompt`: calling the tool ends the prompt (AutonomousAgent's finish). ProcessPrompt returns the tool's
        result as the reply, without calling the model again; tool calls after it in the same message are not run.
        A handler that throws does not end the prompt: the model reads the error and can call the tool again.
    */
    void RegisterTool(const std::string& name, const std::string& description, json parameters, ToolHandler handler,
                      bool ends_prompt = false) {
        tools_schema_.push_back({
            {"type", "function"},
            {"function", {
                {"name", name},
                {"description", description},
                {"parameters", parameters}
            }}
        });
        tool_dispatch_[name] = std::move(handler);
        if (ends_prompt) {
            final_tools_.insert(name);
        } else {
            final_tools_.erase(name);
        }
        tools_tokens_ = EstimateTokens(tools_schema_.dump().size());
    }

    /*
        Tools left out of the next prompts, until called again: they are not sent to the model, and a call to one
        (a small model calls what it saw earlier in the conversation) is answered with an error instead of run.
        ChatAgent leaves the searches out of a file task (create, change, delete or run a file). Empty: every tool.
    */
    void SetHiddenTools(std::unordered_set<std::string> names) { hidden_tools_ = std::move(names); }

    /*
        Only these tools for the next prompts, until called again (the hidden ones stay out too); empty: no limit.
        AutonomousAgent offers only finish when it asks whether the goal is done, so the model cannot start the
        work over while it answers.
    */
    void SetOnlyTools(std::unordered_set<std::string> names) { only_tools_ = std::move(names); }

    const PromptStats& LastPrompt() const { return last_prompt_; }

    // Process a user prompt through the agent lifecycle
    std::string ProcessPrompt(const std::string& user_input) {
        cancel_.Reset();
        last_prompt_  = PromptStats();
        prompt_start_ = messages_.size();
        overflow_reported_ = false;
        messages_.push_back({{"role", "user"}, {"content", user_input}});

        // Tool resolution: the model may call tools, read their results and call more, up to max_tool_rounds_.
        for (int round = 0;; ++round) {
            const bool allow_tools = round < max_tool_rounds_;
            if (!allow_tools) {
                Notify("[Harness] " + std::to_string(max_tool_rounds_) +
                       " rounds of tool calls without an answer; asking the model to answer now.");
                messages_.push_back({
                    {"role", "system"},
                    {"content", "You have called tools enough for this question. Do not call any more tools. "
                                "Answer now from the tool results above. If they do not contain the answer, "
                                "say so."}
                });
            }

            if (cancel_.Cancelled()) {
                return Stopped(json::object());
            }
            json response = SendChat(allow_tools);
            if (cancel_.Cancelled()) {
                return Stopped(response);
            }
            if (!response.contains("message")) {
                last_prompt_.failed = true;
                return "Error: " + response.value("error", std::string("Empty or invalid response from LLM engine."));
            }

            json message_obj = response["message"];
            if (allow_tools && tools_supported_) {
                RecoverTextToolCalls(message_obj);
            }
            messages_.push_back(message_obj);

            const bool wants_tools = message_obj.contains("tool_calls") && !message_obj["tool_calls"].empty();
            if (!wants_tools || !allow_tools) {
                return message_obj.value("content", "");
            }

            std::string final_reply;
            if (RunToolCalls(message_obj["tool_calls"], final_reply)) {
                // Closes the turn, so the conversation is a complete exchange, as after Stopped().
                messages_.push_back({{"role", "assistant"}, {"content", final_reply}});
                return final_reply;
            }
        }
    }

private:
    OllamaClient    client_;
    std::string     model_;
    json            messages_;
    json            tools_schema_       = json::array();
    bool            tools_supported_    = true;
    EventHandler    on_event_;
    StreamHandler   on_stream_;
    CancelToken     cancel_;                    // see Cancel()
    int             max_tool_rounds_    = 5;
    size_t          prompt_start_       = 1;     // index of the current prompt's user message
    size_t          context_budget_     = 0;     // see SetContextBudget
    size_t          tools_tokens_       = 0;     // estimated size of the tool definitions sent with every call
    bool            overflow_reported_  = false; // "still too large" already reported for the current prompt
    PromptStats     last_prompt_;               // see LastPrompt()
    std::unordered_map<std::string, ToolHandler> tool_dispatch_;
    std::unordered_set<std::string>              final_tools_;   // registered with ends_prompt
    std::unordered_set<std::string>              hidden_tools_;  // see SetHiddenTools
    std::unordered_set<std::string>              only_tools_;    // see SetOnlyTools

    // Sent to the model and run when called: not hidden, and among the only tools when there are some.
    bool Offered(const std::string& name) const {
        return !hidden_tools_.count(name) && (only_tools_.empty() || only_tools_.count(name));
    }

    // When the conversation must shrink, earlier tool results longer than this are cut down to a preview.
    static constexpr size_t         kKeepToolResultChars    = 800;
    static constexpr size_t         kPreviewChars           = 500;
    // Starts every shortened result, so it is not shortened twice.
    static constexpr const char*    kShortenedNote          = "[Result of an earlier question, shortened to save context.";
    /*
        Rough size of a token for the budget. English averages ~4 characters per token and Portuguese a little
        less (accents take 2 bytes); 3 overestimates, which errs on the safe side.
    */
    static constexpr size_t         kBytesPerToken          = 3;
    static constexpr size_t         kTokensPerMessage       = 8;  // role markers the chat template adds around each message

    /*
        Runs the calls in order. True when one of them was a tool registered with ends_prompt: its result is in
        `final_reply`, and the calls after it are not run.
    */
    bool RunToolCalls(const json& tool_calls, std::string& final_reply) {
        std::string ended_by;
        for (const auto& call : tool_calls) {
            std::string fn_name = call["function"]["name"];
            json fn_args = call["function"].value("arguments", json::object());

            Notify("[Harness Dispatching Tool: " + fn_name + "]");
            ++last_prompt_.tool_calls;

            std::string tool_result;
            auto handler = tool_dispatch_.find(fn_name);
            if (cancel_.Cancelled()) {
                // Every call still gets a result, so the conversation stays well-formed for the next prompt.
                tool_result = "Not run: the user stopped the reply.";
            } else if (!ended_by.empty()) {
                tool_result = "Not run: " + ended_by + " was called before it.";
            } else if (handler == tool_dispatch_.end()) {
                tool_result = "Error: Tool execution target not bound.";
            } else if (!Offered(fn_name)) {
                tool_result = "Not run: " + fn_name + " is not available for this request. Use the tools you "
                              "were given for it.";
                Notify("[Harness] " + fn_name + " is left out for this request; not run.");
            } else {
                // A throwing tool must not take down the session; report it to the model instead.
                try {
                    tool_result = handler->second(fn_args);
                    if (final_tools_.count(fn_name)) {
                        ended_by    = fn_name;
                        final_reply = tool_result;
                    }
                } catch (const std::exception& e) {
                    // Shown too: only the model read it, so a refused finish looked like one that worked.
                    tool_result = "Error: tool '" + fn_name + "' failed: " + e.what();
                    Notify("[Harness] " + tool_result);
                }
            }

            messages_.push_back({
                {"role", "tool"},
                {"tool_name", fn_name},
                {"content", tool_result}
            });
        }
        return !ended_by.empty();
    }

    /*
        A small model (llama3.2) sometimes writes a tool call into its reply as text, after a sentence:
        "Vou tentar com uma busca na internet. {"name": "search_internet", "parameters": {"query": "..."}}".
        Ollama recognises a call only when the reply starts with it, so this one arrives as plain text and the
        prompt would end on it with nothing run. When the reply has no tool calls, every JSON object in its text
        that names a registered tool and gives its arguments becomes a tool call, and the text before the first
        one stays as the reply's content.
    */
    void RecoverTextToolCalls(json& message) const {
        if (message.contains("tool_calls") && !message["tool_calls"].empty()) {
            return;
        }
        const std::string content = message.value("content", std::string());
        json        calls = json::array();
        size_t      first = std::string::npos;
        std::string names;
        for (size_t open = content.find('{'); open != std::string::npos;) {
            const size_t close = MatchingBrace(content, open);
            json call = close == std::string::npos ? json() : ToolCallFromText(content.substr(open, close - open + 1));
            if (call.is_null()) {
                open = content.find('{', open + 1);
                continue;
            }
            if (first == std::string::npos) {
                first = open;
            }
            names += (names.empty() ? "" : ", ") + call["function"]["name"].get<std::string>();
            calls.push_back(std::move(call));
            open = content.find('{', close + 1);
        }
        if (calls.empty()) {
            return;
        }

        // The sentence before the call, without the code fence or tag a model may open around it.
        std::string before = content.substr(0, first);
        const auto trim = [&before] {
            const size_t end = before.find_last_not_of(" \t\r\n");
            before.erase(end == std::string::npos ? 0 : end + 1);
        };
        trim();
        for (const std::string marker : {"```json", "```", "<tool_call>"}) {
            if (before.size() >= marker.size() &&
                before.compare(before.size() - marker.size(), marker.size(), marker) == 0) {
                before.erase(before.size() - marker.size());
                trim();
            }
        }
        message["content"]    = before;
        message["tool_calls"] = calls;
        Notify("[Harness] The model wrote a tool call as text instead of calling it; running it: " + names);
    }

    // The index of the '}' that closes the '{' at `open`, skipping braces inside JSON strings; npos if none.
    static size_t MatchingBrace(const std::string& text, size_t open) {
        int  depth     = 0;
        bool in_string = false;
        bool escaped   = false;
        for (size_t i = open; i < text.size(); ++i) {
            const char c = text[i];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
            } else if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}' && --depth == 0) {
                return i;
            }
        }
        return std::string::npos;
    }

    /*
        The tool call written as `text`, shaped as Ollama returns one: {"function": {"name", "arguments"}}.
        Accepts llama's {"name", "parameters"}, {"name", "arguments"} (arguments as an object or as a JSON string)
        and Ollama's own {"function": {...}}. Null unless it names a registered tool and its arguments are an object.
    */
    json ToolCallFromText(const std::string& text) const {
        const json obj = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (!obj.is_object()) {
            return nullptr;
        }
        const json& fn   = (obj.contains("function") && obj.at("function").is_object()) ? obj.at("function") : obj;
        const auto  name = fn.find("name");
        if (name == fn.end() || !name->is_string() || !tool_dispatch_.count(name->get<std::string>())) {
            return nullptr;
        }
        json args;
        for (const char* key : {"parameters", "arguments"}) {
            const auto it = fn.find(key);
            if (it != fn.end()) {
                args = it->is_string() ? json::parse(it->get<std::string>(), nullptr, false) : *it;
                break;
            }
        }
        if (!args.is_object()) {
            return nullptr;
        }
        return json{{"function", {{"name", *name}, {"arguments", args}}}};
    }

    /*
        The user stopped the prompt: keeps what the model had written of `response`, closes the turn with it so the
        next prompt sees a complete exchange, and returns it.
    */
    std::string Stopped(const json& response) {
        std::string partial;
        auto message = response.find("message");
        if (message != response.end() && message->is_object()) {
            partial = message->value("content", std::string());
        }
        const std::string reply = partial.empty() ? "(Stopped.)" : partial + "\n\n(Stopped.)";
        messages_.push_back({{"role", "assistant"}, {"content", reply}});
        Notify("[Stopped by the user]");
        return reply;
    }

    static size_t EstimateTokens(size_t bytes) { return bytes / kBytesPerToken; }

    static size_t MessageTokens(const json& msg) {
        size_t bytes = msg.value("content", std::string()).size();
        if (msg.contains("tool_calls")) {
            bytes += msg["tool_calls"].dump().size();
        }
        return EstimateTokens(bytes) + kTokensPerMessage;
    }

    /*
        Search results are a few thousand characters each. When the conversation outgrows the context, Ollama
        silently drops the oldest messages and the model loses what it fetched. So before each call, if the
        conversation would not fit the budget, the oldest tool results of earlier questions are cut to a preview
        until it takes three quarters of the budget; the replies that used them stay in full.
        - Until then nothing is cut, so a follow-up question can still use what earlier searches found.
        - The current question's results are never cut: the model is reading them.
        - Cutting several at once, rarely, keeps Ollama's prompt cache useful: it reuses only the unchanged
          start of the conversation, and every cut changes it from that message on.
        The full results are still in ChatAgent's ToolResultCache, so calling the tool again is answered from
        memory without running anything.
    */
    void FitContext() {
        if (context_budget_ == 0) {
            return;
        }
        size_t used = tools_tokens_;
        for (const json& msg : messages_) {
            used += MessageTokens(msg);
        }
        if (used <= context_budget_) {
            return;
        }

        const size_t target = context_budget_ * 3 / 4;
        size_t shortened = 0;
        for (size_t i = 1; i < prompt_start_ && i < messages_.size() && used > target; ++i) {
            json& msg = messages_[i];
            if (msg.value("role", std::string()) != "tool") {
                continue;
            }
            const std::string content = msg.value("content", std::string());
            if (content.size() <= kKeepToolResultChars || content.rfind(kShortenedNote, 0) == 0) {
                continue;
            }

            const size_t before = MessageTokens(msg);
            const std::string tool = msg.value("tool_name", std::string("the tool"));
            msg["content"] = std::string(kShortenedNote) + " If you need the full text, call " + tool +
                             " again with the same arguments: it is answered from memory.]\n" +
                             Utf8Prefix(content, kPreviewChars) + " ...";
            used = used - before + MessageTokens(msg);
            ++shortened;
        }
        if (shortened) {
            Notify("[Harness] The conversation was near the context limit: shortened " + std::to_string(shortened) +
                   " earlier tool result(s).");
        }
        if (used > context_budget_ && !overflow_reported_) {
            overflow_reported_ = true;
            Notify("[Harness] The conversation (~" + std::to_string(used) + " tokens) is larger than the context "
                   "budget (" + std::to_string(context_budget_) + "); Ollama will drop the oldest messages. "
                   "Start a new chat, or raise num_ctx in config\\settings.txt.");
        }
    }

    // Cuts at a character boundary: a split multi-byte character would make the JSON payload invalid.
    static std::string Utf8Prefix(const std::string& s, size_t max_bytes) {
        if (s.size() <= max_bytes) {
            return s;
        }
        size_t end = max_bytes;
        while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
            --end;
        }
        return s.substr(0, end);
    }

    void Notify(const std::string& text, bool is_warning = false) const {
        if (on_event_) {
            on_event_(text);
        } else if (is_warning) {
            std::cerr << "\n" << text << "\n";
        } else {
            TextUtilities::print("\n" + text + "\n");
        }
    }

    // One call to the model, its reply streamed to on_stream_ as it is written.
    json StreamChat(bool with_tools) {
        WaitIndicator waiting("Waiting for " + model_, /*enabled=*/!on_event_);
        bool first = true;
        const OllamaClient::TextHandler on_text = [&](const std::string& piece) {
            if (!on_stream_) {
                return;
            }
            if (first) {
                waiting.Stop();  // the reply itself now shows that the model is working
            }
            on_stream_(piece, first);
            first = false;
        };
        return client_.Chat(model_, messages_, with_tools ? OfferedTools() : json::array(), on_text, &cancel_);
    }

    // The tools sent to the model: the registered ones that are Offered().
    json OfferedTools() const {
        if (hidden_tools_.empty() && only_tools_.empty()) {
            return tools_schema_;
        }
        json tools = json::array();
        for (const json& tool : tools_schema_) {
            if (Offered(tool["function"]["name"].get<std::string>())) {
                tools.push_back(tool);
            }
        }
        return tools;
    }

    json SendChat(bool allow_tools = true) {
        FitContext();
        const bool send_tools = allow_tools && tools_supported_;
        json response = StreamChat(send_tools);

        if (cancel_.Cancelled()) {
            return response;  // ProcessPrompt reports it
        }

        std::string error = response.value("error", std::string());
        if (send_tools && error.find("does not support tools") != std::string::npos) {
            Notify("[Harness] Model '" + model_ + "' does not support tools; continuing as plain chat. "
                   "Registered tools will NOT run. Pick a model with the 'tools' capability "
                   "(check with `ollama show " + model_ + "`).", true);
            tools_supported_ = false;
            response = StreamChat(false);
        }

        const std::string done_reason = response.value("done_reason", std::string());
        if (done_reason == "length") {
            Notify("[Harness] The model's reply hit the num_predict limit and was cut off; any tool call in it is "
                   "incomplete. Raise num_predict in config\\settings.txt for longer replies.", true);
        } else if (done_reason == "interrupted" && !cancel_.Cancelled()) {
            Notify("[Harness] The reply stopped before the model finished it" +
                   (response.contains("error") ? ": " + response.value("error", std::string()) : std::string(".")),
                   true);
        }
        return response;
    }
};