#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <iomanip>

#include "llm/OllamaClient.hpp"
#include "agent/AgentConfig.hpp"
#include "agent/AgentHarness.hpp"
#include "agent/ToolResultCache.hpp"
#include "execution/CLISecurityChecking.hpp"
#include "execution/CodeExecutor.hpp"
#include "rag/RagIndex.hpp"
#include "common/MacroWrap.h"

/*
    The chatbot: model, system prompt and tools. It knows nothing about how it is shown.
    A front end (the console in cli_main.cpp, the window in ChatWindow.hpp) calls Ask() and, optionally,
    receives tool activity through an ActivityHandler. Without one, activity is printed to the console.
*/
class ChatAgent {
public:
    /*
        One status line per call (tool started, code being run...), no surrounding newlines.
        Called on the thread that runs Ask().
    */
    using ActivityHandler = std::function<void(const std::string&)>;
    // The reply as the model writes it; see AgentHarness::StreamHandler.
    using StreamHandler = AgentHarness::StreamHandler;
    // Asks the user before flagged code runs; see SetConfirmHandler.
    using ConfirmHandler = CLISecurityChecking::ConfirmHandler;
    using Config = ChatAgentConfig;

    // `config`: usually ChatAgentConfig::Load(), the settings in config\settings.txt and config\system_prompt.txt.
    explicit ChatAgent(ActivityHandler on_activity = nullptr, Config config = Config())
        : config_(std::move(config)),
          on_activity_(std::move(on_activity)),
          harness_(OllamaClient(config_.endpoint, ChatOptions(config_)), config_.model, config_.system_prompt),
          embed_client_(config_.endpoint),
          rag_(MakeRagConfig(config_),
               [this](const std::vector<std::string>& texts, std::vector<std::vector<float>>& vectors,
                      unsigned timeout_ms) {
                   // The same options on every request: Ollama reloads a model when its options change.
                   const json options = {{"num_ctx", config_.embed_num_ctx}};
                   return embed_client_.Embed(config_.embed_model, texts, vectors, options, timeout_ms);
               }) {
        exec_limits_.timeout_ms = config_.code_timeout_ms;
        if (on_activity_) {
            harness_.SetEventHandler(on_activity_);
        }
        harness_.SetMaxToolRounds(config_.max_tool_rounds);
        harness_.SetContextBudget(config_.num_ctx - config_.num_predict);
        RegisterTools();
        for (const std::string& note : config_.notes) {
            Report("[" + note + "]");
        }
    }

    // Shows the reply while the model writes it, on the thread that runs Ask(). Do not call while Ask() is running.
    void SetStreamHandler(StreamHandler handler) { harness_.SetStreamHandler(std::move(handler)); }

    /*
        Asks the user whether code that CLISecurityChecking flags may run, or a file be written outside the
        workspace folder (the console's prompt, the window's popup). Called on the thread that runs Ask(), which
        waits for the answer. Without a handler, flagged code is never run nor such a file written. Do not call
        while Ask() is running.
    */
    void SetConfirmHandler(ConfirmHandler handler) { confirm_ = std::move(handler); }

    /*
        Stops the Ask() in progress, which then returns what the model had written so far (see
        AgentHarness::Cancel). Meant for a Stop button: safe to call from any thread while Ask() runs.
    */
    void Cancel() { harness_.Cancel(); }

    /*
        Takes the settings the Settings window saved (`config`: ChatAgentConfig::Load() again) from the next Ask()
        on, keeping the conversation: the model, the size of the context and of the replies, the tool rounds, the
        task steps, the knowledge base limits and the system prompt, which holds an agent's instructions. The rest
        (endpoint, folders, tool descriptions) stays as it was at the start. Ollama reloads a model whose num_ctx
        changes, so that reply starts slower. Do not call while Ask() is running.
    */
    void ApplySettings(const Config& config) {
        const bool kb_changed = config.kb_max_passages != config_.kb_max_passages ||
                                config.kb_max_result_chars != config_.kb_max_result_chars;
        config_.model                = config.model;
        config_.num_ctx              = config.num_ctx;
        config_.num_predict          = config.num_predict;
        config_.embed_num_ctx        = config.embed_num_ctx;  // the embedding requests read it each time
        config_.max_tool_rounds      = config.max_tool_rounds;
        config_.autonomous_max_steps = config.autonomous_max_steps;
        config_.kb_max_passages      = config.kb_max_passages;
        config_.kb_max_result_chars  = config.kb_max_result_chars;
        config_.system_prompt        = config.system_prompt;
        config_.instructions         = config.instructions;

        harness_.SetModel(config_.model);
        harness_.SetModelOptions(ChatOptions(config_));
        harness_.SetContextBudget(config_.num_ctx - config_.num_predict);
        harness_.SetMaxToolRounds(config_.max_tool_rounds);
        harness_.SetSystemPrompt(config_.system_prompt);
        rag_.SetLimits(config_.kb_max_passages, config_.kb_max_result_chars);
        if (kb_changed) {
            search_cache_.Clear();  // the results kept in memory have the old size
        }
    }

    // The tool handlers capture `this`.
    ChatAgent(const ChatAgent&)             = delete;
    ChatAgent& operator=(const ChatAgent&)  = delete;

    /*
        Adds a tool next to the built-in ones (AutonomousAgent adds finish). `ends_prompt`: see
        AgentHarness::RegisterTool. Call before the first Ask().
    */
    void RegisterTool(const std::string& name, const std::string& description, json parameters,
                      AgentHarness::ToolHandler handler, bool ends_prompt = false) {
        harness_.RegisterTool(name, description, std::move(parameters), std::move(handler), ends_prompt);
    }

    // What the last Ask() did: tool calls made, and whether the model replied at all.
    const AgentHarness::PromptStats& LastAsk() const { return harness_.LastPrompt(); }

    /*
        Tool calls in the last Ask() that repeat one already made in this conversation: a search with the same
        words (answered with a reminder, from memory, or run again when the earlier one failed), the same file
        written again with the same content, the same code run again. They bring nothing new, so
        AutonomousAgent does not count them as progress.
    */
    int LastAskRepeats() const { return repeated_actions_; }

    /*
        What the tools did since the last Ask(), one line each ("wrote C:\...\a.py", "ran: python a.py -> exit code
        0", "searched the internet for: x"), the last `max_lines` of it. AutonomousAgent puts it in its messages:
        a small model does not keep track of what it already did, and a goal restated alone sends it to do it all
        over again. Empty when the tools did nothing.
    */
    std::string TaskLog(size_t max_lines = 12) const {
        std::string text;
        const size_t first = task_log_.size() > max_lines ? task_log_.size() - max_lines : 0;
        for (size_t i = first; i < task_log_.size(); ++i) {
            text += "- " + task_log_[i] + "\n";
        }
        return text;
    }

    /*
        Sends one user message and returns the model's final reply, after any tool calls.
        Blocks for as long as the model takes (minutes on a slow machine): call it off the UI thread.
        Not safe to call from two threads at once.
    */
    std::string Ask(const std::string& prompt) {
        kb_searched_        = false;
        internet_searched_  = false;
        internet_requested_ = false;
        task_log_.clear();
        const FileTask file = FindFileTask(prompt);
        delete_task_ = file.deletes;
        if (file.Found()) {
            // The words found, so a message taken for a file task by mistake shows why.
            Report("[File task (\"" + file.action + "\", \"" + file.object + "\"): the searches are left out, so "
                   "the model goes straight to the file tools" +
                   (file.deletes ? std::string(". It asks to delete: delete_file, and write_file may not empty a "
                                               "file instead]")
                                 : std::string("]")));
            harness_.SetHiddenTools({"search_knowledge_base", "search_internet"});
        } else {
            harness_.SetHiddenTools({});
        }
        return ContinueTask(prompt);
    }

    /*
        Like Ask(), for a message that goes on with the same task: AutonomousAgent's next step. The search order
        carries over from the task's earlier messages instead of starting again: once the knowledge base has been
        searched, search_internet runs at once, and a request for the internet stays in force even when the message
        does not repeat it (a long goal is not restated in every step). So does a file task: the searches stay out
        for the whole run, and so do a request to delete (see WriteFile) and TaskLog().
        `only_tools`: offer only these tools for this message, with at most 2 rounds of tool calls: a short
        question, not a step of work (AutonomousAgent asks with finish alone whether the goal is done). Empty: the
        usual tools and rounds.
    */
    std::string ContinueTask(const std::string& prompt, std::unordered_set<std::string> only_tools = {}) {
        searched_this_prompt_.clear();
        repeated_actions_ = 0;
        if (!internet_requested_ && AsksForInternet(prompt)) {
            internet_requested_ = true;
            Report("[The message asks for the internet: it is searched first]");
        }
        const bool limited = !only_tools.empty();
        harness_.SetOnlyTools(std::move(only_tools));
        if (limited) {
            harness_.SetMaxToolRounds(std::min(2, config_.max_tool_rounds));
        }
        std::string reply = harness_.ProcessPrompt(prompt);
        harness_.SetOnlyTools({});
        harness_.SetMaxToolRounds(config_.max_tool_rounds);
        return reply;
    }

    /*
        Forgets the conversation and the search results kept in memory, and updates the knowledge base index on
        the next search, so documents added to the knowledge base since are found. Do not call while Ask() is
        running.
    */
    void NewConversation() {
        harness_.ResetConversation();
        search_cache_.Clear();
        actions_this_conversation_.clear();
        last_script_.clear();
        rag_synced_ = false;
    }

    /*
        Brings the knowledge base index up to date, then embeds every passage that has no vector yet. With
        bge-m3 on a laptop that is ~1.6 passages per second: hours for a large knowledge base, so it is a batch
        job (agent.exe --index), not part of a chat. Progress is saved as it goes; `cancel`, when set, stops it
        after the current batch, and the next run continues from there. Throws when the knowledge folder is
        missing. Do not call while Ask() is running.
    */
    std::string IndexKnowledgeBase(const ActivityHandler& progress = nullptr, const std::atomic<bool>* cancel = nullptr) {
        const RagIndex::SyncReport sync = rag_.Sync(progress);
        rag_synced_ = true;
        if (!rag_.CanEmbed()) {
            return sync.Summary() + "\nNo embedding model configured: search uses keywords only.";
        }
        return sync.Summary() + "\n" + rag_.EmbedPending(0, progress, cancel).Summary();
    }

    std::string Name() const {
        std::ostringstream out;
        out << config_.agent_name << " v" << std::fixed << std::setprecision(2) << config_.agent_version;
        return out.str();
    }
    const std::string& Model()      const { return config_.model; }
    const std::string& Endpoint()   const { return config_.endpoint; }
    const Config&      Settings()   const { return config_; }

    // One-line summary of this machine. Not used by the agent yet; kept from the original main.cpp.
    static std::string SystemDiagnostics() {
        std::ostringstream out;

        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
        if (rtlGetVersion) {
            rtlGetVersion(&vi);
        }
        const char* name = (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22000) ? "Windows 11" : "Windows";
        out << "OS: " << name << " " << vi.dwMajorVersion << "." << vi.dwMinorVersion
            << " (build " << vi.dwBuildNumber << ")";

        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            out << " | Total RAM: " << mem.ullTotalPhys / (1024 * 1024) << "MB"
                << " | Free RAM: " << mem.ullAvailPhys / (1024 * 1024) << "MB"
                << " | Memory Load: " << mem.dwMemoryLoad << "%";
        }

        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        out << " | Logical CPUs: " << si.dwNumberOfProcessors << " | Status: Nominal";
        return out.str();
    }

private:
    Config                config_;
    ActivityHandler       on_activity_;
    ConfirmHandler        confirm_;
    CodeExecutor::Limits  exec_limits_;
    AgentHarness          harness_;
    // Talks to the embedding model; kept apart from the harness's client, which sends the chat options.
    OllamaClient          embed_client_;
    // The knowledge base: passages of the saved documents, so a search returns a few of them, not documents.
    RagIndex              rag_;
    // rag_ has been brought up to date with the knowledge folder in this conversation.
    bool                  rag_synced_ = false;
    /*
        The search order is enforced here, not only in the prompt: a small model reorders prompt steps,
        and when a step errors it moves on to the next. Until the knowledge base has been searched for
        the current prompt (or, in a task, in any of its steps: see ContinueTask), search_internet searches
        the knowledge base instead.
    */
    bool                  kb_searched_ = false;
    /*
        The order is reversed when the user's message asks for the internet: until the internet has been
        searched for the current prompt (or task), search_knowledge_base searches the internet instead.
    */
    bool                  internet_requested_ = false;
    bool                  internet_searched_ = false;
    // The prompt (or task) asks to delete a file (file_delete_actions): write_file does not empty one; see WriteFile.
    bool                  delete_task_ = false;
    // Full search results for the whole conversation (the context keeps only previews of old ones).
    ToolResultCache       search_cache_;
    // Searches already answered for the current prompt, by cache key.
    std::unordered_set<std::string> searched_this_prompt_;
    /*
        Every search of the conversation, by cache key, whether it worked or not (a failed one is not in
        search_cache_), every file written with its content and every code run; and how many tool calls of the
        last Ask() were already in it; see LastAskRepeats.
    */
    std::unordered_set<std::string> actions_this_conversation_;
    int                             repeated_actions_ = 0;
    std::vector<std::string>        task_log_;     // see TaskLog
    /*
        The script write_file saved last, as execute_code runs it (its name when it is in the workspace): what a
        bare "python" runs; see SavedScriptCommand.
    */
    std::string                     last_script_;

    // Counts `key` (tool and arguments) as a repeat when the conversation already made that call.
    void NoteAction(const std::string& key) {
        if (!actions_this_conversation_.insert(key).second) {
            ++repeated_actions_;
        }
    }

    // "print('a')\nprint('b')" -> "print('a') ...", for a TaskLog line.
    static std::string FirstLine(const std::string& text, size_t max_chars = 80) {
        std::string line = TrimSpace(text);
        const size_t end = line.find('\n');
        const bool   more = end != std::string::npos || line.size() > max_chars;
        line = line.substr(0, std::min(end, max_chars));
        return more ? TrimSpace(line) + " ..." : line;
    }

    struct SearchTool {
        const char* id;       // names the tool in the cache key
        const char* label;    // shown in the activity log
        const char* prefix;   // put before the results given to the model
        const char* repeat;   // returned instead of the same results again within one prompt
    };

    static constexpr SearchTool kKnowledgeBase{
        "knowledge_base", "Searching the knowledge base", "Knowledge base results:\n",
        "You already searched the knowledge base for these words while answering this question; the results "
        "are above. Do not repeat the search. Answer from those results, search with different words, or, if "
        "the knowledge base has nothing on the subject, call search_internet."};

    static constexpr SearchTool kInternet{
        "internet", "Searching the internet", "Internet results:\n",
        "You already searched the internet for this while answering this question; the results are above. "
        "Do not repeat the search. Answer from those results, try a different query, or say that the sources "
        "do not have the answer."};

    void Report(const std::string& text) const {
        if (on_activity_) {
            on_activity_(text);
        } else {
            TextUtilities::print("\n" + text + "\n");
        }
    }

    static std::string StringArg(const json& args, const char* key) {
        auto it = args.find(key);
        return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string();
    }

    // Schema of a tool that takes one string parameter.
    static json TextParam(const char* name, const char* description) {
        return {
            {"type", "object"},
            {"properties", {{name, {{"type", "string"}, {"description", description}}}}},
            {"required", json::array({name})},
        };
    }

    struct ScriptResult {
        std::string text;
        bool        ok = false;  // the search completed (a script exited with code 0): safe to keep in memory
    };

    // The options sent with every chat request.
    static json ChatOptions(const Config& config) {
        return {{"num_ctx", config.num_ctx}, {"num_predict", config.num_predict}};
    }

    static RagIndex::Config MakeRagConfig(const Config& config) {
        RagIndex::Config rag;
        rag.knowledge_dir    = config.knowledge_dir;
        rag.index_path       = config.rag_index;
        rag.embed_model      = config.embed_model;
        rag.max_passages     = config.kb_max_passages;
        rag.max_result_chars = config.kb_max_result_chars;
        return rag;
    }

    /*
        Runs a script with the model's text as its last argument, after `options` (built by the program, never
        from the model's text). No shell is involved, and the text is reduced to letters, digits and dashes, so
        it cannot break out of the quoted argument; a "--" before it keeps a leading dash from reading as an option.
    */
    ScriptResult RunSearchScript(const char* script, const std::string& text, const std::string& options = {}) const {
        std::string words;
        for (unsigned char c : text) {
            words += (std::isalnum(c) || c == '-' || c >= 0x80) ? static_cast<char>(c) : ' ';
        }
        if (words.find_first_not_of(' ') == std::string::npos) {
            return {"Error: no search words given.", false};
        }

        // -X utf8: writing to a pipe, Python would use the ANSI code page and mangle accents.
        const std::string command =
            "python -X utf8 \"" + config_.scripts_dir + script + "\" " + options + " -- \"" + words + "\"";
        auto res = CodeExecutor::ExecuteCommand(command, exec_limits_);
        if (!res.Started()) {
            return {"Error: " + res.error, false};
        }
        if (res.timed_out) {
            return {"Error: the search timed out.", false};
        }

        std::string out = res.stdout_str.empty() ? "(no output)\n" : res.stdout_str;
        if (res.exit_code != 0 && !res.stderr_str.empty()) {
            out += "\n--- stderr ---\n" + res.stderr_str;
        }
        return {out, res.exit_code == 0};
    }

    /*
        Every search goes through here.
        - Same search again within one prompt: a short reminder instead of the same passages. A small model
          otherwise loops on it, and each copy takes ~1000 tokens of the 8192-token context.
        - Same search in a later prompt: the result comes from memory. The harness has shortened it in the
          context by then, so the model may legitimately need it again; no need to rerun the script.
        Failures (network down, timeout) are not kept, so the next attempt runs the search again.
        `run` performs the search; it is called only when neither of the above applies.
    */
    template <class Run>
    std::string Search(const SearchTool& tool, const std::string& query, Run run) {
        const std::string key = ToolResultCache::MakeKey(tool.id, query);
        NoteAction(key);

        if (!searched_this_prompt_.insert(key).second) {
            Report(std::string("[Repeated search, not run again: ") + query + "]");
            return tool.repeat;
        }
        task_log_.push_back(std::string(&tool == &kInternet ? "searched the internet for: "
                                                            : "searched the knowledge base for: ") + query);
        if (const std::string* kept = search_cache_.Find(key)) {
            Report(std::string("[") + tool.label + " (from memory): " + query + "]");
            return tool.prefix + *kept;
        }

        Report(std::string("[") + tool.label + ": " + query + "]");
        ScriptResult result = run();
        if (result.ok) {
            search_cache_.Store(key, result.text);
        }
        return tool.prefix + result.text;
    }

    /*
        True when the user's message explicitly asks for the internet: "search the internet", "look it up online",
        "check Wikipedia", "pesquise na internet", "busque na web". Plain word matching, because a small model
        does not reliably act on this from the prompt alone. Bare "web" is not enough ("web app"); it needs a
        word before it such as "the web" or "na web". Both word lists are in config\settings.txt.
    */
    bool AsksForInternet(const std::string& prompt) const {
        const std::vector<std::string> words = Words(prompt);
        for (size_t i = 0; i < words.size(); ++i) {
            if (Listed(config_.internet_words, words[i])) {
                return true;
            }
            if (words[i] == "web" && i > 0 && Listed(config_.words_before_web, words[i - 1])) {
                return true;
            }
        }
        return false;
    }

    // What FindFileTask found in a message.
    struct FileTask {
        std::string action;           // the word of file_task_actions that makes it a file task ("apague")
        std::string object;           // and the word of file_task_objects ("arquivo", "py")
        bool        deletes = false;  // it has a word of file_delete_actions, file task or not

        bool Found() const { return !action.empty(); }
    };

    /*
        Whether the message asks to create, change, delete or run a file: it has a word of file_task_actions and
        one of file_task_objects ("create a python script", "apague o arquivo velho.txt"), and it does not ask for
        a search too (the internet, or a word of search_words). Apart from that, whether it asks to delete
        (file_delete_actions). Plain word matching, as AsksForInternet; the lists are in config\settings.txt.
    */
    FileTask FindFileTask(const std::string& prompt) const {
        const std::vector<std::string> words = Words(prompt);
        FileTask task;
        task.deletes = !FirstListed(words, config_.file_delete_actions).empty();
        if (!FirstListed(words, config_.search_words).empty() || AsksForInternet(prompt)) {
            return task;
        }
        const std::string action = FirstListed(words, config_.file_task_actions);
        const std::string object = FirstListed(words, config_.file_task_objects);
        if (!action.empty() && !object.empty()) {
            task.action = action;
            task.object = object;
        }
        return task;
    }

    // The first of `words` that is in `list`; empty when none is.
    static std::string FirstListed(const std::vector<std::string>& words, const std::vector<std::string>& list) {
        for (const std::string& word : words) {
            if (Listed(list, word)) {
                return word;
            }
        }
        return {};
    }

    /*
        The words of `text`, ASCII letters lowered: "Crie hello.py!" -> {"crie", "hello", "py"}. An underscore
        stays in its word, so a file name is not read as words of the lists: edit_arquivo.py gives "edit_arquivo"
        and "py", not "edit" (an action) and "arquivo".
    */
    static std::vector<std::string> Words(const std::string& text) {
        std::vector<std::string> words;
        std::string word;
        for (unsigned char c : text) {
            if (std::isalnum(c) || c == '_' || c >= 0x80) {
                word += static_cast<char>(std::tolower(c));
            } else if (!word.empty()) {
                words.push_back(word);
                word.clear();
            }
        }
        if (!word.empty()) {
            words.push_back(word);
        }
        return words;
    }

    static bool Listed(const std::vector<std::string>& list, const std::string& word) {
        return std::find(list.begin(), list.end(), word) != list.end();
    }

    std::string SearchKnowledgeBase(const std::string& query) {
        if (internet_requested_ && !internet_searched_) {
            Report("[search_knowledge_base called, but the user asked for the internet]");
            return "The knowledge base was NOT searched: the user asked for the internet, so the internet was "
                   "searched instead. " + SearchWeb(query) +
                   "\nAnswer from these results and say they came from the internet. Call search_knowledge_base "
                   "again only if the user also asked about their own documents.";
        }
        kb_searched_ = true;
        return Search(kKnowledgeBase, query, [&] { return SearchDocuments(query); });
    }

    /*
        The passages of the saved documents that best match the query, by keywords and by meaning. On the first
        search of a conversation the index catches up with the knowledge folder: documents added, changed or
        removed since. New passages are found by keywords at once; by meaning only after IndexKnowledgeBase()
        embeds them, which is too slow to do here.
    */
    ScriptResult SearchDocuments(const std::string& query) {
        try {
            if (!rag_synced_) {
                const RagIndex::SyncReport sync = rag_.Sync([this](const std::string& s) { Report("[" + s + "]"); });
                rag_synced_ = true;
                if (sync.changed) {
                    Report("[" + sync.Summary() + "]");
                }
                const size_t pending = rag_.PendingCount();
                if (rag_.CanEmbed() && pending > 0) {
                    Report("[" + std::to_string(pending) + " of " + std::to_string(rag_.PassageCount()) +
                           " passages are not embedded yet, so they are found by keywords only. "
                           "Run `agent.exe --index` to embed them.]");
                }
            }
            const RagIndex::SearchResult result = rag_.Search(query);
            if (!result.warning.empty()) {
                Report("[" + result.warning + "]");
            }
            // A keywords-only result, from a moment when the embedding model did not answer, is not kept.
            return {result.text, result.warning.empty()};
        } catch (const std::exception& e) {
            return {std::string("Error: the knowledge base could not be searched: ") + e.what(), false};
        }
    }

    std::string ExecuteCode(const json& args) {
        std::string lang = StringArg(args, "language");
        std::string code = StringArg(args, "code");
        if (lang.empty()) {
            lang = "cmd";
        }
        if (code.empty()) {
            return "Error: No code provided.";
        }
        const std::string command = SavedScriptCommand(lang, code);
        if (!command.empty()) {
            Report("[The code is a saved script's name, not code: running the script, " + command + "]");
            lang = "cmd";
            code = command;
        } else if (IsBareInterpreter(code)) {
            // It would read its input, and there is none: nothing runs, and the model learns nothing.
            task_log_.push_back("ran " + TrimSpace(code) + " alone: nothing to run");
            return "Not run: \"" + TrimSpace(code) + "\" alone has nothing to run, since it gets no input here. "
                   "Give the script to run, such as: python name.py";
        }
        NoteAction("execute_code|" + Lower(lang) + "|" + TrimSpace(code));

        /*
            Code that could do harm runs only if the user says yes. The model is told not to try another way: a
            small model otherwise rewrites the same thing until it is no longer flagged.
        */
        const CLISecurityChecking::Report risk = CLISecurityChecking::Check(lang, code);
        if (risk.Flagged()) {
            // One line: the front end's question shows the details.
            Report("[Potentially harmful code (" + lang + "): " + risk.Harms() + ". Asking the user]");
            if (!confirm_ || !confirm_(lang, code, risk)) {
                Report("[Not run: the user did not allow it]");
                task_log_.push_back("not allowed to run: " + FirstLine(code));
                return "Not run: the user did not allow this code. It was flagged because it " + risk.Harms() +
                       ". Do not try to do the same thing in another way. Tell the user what you wanted to do and "
                       "why, and go on without it.";
            }
            Report("[Allowed by the user]");
        }

        Report("[Executing Code (" + lang + ")]\n" + code);

        // In the workspace folder, so the files the code makes with a relative path are found there.
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(config_.workspace_dir), ec);
        auto res = CodeExecutor::ExecuteCode(lang, code, exec_limits_, config_.workspace_dir);
        if (!res.Started()) {
            task_log_.push_back("could not run: " + FirstLine(code));
            return "Error: " + res.error;
        }
        task_log_.push_back("ran: " + FirstLine(code) + " -> " +
                            (res.timed_out ? std::string("timed out")
                                           : "exit code " + std::to_string(res.exit_code) +
                                                 (res.stdout_str.empty() && res.stderr_str.empty() ? ", no output" : "")));

        std::ostringstream out;
        if (res.timed_out) {
            out << "Timed out after " << exec_limits_.timeout_ms
                << " ms; the process tree was killed. Partial output follows.\n";
        } else {
            out << "Exit code: " << res.exit_code << "\n";
        }
        out << "Ran in: " << config_.workspace_dir << "\n";
        out << "Elapsed: " << res.elapsed_ms << " ms\n";
        out << "--- stdout ---\n" << (res.stdout_str.empty() ? "(empty)\n" : res.stdout_str);
        if (!res.stderr_str.empty()) {
            out << "\n--- stderr ---\n" << res.stderr_str;
        }
        if (res.truncated) {
            out << "\n[output truncated]";
        }
        return out.str();
    }

    /*
        The write_file tool: saves `content` as the file at `path`, creating its folders. A path without a drive is
        relative to the workspace folder. Without it, a model asked for a script put the script in execute_code,
        which ran it once and saved nothing.
        The user is asked first, as for flagged code, when the file goes outside the workspace folder (among their
        own files) or is a script whose code CLISecurityChecking flags: execute_code would later run it with a
        command that shows nothing of it (python "script.py"), so its code is checked here.
    */
    std::string WriteFile(const json& args) {
        namespace fs = std::filesystem;
        const std::string given   = StringArg(args, "path");
        const auto        content = args.find("content");
        if (given.empty()) {
            return "Error: no path given. Call write_file again with the file's path and name in path.";
        }
        if (content == args.end() || !content->is_string()) {
            return "Error: no content given. Call write_file again with the whole text of the file in content.";
        }

        std::string    bad_path;
        const fs::path path = ToolPath(given, "write_file", bad_path);
        if (path.empty()) {
            return bad_path;
        }
        const fs::path    workspace = fs::u8path(config_.workspace_dir);
        const std::string shown     = path.u8string();
        std::error_code ec;
        if (!path.has_filename() || fs::is_directory(path, ec)) {
            return "Error: " + shown + " is a folder. Call write_file again with the path of a file in it, its "
                   "name included.";
        }
        const bool replaces = fs::exists(path, ec);
        NoteAction("write_file|" + Lower(shown) + "|" + content->get<std::string>());

        /*
            Asked to delete a file, a small model writes it with no content, which only empties it, and then says
            it is gone. After NoteAction, so the same call again counts as a repeat (AutonomousAgent stops on those).
        */
        if (replaces && delete_task_ && TrimSpace(content->get<std::string>()).empty()) {
            Report("[write_file with no content on " + shown + ", in a message that asks to delete: not written]");
            return "Not written: write_file does not delete files. With no content it would only empty " + shown +
                   ", and the file would still be there. To delete it, call delete_file with path: " + given;
        }

        CLISecurityChecking::Report report;
        report.file = shown;
        if (const char* lang = ScriptLanguage(path)) {
            report.findings = CLISecurityChecking::Check(lang, content->get<std::string>()).findings;
        }
        // After the script's findings, so a DANGER among them stays first.
        if (InSystemFolder(path)) {
            report.findings.push_back({CLISecurityChecking::Severity::Danger, "Windows",
                                       "writes into Windows or the installed programs", 0, shown, {}});
        } else if (!IsInside(path, workspace)) {
            report.findings.push_back({CLISecurityChecking::Severity::Caution, "Any",
                                       replaces ? "replaces a file outside the workspace folder"
                                                : "writes a file outside the workspace folder",
                                       0, "the workspace folder is " + workspace.u8string(), {}});
        }
        if (report.Flagged()) {
            Report("[write_file " + shown + ": " + report.Harms() + ". Asking the user]");
            if (!confirm_ || !confirm_("text", content->get<std::string>(), report)) {
                Report("[Not written: the user did not allow it]");
                task_log_.push_back("not allowed to write " + shown);
                return "Not written: the user did not allow writing " + shown + ". It was flagged because it " +
                       report.Harms() + ". Do not try to write it in another way. Tell the user what you wanted to "
                       "write and where, and go on without it.";
            }
            Report("[Allowed by the user]");
        }

        /*
            As CodeExecutor writes its scripts: Windows PowerShell 5.1 reads a .ps1 without a BOM as ANSI, and cmd
            mis-parses labels in a .cmd or .bat with LF-only lines.
        */
        const std::string ext   = Lower(path.extension().u8string());
        const std::string& text = content->get_ref<const std::string&>();
        const bool        crlf  = ext == ".cmd" || ext == ".bat";
        std::string       bytes;
        if (ext == ".ps1" && text.compare(0, 3, "\xEF\xBB\xBF") != 0) {
            bytes = "\xEF\xBB\xBF";
        }
        for (size_t i = 0; i < text.size(); ++i) {
            if (crlf && text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
                bytes += '\r';
            }
            bytes += text[i];
        }

        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return "Error: could not create the folder " + path.parent_path().u8string() + ": " + ec.message();
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            return "Error: could not write " + shown + " (in use by another program, or no permission).";
        }

        Report("[" + std::string(replaces ? "Replaced " : "Created ") + shown + "]");
        std::string result = std::string(replaces ? "Replaced " : "Created ") + shown + " (" +
                             std::to_string(bytes.size()) + " bytes).";
        const std::string error = PythonSyntaxError(path);
        if (!error.empty()) {
            Report("[The file is not valid Python: " + error + "]");
            task_log_.push_back("wrote " + shown + ", but it is not valid Python (" + error + ")");
            return result + " But Python cannot read it: " + error + ". The content was probably cut at a double "
                   "quote. Call write_file again with the whole content, writing text between single quotes "
                   "('Hello').";
        }
        task_log_.push_back("wrote " + shown);
        // A file in the workspace runs by its name: execute_code runs there.
        const fs::path    name = IsInside(path, workspace) ? path.lexically_relative(workspace) : path;
        const std::string run  = RunCommand(name);
        if (!run.empty()) {
            last_script_ = name.u8string();
            result += " If the user asked to run it, call execute_code with language cmd and code: " + run;
        }
        return result;
    }

    /*
        The full path a file tool (`tool`: write_file, delete_file) names: a path without a drive is in the
        workspace folder. Empty, with `error` for the model, when it is neither full nor a name in the workspace:
        "C:file.py" and "\file.py" depend on the current drive or folder.
    */
    std::filesystem::path ToolPath(const std::string& given, const char* tool, std::string& error) const {
        namespace fs = std::filesystem;
        fs::path path = fs::u8path(given);
        if (path.has_root_name() != path.has_root_directory()) {
            error = "Error: \"" + given + "\" is not a full path (C:\\folder\\file.py) nor a name in the workspace "
                    "folder (file.py). Call " + tool + " again with one of those.";
            return {};
        }
        if (path.is_relative()) {
            path = fs::u8path(config_.workspace_dir) / path;
        }
        return path.lexically_normal();
    }

    /*
        The delete_file tool: deletes the file at `path`, or an empty folder. A path without a drive is in the
        workspace folder. Without it, a model asked to delete a file wrote it with no content (see WriteFile).
        The user is always asked first, in the workspace too: a deleted file does not come back, and execute_code
        asks before a del as well. A folder with files in it is not deleted: a whole tree is for the user to delete.
    */
    std::string RemoveFile(const json& args) {  // not DeleteFile: windows.h makes that DeleteFileW
        namespace fs = std::filesystem;
        const std::string given = StringArg(args, "path");
        if (given.empty()) {
            return "Error: no path given. Call delete_file again with the file's path and name in path.";
        }
        std::string    error;
        const fs::path path = ToolPath(given, "delete_file", error);
        if (path.empty()) {
            return error;
        }
        const fs::path    workspace = fs::u8path(config_.workspace_dir);
        const std::string shown     = path.u8string();
        NoteAction("delete_file|" + Lower(shown));

        std::error_code       ec;
        const fs::file_status status = fs::symlink_status(path, ec);
        if (!fs::exists(status)) {
            return "Nothing deleted: " + shown + " does not exist. Check the name the user gave; if you deleted it "
                   "earlier, that is done.";
        }
        const bool folder = fs::is_directory(status);
        if (folder && !fs::is_empty(path, ec)) {
            return "Not deleted: " + shown + " is a folder with files in it, and delete_file deletes one file or an "
                   "empty folder. Tell the user, and go on without it.";
        }

        CLISecurityChecking::Report report;
        report.file    = shown;
        report.deletes = true;
        if (InSystemFolder(path)) {
            report.findings.push_back({CLISecurityChecking::Severity::Danger, "Windows",
                                       "deletes from Windows or the installed programs", 0, shown, {}});
        } else if (!IsInside(path, workspace)) {
            report.findings.push_back({CLISecurityChecking::Severity::Caution, "Any",
                                       folder ? "deletes a folder outside the workspace folder"
                                              : "deletes a file outside the workspace folder",
                                       0, "the workspace folder is " + workspace.u8string(), {}});
        } else {
            report.findings.push_back({CLISecurityChecking::Severity::Caution, "Any",
                                       folder ? "deletes a folder" : "deletes a file", 0, shown, {}});
        }
        Report("[delete_file " + shown + ": " + report.Harms() + ". Asking the user]");
        if (!confirm_ || !confirm_("text", std::string(), report)) {
            Report("[Not deleted: the user did not allow it]");
            task_log_.push_back("not allowed to delete " + shown);
            return "Not deleted: the user did not allow deleting " + shown + ". Do not try to delete it in another "
                   "way. Tell the user, and go on without it.";
        }
        Report("[Allowed by the user]");

        fs::remove(path, ec);
        std::error_code gone;
        if (ec || fs::exists(fs::symlink_status(path, gone))) {
            return "Error: could not delete " + shown + " (in use by another program, or no permission)" +
                   (ec ? ": " + ec.message() : std::string()) + ".";
        }
        Report("[Deleted " + shown + "]");
        task_log_.push_back("deleted " + shown);
        return "Deleted " + shown + ".";
    }

    /*
        For a .py file: what Python finds wrong with its syntax ("line 1: '(' was never closed"), empty when it
        reads it, or when the file is not Python or Python cannot be started. A small model writes a double quote
        unescaped in the JSON of its tool call, and the content arrives cut there (print("Hello") as print( ):
        this tells it so, instead of a later run failing with no clue why.
    */
    std::string PythonSyntaxError(const std::filesystem::path& path) const {
        if (Lower(path.extension().u8string()) != ".py") {
            return {};
        }
        CodeExecutor::Limits limits = exec_limits_;
        limits.timeout_ms = 5000;
        const std::string command = "python -X utf8 -c \"import ast,sys; ast.parse(open(sys.argv[1],'rb').read(), "
                                    "sys.argv[1])\" \"" + path.u8string() + "\"";
        const CodeExecutor::ExecutionResult res = CodeExecutor::ExecuteCommand(command, limits);
        if (!res.Started() || res.timed_out || res.exit_code == 0) {
            return {};
        }
        // The traceback ends: File "...", line N / the line / ^ / SyntaxError: message.
        std::string line, message;
        std::istringstream in(res.stderr_str);
        for (std::string l; std::getline(in, l);) {
            const size_t at = l.find("\", line ");
            if (at != std::string::npos) {
                line = l.substr(at + 8);
            } else if (l.find("Error: ") != std::string::npos && l[0] != ' ') {
                message = l.substr(l.find("Error: ") + 7);
            }
        }
        if (message.empty()) {
            return {};  // not a syntax error (Python itself failed): nothing to tell the model
        }
        return (line.empty() ? std::string() : "line " + line + ": ") + message;
    }

    /*
        Asked to run a saved script, a small model passes its name as the code (language python, code hello.py,
        which Python reads as a variable: NameError), the command as Python code (python hello.py: SyntaxError),
        or the interpreter alone (python, which runs nothing). When the code is one line naming an existing
        script, alone or after python / py, or is "python" alone after write_file saved a .py, this is the command
        that runs that script; otherwise empty, and the code runs as given.
    */
    std::string SavedScriptCommand(const std::string& lang, const std::string& code) const {
        namespace fs = std::filesystem;
        std::string line = TrimSpace(code);
        if (line.empty() || line.find('\n') != std::string::npos) {
            return {};
        }
        // "python" alone right after write_file saved a script: it meant that script.
        if (IsBareInterpreter(line)) {
            const fs::path last = fs::u8path(last_script_);
            return Lower(last.extension().u8string()) == ".py" ? RunCommand(last) : std::string();
        }
        // "python hello.py": a command, which cmd and PowerShell run as it is; only Python reads it as code.
        bool after_python = false;
        for (const std::string prefix : {"python ", "py "}) {
            if (Lower(line).compare(0, prefix.size(), prefix) == 0) {
                if (Lower(lang) != "python" && Lower(lang) != "py") {
                    return {};
                }
                line         = TrimSpace(line.substr(prefix.size()));
                after_python = true;
                break;
            }
        }
        if (line.size() > 1 && line.front() == '"' && line.back() == '"') {
            line = line.substr(1, line.size() - 2);
        }
        const fs::path path = fs::u8path(line);
        if (line.find_first_of("\"<>|&") != std::string::npos ||
            (after_python && Lower(path.extension().u8string()) != ".py")) {
            return {};
        }
        std::error_code ec;
        if (!fs::is_regular_file(path.is_relative() ? fs::u8path(config_.workspace_dir) / path : path, ec)) {
            return {};
        }
        return RunCommand(path);  // empty for a file that is not run that way
    }

    // The Python interpreter's name alone, with nothing to run: "python", "py", "python3".
    static bool IsBareInterpreter(const std::string& code) {
        const std::string name = Lower(TrimSpace(code));
        return name == "python" || name == "py" || name == "python3" || name == "python.exe";
    }

    static std::string TrimSpace(const std::string& s) {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    }

    // The language CLISecurityChecking reads a script in, from its extension; null for a file that is not a script.
    static const char* ScriptLanguage(const std::filesystem::path& path) {
        const std::string ext = Lower(path.extension().u8string());
        if (ext == ".py" || ext == ".pyw") {
            return "python";
        }
        if (ext == ".ps1" || ext == ".psm1") {
            return "powershell";
        }
        if (ext == ".cmd" || ext == ".bat" || ext == ".sh" || ext == ".js" || ext == ".vbs") {
            return "cmd";  // read like cmd and PowerShell, which covers the Linux shell rules too
        }
        return nullptr;
    }

    // The command that runs the script at `path`, for execute_code; empty for a file that is not run that way.
    static std::string RunCommand(const std::filesystem::path& path) {
        const std::string ext    = Lower(path.extension().u8string());
        const std::string quoted = "\"" + path.u8string() + "\"";
        if (ext == ".py") {
            return "python " + quoted;
        }
        if (ext == ".ps1") {
            return "powershell -NoProfile -ExecutionPolicy Bypass -File " + quoted;
        }
        if (ext == ".cmd" || ext == ".bat") {
            return "call " + quoted;
        }
        return {};
    }

    // `path` is `folder` or inside it. Both full and normalized; Windows paths ignore case.
    static bool IsInside(const std::filesystem::path& path, const std::filesystem::path& folder) {
        const std::string p = Lower(path.lexically_normal().u8string());
        std::string       f = Lower(folder.lexically_normal().u8string());
        if (!f.empty() && f.back() != '\\') {
            f += '\\';
        }
        return p.compare(0, f.size(), f) == 0 || p + '\\' == f;
    }

    // Under Windows or the installed programs: %SystemRoot%, %ProgramFiles%, %ProgramFiles(x86)%, %ProgramData%.
    static bool InSystemFolder(const std::filesystem::path& path) {
        for (const char* name : {"SystemRoot", "ProgramFiles", "ProgramFiles(x86)", "ProgramData"}) {
            const char* dir = std::getenv(name);
            if (dir && *dir && IsInside(path, std::filesystem::path(dir))) {
                return true;
            }
        }
        return false;
    }

    static std::string Lower(std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    std::string SearchInternet(const json& args) {
        const std::string query = StringArg(args, "query");
        if (!kb_searched_ && !internet_requested_) {
            Report("[search_internet called before the knowledge base was searched]");
            return "The internet was NOT searched: the knowledge base must be searched first. " +
                   SearchKnowledgeBase(query) +
                   "\nIf these passages answer the question, answer from them. "
                   "If they are about something else, call search_internet again.";
        }
        return SearchWeb(query);
    }

    std::string SearchWeb(const std::string& query) {
        internet_searched_ = true;
        return Search(kInternet, query, [&] { return RunSearchScript("websearch.py", query, WebSearchOptions()); });
    }

    /*
        websearch.py's options, from the web_* settings. Its budget ends 1.5 s before code_timeout_ms kills it
        (Python's start and the output take the rest), so it prints what it has instead of being cut off.
    */
    std::string WebSearchOptions() const {
        std::string sources;
        for (const std::string& source : config_.web_sources) {
            std::string name;
            for (const unsigned char c : source) {
                if (std::isalnum(c) || c == '_' || c == '-') {
                    name += static_cast<char>(c);
                }
            }
            if (!name.empty()) {
                sources += (sources.empty() ? "" : ",") + name;
            }
        }
        const unsigned budget_ds = config_.code_timeout_ms > 3500 ? (config_.code_timeout_ms - 1500) / 100 : 20;
        std::string options = "--pages " + std::to_string(config_.web_pages_to_read) +
                              " --max-chars " + std::to_string(config_.web_max_result_chars) +
                              " --budget " + std::to_string(budget_ds / 10) + "." + std::to_string(budget_ds % 10);
        if (!sources.empty()) {
            options += " --sources " + sources;
        }
        return options;
    }

    void RegisterTools() {
        json exec_params = {
            {"type", "object"},
            {"properties", {
                {"language", {
                    {"type", "string"},
                    {"description", "Programming language or shell: python, powershell, or cmd"}
                }},
                {"code", {
                    {"type", "string"},
                    {"description", "Raw source code or command string to execute."}
                }}
            }},
            {"required", json::array({"language", "code"})}
        };

        json write_params = {
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description", "Only the file name, such as hello.py, which puts it in the workspace folder. "
                                    "When the user named a folder: that folder followed by the file name."}
                }},
                {"content", {
                    {"type", "string"},
                    {"description", "The whole text of the file. In code, prefer single quotes for text."}
                }}
            }},
            {"required", json::array({"path", "content"})}
        };

        // The descriptions the model reads are in config\settings.txt (tool_* keys).
        harness_.RegisterTool(
            "write_file", config_.tool_write_file, write_params,
            [this](const json& args) { return WriteFile(args); });

        harness_.RegisterTool(
            "delete_file", config_.tool_delete_file,
            TextParam("path", "The file's name as the user gave it: the name alone for a file in the workspace "
                              "folder; when the user named a folder, that folder followed by the name."),
            [this](const json& args) { return RemoveFile(args); });

        harness_.RegisterTool(
            "execute_code", config_.tool_execute_code, exec_params,
            [this](const json& args) { return ExecuteCode(args); });

        harness_.RegisterTool(
            "search_knowledge_base", config_.tool_search_knowledge_base,
            TextParam("query", "The question in a few words, or its main words."),
            [this](const json& args) { return SearchKnowledgeBase(StringArg(args, "query")); });

        harness_.RegisterTool(
            "search_internet", config_.tool_search_internet,
            TextParam("query", "A short search query."),
            [this](const json& args) { return SearchInternet(args); });
    }
};
