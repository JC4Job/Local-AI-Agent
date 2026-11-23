#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "framework/FileSystem.hpp"
#include "sys/Sys.hpp"

/*
 * ChatAgentConfig - the agent's settings.
 *
 * Every value in the struct is a built-in default. At startup Load() overrides it from these files, in this
 * order, the last one winning:
 *
 *   1. config\settings.txt
 *   2. config\machines\<computer name>.txt   this computer's own file (Settings window of agent_gui.exe),
 *                                             so each computer can have its own context and reply sizes
 *   3. config\agents\<name>.txt               an agent made in the window (CreateAgentFile): its name, its
 *                                             model, the settings it has its own of (SaveAgentSettings), and
 *                                             after its --- line the instructions added to its system prompt
 *
 * A key the files leave out keeps its default; with no files the agent runs on the defaults alone. Changing
 * the model or the prompt takes a restart, not a rebuild.
 *
 * The directives the model follows (which tool for which request, how to answer, how to work alone) are not in
 * the code: they are config\system_prompt.txt and config\autonomous_prompt.txt. config\defaults\ holds their
 * factory copies, read when a prompt file is missing and written back by restore_config.exe (with
 * DefaultSettingsText() for settings.txt). With neither, the short built-in kFallbackSystemPrompt is used.
 *
 * Every path starts at the program's folder (framework/FileSystem.hpp): the executable's folder, or its parent
 * when the executable is in bin\. The config folder, the scripts and the knowledge base live there, so the
 * searches follow the compiled program wherever the project is copied.
 *
 * NOTE: the default agent's settings are every agent's base. An agent file overrides only the keys it has.
 *
 * WARNING: run.ps1 reads the same settings.txt and keeps its own copy of the model and size defaults, so the
 * models it preloads get the options the agent sends. Change one of those defaults here and change it there
 * too: Ollama reloads a model whose num_ctx changes, which costs the preload.
 */
struct ChatAgentConfig {
    /*
     * ---- Built-in defaults ----
     *
     * Every setting starts with its value here; Load() then overrides it from the settings files. A number
     * carries the range Set() accepts from a file: {default, min, max}.
     *
     * WARNING: a setting added here must also be added to Set() and DefaultSettingsText(). Set() rejects a key
     * it does not know, so the files could not change it, and restore_config.exe writes settings.txt from
     * DefaultSettingsText(), so a restored config would not show it.
     */
    template <class Number>
    struct NumberDefault {
        Number    value;
        long long min;
        long long max;
    };

    /*
     * Shown after the name in the banner (ChatAgent.hpp).
     *
     * WARNING: the banner prints it with 2 decimals, so 0.001 shows as "v0.00".
     */
    static constexpr float       kAgentVersion = 0.001f;
    static constexpr const char* kAgentName    = "Sof_IA";
    static constexpr const char* kEndpoint     = "http://127.0.0.1:11434/api/chat";
    /*
     * The chat model. It needs Ollama's 'tools' capability (check with: ollama show <model>).
     * Also tried: qwen2.5:3b.
     *
     * WARNING: gemma:2b does not work: it is too small for the prompt payloads (system prompt, tool schemas
     * and search results together).
     */
    static constexpr const char* kModel        = "llama3.2";
    // Local embedding model (in Ollama) for search by meaning; empty: keywords only.
    static constexpr const char* kEmbedModel   = "bge-m3";

    /*
     * Sizes, in tokens (about 3/4 of a word each).
     *
     * num_ctx:       Ollama's context window, shared by the system prompt, the tools, the conversation, the
     *                search results and the reply.
     * num_predict:   the longest reply. The harness shortens old tool results to keep this much free.
     * embed_num_ctx: passages are ~300 tokens, so 1024 is plenty and leaves more of the small GPU free.
     *
     * WARNING: do not go back to Ollama's default num_ctx of 4096: search results overflow it.
     *
     * WARNING: num_predict must stay below num_ctx (Load() falls back to num_ctx / 4 otherwise). Do not remove
     * the cap: an overflowing reply then runs until the HTTP timeout.
     */
    //                                                              default  min   max
    static constexpr NumberDefault<int>      kNumCtx             = {16384,   1024, 1 << 20};
    static constexpr NumberDefault<int>      kNumPredict         = {4096,    64,   1 << 20};
    static constexpr NumberDefault<int>      kEmbedNumCtx        = {1024,    256,  1 << 16};
    /*
     * max_tool_rounds:      rounds of tool calls per question before the model is told to answer with what it
     *                       has.
     * code_timeout_ms:      the longest run of execute_code and of the search scripts.
     * autonomous_max_steps: the most steps one goal may take in autonomous mode (agent.exe --auto, Run task in
     *                       the window; AutonomousAgent.hpp). A step is one message the program sends in place
     *                       of the user.
     *
     * WARNING: each autonomous step is answered with up to max_tool_rounds rounds of tool calls, so one goal can
     * cost autonomous_max_steps * max_tool_rounds model calls. On a slow machine a step takes about a minute.
     */
    static constexpr NumberDefault<int>      kMaxToolRounds      = {5,       1,    50};
    static constexpr NumberDefault<unsigned> kCodeTimeoutMs      = {10000,   1000, 3600000};
    static constexpr NumberDefault<int>      kAutonomousMaxSteps = {10,      1,    500};
    // What one knowledge base search may put in the context.
    static constexpr NumberDefault<size_t>   kKbMaxPassages      = {6,       1,    100};
    static constexpr NumberDefault<size_t>   kKbMaxResultChars   = {4000,    500,  200000};

    // A message with one of these words asks for the internet, which is then searched before the knowledge base.
    static constexpr const char* kInternetWords[]  = {"internet", "online", "wikipedia", "google"};
    // NOTE: "web" alone is too common ("web app"): it counts only after one of these ("the web", "na web").
    static constexpr const char* kWordsBeforeWeb[] = {"the", "on", "na", "da", "pela"};
    /*
     * search_internet (scripts\websearch.py).
     *
     * web_sources:          tried in order until one has results. None needs a key.
     * web_pages_to_read:    how many of the top results' pages it reads.
     * web_max_result_chars: the most text it returns.
     *
     * WARNING: the script must end before code_timeout_ms, so it reads the pages in whatever time the search
     * left. A higher web_pages_to_read only helps if code_timeout_ms leaves time to read them.
     *
     * WARNING: tool output is capped at 16 KB, which is why web_max_result_chars stops at 12000.
     */
    static constexpr const char*             kWebSources[]       = {"duckduckgo", "bing", "wikipedia"};
    static constexpr NumberDefault<int>      kWebPagesToRead     = {3,       0,    10};
    static constexpr NumberDefault<size_t>   kWebMaxResultChars  = {6000,    1000, 12000};

    /*
     * Folders and files, relative to the program's folder. A settings file names them relative to the config
     * folder, hence the ..\ in front there.
     *
     * scripts:   the search scripts.
     * knowledge: the documents reader.py writes.
     * rag index: the passage index the knowledge base search keeps of them.
     * workspace: where write_file puts a file named without a full path, and the folder execute_code runs in,
     *            so the files a script makes with a relative path land there too, not next to the program.
     * life:      each agent's life (bio/EndocrineSystem.hpp), one file per agent; see LifeFile.
     *
     * NOTE: writing a file anywhere outside the workspace asks the user first.
     */
    static constexpr const char* kScriptsDir   = "scripts";
    static constexpr const char* kKnowledgeDir = "data\\knowledge";
    static constexpr const char* kRagIndex     = "data\\rag\\knowledge.idx";
    static constexpr const char* kWorkspaceDir = "workspace";
    static constexpr const char* kLifeDir      = "data\\life";

    /*
     * File tasks: a message with a word from each of these two lists asks to create, save or run a file
     * ("create a python script in C:\tools", "crie um arquivo na pasta"). The search tools are left out for it
     * (in a task, for the whole run). An empty list turns this off.
     *
     * WARNING: a small model cannot control these operations by itself. Given the search tools, it searches
     * the knowledge base, then the internet, before doing anything; without them it goes straight to
     * write_file and execute_code.
     */
    static constexpr const char* kFileTaskActions[] = {
        // Create / Write / Edit
        "create", "write", "save", "make", "generate", "edit", "modify", "append", "mkdir",
        "crie", "criar", "cria", "escreva", "escrever", "salve", "salvar", "gere", "gerar", "faça", "fazer",
        "edite", "editar", "modifique", "modificar", "adicione", "adicionar",
        // Delete / Remove / Clean
        "delete", "remove", "erase", "del", "rm", "rmdir", "clean", "clear", "wipe",
        "delete", "deletar", "apague", "apagar", "apaga", "exclua", "excluir", "exclui", "remova", "remover", "limpe", "limpar",
        // Move / Rename / Copy / Read / List
        "move", "rename", "copy", "list", "read", "show", "open", "run", "execute",
        "mova", "mover", "renomeie", "renomear", "copie", "copiar", "liste", "listar", "leia", "ler", "mostre", "mostrar", "abra", "abrir", "rode", "rodar", "executar"};

    static constexpr const char* kFileTaskObjects[] = {
        // Files & Folders
        "file", "files", "folder", "folders", "directory", "directories", "dir", "path", "workspace",
        "arquivo", "arquivos", "pasta", "pastas", "diretório", "diretorio", "diretórios", "diretorios", "caminho",
        // Code & Scripts
        "script", "scripts", "program", "programa", "code", "código", "codigo",
        // Common Extensions
        "py", "ps1", "bat", "cmd", "sh", "txt", "md", "csv", "json", "xml", "yaml", "yml", "log", "ini", "cfg", "cpp", "h", "hpp", "js", "ts", "html", "css"};
    /*
     * A message with one of these words asks to delete a file. In such a message (in a task, for the whole run)
     * write_file refuses to empty a file that exists and tells the model to call delete_file instead.
     *
     * WARNING: asked to delete a file, a small model "deletes" it with write_file and no content, which only
     * empties it, then says it is gone. This list is what stops that.
     *
     * NOTE: "clean", "clear" and "limpe" are left out on purpose: emptying a file is what they ask.
     */
    static constexpr const char* kFileDeleteActions[] = {
        "delete", "remove", "erase", "del", "rm", "rmdir", "wipe",
        "deletar", "apague", "apagar", "apaga", "exclua", "excluir", "exclui", "remova", "remover"};
    /*
     * ...unless the message also has one of these words, or asks for the internet (internet_words): then it
     * needs a search too, and the search tools stay.
     *
     * WARNING: do not add "documents": it is in paths (C:\Users\me\Documents), so every file task there would
     * keep the search tools.
     */
    static constexpr const char* kSearchWords[] = {
        "search", "knowledge", "pesquise", "pesquisar", "pesquisa", "busque", "buscar", "procure", "procurar",
        "conhecimento"};

    // The prompt files, relative to the config folder; config\defaults\ has their factory copies.
    static constexpr const char* kSystemPromptFile     = "system_prompt.txt";
    static constexpr const char* kAutonomousPromptFile = "autonomous_prompt.txt";
    /*
     * Used only when config\system_prompt.txt and its copy in config\defaults\ are both missing.
     *
     * NOTE: the real directives are in those files. Edit them there: no rebuild needed.
     */
    static constexpr const char* kFallbackSystemPrompt =
        "You are a helpful assistant running on the user's Windows machine.\n"
        "Use a tool only when the message needs it: write_file to create or save a file, delete_file to delete\n"
        "one, execute_code to run something on this machine, search_knowledge_base and search_internet for\n"
        "questions about facts.\n"
        "Never invent quotes, sources, numbers or events.\n";
    // Added to the system prompt in autonomous mode. As above: the real one is config\autonomous_prompt.txt.
    static constexpr const char* kFallbackAutonomousPrompt =
        "Autonomous mode:\n"
        "You are working alone on a goal the user gave you, and nobody answers questions. In each step, take\n"
        "the next action with the tool the goal needs. When the goal is done, or cannot be done, call finish\n"
        "with the result.\n";

    // What the model reads about each tool.
    static constexpr const char* kToolWriteFile =
        "Creates or replaces a text file with the content given, creating its folders, and returns its full path. "
        "Use it to create or save a script, a program, a text or any other file. It does not run anything. A path "
        "without a folder puts the file in the workspace folder. Writing outside the workspace folder, or a script "
        "with code that could do harm, happens only if the user allows it.";
    static constexpr const char* kToolDeleteFile =
        "Deletes one file, or an empty folder. Use it when the user asks to delete, remove or erase a file. "
        "write_file cannot delete a file. A path without a folder is a file in the workspace folder. It happens "
        "only if the user allows it.";
    static constexpr const char* kToolExecuteCode =
        "Runs code or a command on the local Windows machine now and returns its exit code, stdout and stderr. "
        "It does not save the code: to create a file, use write_file. To run a saved file, give the command that "
        "runs it, such as: python hello.py. Languages: python, powershell, cmd. It runs in the workspace folder. Limits: 10 s, 512 MB, no stdin, output capped at 16 KB per stream. Code that deletes or "
        "writes files, installs software, changes settings or uses the network runs only if the user allows it.";
    static constexpr const char* kToolSearchKnowledgeBase =
        "Searches all the documents the user saved and returns the few passages that best match, "
        "each with its document name and page. Use it for questions about facts, people, events or the user's "
        "documents, before search_internet unless the user asked for the internet. Not for creating or running "
        "files, and not for greetings or small talk.";
    static constexpr const char* kToolSearchInternet =
        "Searches the web and returns the best results: title, address, date when known, and the text of the top "
        "pages that is about the query. Pages can be old: check the date before calling a value current. Call it "
        "directly when the user asks to search the internet, the web, online or Wikipedia; otherwise only after "
        "search_knowledge_base found nothing on the subject. Not for creating or running files.";
    // Autonomous mode only.
    static constexpr const char* kToolFinish =
        "Ends the task. Call it once, when the goal is done or cannot be done. The summary is all the user "
        "reads: put the result itself in it and what was done.";

    /*
     * ---- The settings ----
     * Each starts with its built-in default above.
     */
    float       agent_version        = kAgentVersion;
    std::string agent_name           = kAgentName;
    std::string endpoint             = kEndpoint;
    std::string model                = kModel;
    int         num_ctx              = kNumCtx.value;
    int         num_predict          = kNumPredict.value;
    std::string embed_model          = kEmbedModel;
    int         embed_num_ctx        = kEmbedNumCtx.value;
    int         max_tool_rounds      = kMaxToolRounds.value;
    unsigned    code_timeout_ms      = kCodeTimeoutMs.value;
    int         autonomous_max_steps = kAutonomousMaxSteps.value;

    std::string scripts_dir         = Folder(fileSystem.RelativePathToOSPath(kScriptsDir));
    std::string knowledge_dir       = Folder(fileSystem.RelativePathToOSPath(kKnowledgeDir));
    std::string rag_index           = fileSystem.RelativePathToOSPath(kRagIndex).u8string();
    size_t      kb_max_passages     = kKbMaxPassages.value;
    size_t      kb_max_result_chars = kKbMaxResultChars.value;

    std::vector<std::string> internet_words       = Words(kInternetWords);
    std::vector<std::string> words_before_web     = Words(kWordsBeforeWeb);
    std::vector<std::string> web_sources          = Words(kWebSources);
    int                      web_pages_to_read    = kWebPagesToRead.value;
    size_t                   web_max_result_chars = kWebMaxResultChars.value;

    std::string              workspace_dir       = Folder(fileSystem.RelativePathToOSPath(kWorkspaceDir));
    std::vector<std::string> file_task_actions   = Words(kFileTaskActions);
    std::vector<std::string> file_task_objects   = Words(kFileTaskObjects);
    std::vector<std::string> file_delete_actions = Words(kFileDeleteActions);
    std::vector<std::string> search_words        = Words(kSearchWords);

    std::string system_prompt              = kFallbackSystemPrompt;
    std::string autonomous_prompt          = kFallbackAutonomousPrompt;
    std::string tool_write_file            = kToolWriteFile;
    std::string tool_delete_file           = kToolDeleteFile;
    std::string tool_execute_code          = kToolExecuteCode;
    std::string tool_search_knowledge_base = kToolSearchKnowledgeBase;
    std::string tool_search_internet       = kToolSearchInternet;
    std::string tool_finish                = kToolFinish;

    // Which files Load() read, and anything in them it could not use. Shown in the activity log at startup.
    std::vector<std::string> notes;
    // This computer's name and its own settings file (MachineSettingsPath). Set by Load().
    std::string machine_name;
    std::string machine_settings;
    /*
     * An agent made in the window: its file, and its instructions (the text after the file's --- line).
     * Empty for the default agent.
     */
    std::string agent_file;
    std::string instructions;

    /*
     * The settings of the default agent or, with `agent_file`, of an agent made in the window.
     *
     * NOTE: the agent's file is read last, after settings.txt and this computer's file, so any key it has (the
     * context and reply sizes too) overrides theirs for that agent; a key it leaves out is the default agent's.
     */
    static ChatAgentConfig Load(const std::filesystem::path& agent_file = {}) {
        namespace fs = std::filesystem;
        ChatAgentConfig config;

        const fs::path dir      = fileSystem.RelativePathToOSPath("config");
        config.machine_name     = MachineName();
        config.machine_settings = MachineSettingsPath().u8string();
        config.agent_file       = agent_file.u8string();
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            config.notes.push_back("No config folder at " + dir.u8string() + ": using the built-in settings and "
                                   "system prompt. restore_config.exe writes them there.");
            return config;
        }

        std::string     summary;
        PromptFiles     prompt_files;
        const fs::path  settings_path = dir / "settings.txt";
        if (config.ReadSettings(settings_path, dir, prompt_files)) {
            summary = "Settings from " + settings_path.u8string();
        } else {
            summary = "No " + settings_path.u8string() + ": built-in settings";
        }
        // This computer's values take the place of settings.txt's...
        if (config.ReadSettings(fs::u8path(config.machine_settings), dir, prompt_files)) {
            summary += "; this computer's (" + config.machine_name + ") from " + config.machine_settings;
        } else {
            summary += "; none saved for this computer (" + config.machine_name + ")";
        }
        // ...and an agent's own take the place of both.
        if (!agent_file.empty()) {
            if (config.ReadSettings(agent_file, dir, prompt_files, &config.instructions)) {
                summary += "; agent from " + config.agent_file;
            } else {
                config.notes.push_back("Cannot read the agent file " + config.agent_file + ": the default "
                                       "settings are used.");
            }
        }

        if (config.num_predict >= config.num_ctx) {
            config.notes.push_back("Settings: num_predict must be smaller than num_ctx; using num_ctx / 4.");
            config.num_predict = config.num_ctx / 4;
        }

        std::string text;
        fs::path    from;
        if (ReadPromptFile(dir, prompt_files.system, text, from)) {
            config.system_prompt = text;
            summary += "; system prompt from " + from.u8string();
        } else {
            summary += "; no system prompt at " + Resolve(dir, prompt_files.system).u8string() + " nor in " +
                       DefaultsFolder().u8string() + ": the short built-in one (restore_config.exe writes the files)";
        }
        // Reported only when it is not the usual file: a chat does not use it.
        if (ReadPromptFile(dir, prompt_files.autonomous, text, from)) {
            config.autonomous_prompt = text;
            if (from != Resolve(dir, prompt_files.autonomous)) {
                config.notes.push_back("Autonomous prompt from " + from.u8string());
            }
        } else {
            config.notes.push_back("No autonomous prompt at " + Resolve(dir, prompt_files.autonomous).u8string() +
                                   " nor in " + DefaultsFolder().u8string() + ": agent.exe --auto and Run task use "
                                   "the short built-in one.");
        }
        // An agent made in the window learns its name and its instructions, after the general rules.
        if (!config.agent_file.empty()) {
            config.system_prompt += "\nAbout you:\nYour name is " + config.agent_name + ".\n";
            const std::string own = Trim(config.instructions);
            if (!own.empty()) {
                config.system_prompt += own + "\n";
            }
        }
        config.notes.insert(config.notes.begin(), summary);
        return config;
    }

    /*
     * Applies one setting as a line of a settings file would, e.g. Apply("num_ctx", "8192"). Returns what is
     * wrong with it, empty when it was applied. The Settings window checks its values with it before saving.
     */
    std::string Apply(const std::string& key, const std::string& value) {
        PromptFiles prompt_files;  // a prompt file named here would take effect only through Load()
        return Set(key, value, fileSystem.RelativePathToOSPath("config"), prompt_files);
    }

    using Values = std::vector<std::pair<std::string, std::string>>;  // (key, value), as a settings file has them

    /*
     * Writes `values` into this computer's settings file, so that from the next Load() on they take the place of
     * settings.txt's here, for the default agent and every agent that has no value of its own. Returns what
     * went wrong, empty when saved.
     */
    std::string SaveMachineSettings(const Values& values) const {
        if (machine_settings.empty()) {
            return "the settings were not loaded from the config folder, so there is no file for this computer";
        }
        const std::vector<std::string> header = {
            "# Settings for this computer (" + machine_name + "), written by the Settings window of agent_gui.exe.",
            "# A key here takes the place of the same key in ..\\settings.txt; a key left out comes from there, or",
            "# from the defaults built into the program. Same format as settings.txt, and relative paths are",
            "# relative to the config folder too. An agent's own file (..\\agents\\) takes the place of both for",
            "# that agent. It can be edited by hand.",
            "",
        };
        return WriteSettings(std::filesystem::u8path(machine_settings), values, {}, header, nullptr);
    }

    /*
     * Writes the settings of an agent made in the window into its file: `values` are set, the keys in `follow`
     * taken out, so the agent uses the default agent's value for them from the next Load() on, and the text
     * after the --- line becomes `new_instructions`. Returns what went wrong, empty when saved.
     */
    std::string SaveAgentSettings(const Values& values, const std::vector<std::string>& follow,
                                  const std::string& new_instructions) const {
        if (agent_file.empty()) {
            return "the default agent has no file of its own";
        }
        return WriteSettings(std::filesystem::u8path(agent_file), values, follow, {}, &new_instructions);
    }

    // This computer's name, as %COMPUTERNAME% gives it; "local" if Windows does not give it.
    static std::string MachineName() {
        const std::string name = Sys_MachineName();
        return name.empty() ? "local" : name;
    }

    // This computer's own settings file: config\machines\<computer name>.txt.
    static std::filesystem::path MachineSettingsPath() {
        return fileSystem.RelativePathToOSPath("config\\machines\\" + MachineName() + ".txt");
    }

    /*
     * Where this agent's life is kept (bio/EndocrineSystem.hpp): its hormone levels, the time it spent in each
     * life state and what happened to it. data\life\<agent name>.txt, so each agent lives a life of its own.
     *
     * NOTE: by name, as the agents are told apart in the window: a renamed agent starts a new life.
     */
    std::filesystem::path LifeFile() const {
        std::string stem = SafeFileName(agent_name);
        if (stem.empty()) {
            stem = "agent";
        }
        return fileSystem.RelativePathToOSPath(kLifeDir) / std::filesystem::u8path(stem + ".txt");
    }

    /*
     * Writes `text` with Windows line ends, creating the folders on the way. False if it cannot. For the files
     * Load() reads: the Settings window writes this computer's, restore_config.exe the defaults.
     */
    static bool WriteTextFile(const std::filesystem::path& path, const std::string& text) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        for (const char c : text) {
            if (c == '\n') {
                out << '\r';
            }
            out << c;
        }
        out.close();
        return static_cast<bool>(out);
    }

    /*
     * settings.txt with every key at its built-in default, and the comments that explain them: what
     * restore_config.exe writes.
     *
     * WARNING: a key added to Set() must be added here too, or a restored settings.txt leaves it out and the
     * user never sees it.
     */
    static std::string DefaultSettingsText() {
        const ChatAgentConfig d;
        std::ostringstream    out;
        out << "# Agent settings, read when agent.exe / agent_gui.exe starts: after a change, restart the program.\n"
               "# No rebuild needed. run.ps1 reads this file too, so the models it preloads match.\n"
               "#\n"
               "# Format: one \"key = value\" per line. Lines starting with # are comments (only whole lines, not after a value).\n"
               "#\n"
               "# which the Settings window of agent_gui.exe writes. restore_config.exe writes the defaults back.\n"
               "# Some Harness are inserted on compile time, so it has boundaries.\n"
               "# The main its to search mostlly into the \\data.\n"
               "# WARNING\n"
               "# Be carefull on running into the agents mode it can be really hard depending on the machine that you are running.\n"
               "# The Name was created as the same of my daughter.\n"
               "agent_name = " << d.agent_name << "\n"
               "# --- Models (all local, in Ollama) ---\n"
               "# The chat model needs Ollama's 'tools' capability (check with: ollama show <model>).\n"
               "# Be carefull about the models some of them are too short for this model.\n"
               "model = " << d.model << "\n"
               "endpoint = " << d.endpoint << "\n"
               "# Embedding model for search by meaning; leave empty for keyword search only.\n"
               "embed_model = " << d.embed_model << "\n"
               "embed_num_ctx = " << d.embed_num_ctx << "\n"
               "\n"
               "# --- Size of the conversation and of the replies, in tokens (about 3/4 of a word each) ---\n"
               "# num_ctx: the context window, shared by the system prompt, tools, conversation, search results and reply.\n"
               "# Bigger keeps more of the conversation, but uses more memory and takes longer to read on a slow machine.\n"
               "num_ctx = " << d.num_ctx << "\n"
               "# num_predict: the longest reply. Kept free: older search results are shortened to leave this much room.\n"
               "num_predict = " << d.num_predict << "\n"
               "\n"
               "# --- Tools ---\n"
               "# Rounds of tool calls per question before the model must answer with what it has.\n"
               "max_tool_rounds = " << d.max_tool_rounds << "\n"
               "# Longest run of execute_code and of the search scripts, in milliseconds.\n"
               "code_timeout_ms = " << d.code_timeout_ms << "\n"
               "\n"
               "# --- Autonomous mode (agent.exe --auto \"goal\", or Run task in the window) ---\n"
               "# Most steps one goal may take. Each step is up to max_tool_rounds rounds of tool calls and a note; on a\n"
               "# slow machine a step takes about a minute.\n"
               "autonomous_max_steps = " << d.autonomous_max_steps << "\n"
               "# Instructions added to the system prompt in this mode.\n"
               "autonomous_prompt_file = " << kAutonomousPromptFile << "\n"
               "\n"
               "# --- Folders and files ---\n"
               "scripts_dir = ..\\" << kScriptsDir << "\n"
               "knowledge_dir = ..\\" << kKnowledgeDir << "\n"
               "rag_index = ..\\" << kRagIndex << "\n"
               "# What one knowledge base search may put in the context.\n"
               "kb_max_passages = " << d.kb_max_passages << "\n"
               "kb_max_result_chars = " << d.kb_max_result_chars << "\n"
               "\n"
               "# --- Files and code ---\n"
               "# Where write_file puts a file given without a full path, and the folder execute_code runs in, so the\n"
               "# files a script makes land there too. Writing a file anywhere else asks you first.\n"
               "workspace_dir = ..\\" << kWorkspaceDir << "\n"
               "# A message with a word from each of these two lists asks to create, save or run a file (\"create a python\n"
               "# script in C:\\tools\", \"crie um arquivo\"): the searches are left out for it (in a task, for the whole run),\n"
               "# so the model goes straight to write_file and execute_code instead of searching first. Leave a list empty\n"
               "# to turn this off.\n"
               "file_task_actions = " << JoinWords(d.file_task_actions) << "\n"
               "file_task_objects = " << JoinWords(d.file_task_objects) << "\n"
               "# ...unless the message also has one of these words, or asks for the internet: then the searches stay.\n"
               "search_words = " << JoinWords(d.search_words) << "\n"
               "# A message with one of these words asks to delete a file: write_file then does not empty a file that\n"
               "# exists (a small model \"deletes\" that way), it tells the model to call delete_file.\n"
               "file_delete_actions = " << JoinWords(d.file_delete_actions) << "\n"
               "\n"
               "# --- Text the model reads ---\n"
               "# The directives: which tool for which request and how to answer. Factory copies in defaults\\.\n"
               "system_prompt_file = " << kSystemPromptFile << "\n"
               "\n"
               "# A message containing one of these words asks for the internet, which is then searched before the knowledge base.\n"
               "internet_words = " << JoinWords(d.internet_words) << "\n"
               "# \"web\" counts only after one of these words (\"the web\", \"na web\"), so \"web app\" does not trigger it.\n"
               "words_before_web = " << JoinWords(d.words_before_web) << "\n"
               "\n"
               "# --- Internet search (search_internet: scripts\\websearch.py) ---\n"
               "# Sources tried in this order until one has results: duckduckgo, bing (the whole web), wikipedia. None\n"
               "# needs a key. A search engine can refuse automated searches at any time; the next one is then used.\n"
               "web_sources = " << JoinWords(d.web_sources) << "\n"
               "# Top results whose pages are read for the text about the query (0: only the search engine's snippets).\n"
               "# Reading takes what is left of code_timeout_ms after the search.\n"
               "web_pages_to_read = " << d.web_pages_to_read << "\n"
               "# Most characters one internet search may put in the context.\n"
               "web_max_result_chars = " << d.web_max_result_chars << "\n"
               "\n"
               "# Tool descriptions (one line each).\n"
               "tool_write_file = " << d.tool_write_file << "\n"
               "tool_delete_file = " << d.tool_delete_file << "\n"
               "tool_execute_code = " << d.tool_execute_code << "\n"
               "tool_search_knowledge_base = " << d.tool_search_knowledge_base << "\n"
               "tool_search_internet = " << d.tool_search_internet << "\n"
               "# Autonomous mode only.\n"
               "tool_finish = " << d.tool_finish << "\n";
        return out.str();
    }

    // The agents made in the window: config\agents\*.txt, in file name order.
    static std::vector<std::filesystem::path> AgentFiles() {
        namespace fs = std::filesystem;
        std::vector<fs::path> files;
        std::error_code ec;
        for (fs::directory_iterator it(AgentsFolder(), ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".txt") {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    static std::filesystem::path AgentsFolder() { return fileSystem.RelativePathToOSPath("config\\agents"); }

    /*
     * Saves a new agent made in the window: config\agents\<name>.txt with its name and model, and its
     * instructions after the --- line. Returns the file, or an empty path with the reason in `error`.
     */
    static std::filesystem::path CreateAgentFile(const std::string& name, const std::string& model,
                                                 const std::string& instructions, std::string& error) {
        namespace fs = std::filesystem;
        const std::string stem = SafeFileName(name);
        if (stem.empty()) {
            error = "\"" + name + "\" cannot be used as a file name; choose another name";
            return {};
        }
        const fs::path path = AgentsFolder() / fs::u8path(stem + ".txt");
        std::error_code ec;
        if (fs::exists(path, ec)) {
            error = "there is already an agent file " + path.u8string();
            return {};
        }
        const std::string text =
            "# Agent \"" + name + "\", made in the window of agent_gui.exe. Same format as ..\\settings.txt. It is\n"
            "# read after settings.txt and this computer's own file (..\\machines\\), so a key here takes the place\n"
            "# of theirs for this agent; a key left out is the default agent's. Its Settings window writes the keys\n"
            "# it changes. The text after the --- line is this agent's instructions, added to its system prompt.\n"
            "# Deleting the agent in the window removes this file, and a copy goes to ..\\backup\\.\n"
            "\n"
            "agent_name = " + name + "\n"
            "model = " + model + "\n"
            "\n"
            "---\n" +
            Trim(instructions) + "\n";
        if (!WriteTextFile(path, text)) {
            error = "could not write " + path.u8string();
            return {};
        }
        return path;
    }

    /*
     * Removes an agent's file, after copying it to config\backup\<date and time>\agents\. Returns what went
     * wrong, empty when it is gone.
     */
    static std::string DeleteAgentFile(const std::filesystem::path& path) { return MoveToBackup(path, "agents"); }

    /*
     * Removes a file after copying it to config\backup\<date and time>\<folder>\. Returns what went wrong, empty
     * when it is gone, or was never there.
     */
    static std::string MoveToBackup(const std::filesystem::path& path, const char* folder) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return {};  // removed by hand already
        }
        const fs::path copy = BackupFolder() / folder / path.filename();
        fs::create_directories(copy.parent_path(), ec);
        if (ec || !fs::copy_file(path, copy, fs::copy_options::overwrite_existing, ec)) {
            return "could not copy it to " + copy.u8string() + (ec ? ": " + ec.message() : std::string());
        }
        if (!fs::remove(path, ec)) {
            return "could not remove " + path.u8string() + (ec ? ": " + ec.message() : std::string());
        }
        return {};
    }

    /*
     * The factory copies of the prompt files: config\defaults\system_prompt.txt and autonomous_prompt.txt. Read
     * when config\ has no prompt file, and what restore_config.exe writes back.
     *
     * NOTE: the program never writes there, so the factory copies survive a broken config.
     */
    static std::filesystem::path DefaultsFolder() { return fileSystem.RelativePathToOSPath("config\\defaults"); }

    // The factory copy of a prompt file, config\defaults\<name>. False when it is missing or empty.
    static bool ReadDefaultPrompt(const std::string& name, std::string& text) {
        return ReadTextFile(DefaultsFolder() / std::filesystem::u8path(name), text) && !Trim(text).empty();
    }

    // Where copies of files about to be replaced or removed go: config\backup\<date and time>, e.g. 2026-11-11_153012.
    static std::filesystem::path BackupFolder() {
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M%S", std::localtime(&now));
        return fileSystem.RelativePathToOSPath(std::string("config\\backup\\") + stamp);
    }

private:
    // The prompt files settings.txt names, relative to the config folder.
    struct PromptFiles {
        std::string system      = kSystemPromptFile;
        std::string autonomous  = kAutonomousPromptFile;
    };

    /*
     * Applies every line of one settings file: settings.txt, an agent's or this computer's. False when it
     * cannot be read. With `rest` (an agent's file), a --- line ends the settings and the text after it goes to
     * `rest`.
     */
    bool ReadSettings(const std::filesystem::path& path, const std::filesystem::path& dir, PromptFiles& prompt_files,
                      std::string* rest = nullptr) {
        std::string text;
        if (!ReadTextFile(path, text)) {
            return false;
        }
        const std::string file = path.filename().u8string();
        size_t pos = 0;
        for (int number = 1; pos < text.size(); ++number) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string line = Trim(text.substr(pos, end - pos));
            pos = end + 1;
            if (rest && line == "---") {
                *rest = pos < text.size() ? text.substr(pos) : std::string();
                break;
            }
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const size_t eq = line.find('=');
            std::string error;
            if (eq == std::string::npos) {
                error = "no '=' in \"" + line + "\"";
            } else {
                error = Set(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)), dir, prompt_files);
            }
            if (!error.empty()) {
                notes.push_back(file + " line " + std::to_string(number) + ": " + error + " (ignored)");
            }
        }
        return true;
    }

    /*
     * Sets `values` in the settings file at `path` and takes out the keys in `remove`. A key the file has gets
     * its line replaced; a new one goes after the last key, so it stays with the others. Comments and keys
     * written by hand are kept. With `rest` (an agent's file), the settings end at the --- line and the text
     * after it is replaced by `*rest`. `header`: the first lines of a file that does not exist yet. Returns what
     * went wrong, empty when written.
     */
    static std::string WriteSettings(const std::filesystem::path& path, const Values& values,
                                     const std::vector<std::string>& remove, const std::vector<std::string>& header,
                                     const std::string* rest) {
        std::vector<std::string> lines;  // the settings part of the file
        std::string text;
        if (ReadTextFile(path, text)) {
            std::istringstream in(text);
            for (std::string line; std::getline(in, line);) {
                if (rest && Trim(line) == "---") {
                    break;
                }
                lines.push_back(line);
            }
        } else if (rest) {
            return "cannot read " + path.u8string();  // an agent's file is made with the agent
        } else {
            lines = header;
        }

        std::vector<std::string> out;
        std::vector<bool>        written(values.size(), false);
        size_t                   after_keys = 0;  // in `out`, just after the last key
        for (const std::string& line : lines) {
            const std::string trimmed = Trim(line);
            const size_t      eq      = trimmed.find('=');
            if (trimmed.empty() || trimmed[0] == '#' || eq == std::string::npos) {
                out.push_back(line);
                continue;
            }
            const std::string key = Trim(trimmed.substr(0, eq));
            if (std::find(remove.begin(), remove.end(), key) != remove.end()) {
                continue;
            }
            std::string kept = line;
            for (size_t i = 0; i < values.size(); ++i) {
                if (values[i].first == key) {
                    kept       = key + " = " + values[i].second;
                    written[i] = true;
                }
            }
            out.push_back(kept);
            after_keys = out.size();
        }
        std::vector<std::string> added;
        for (size_t i = 0; i < values.size(); ++i) {
            if (!written[i]) {
                added.push_back(values[i].first + " = " + values[i].second);
            }
        }
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(after_keys ? after_keys : out.size()), added.begin(),
                   added.end());

        std::string file;
        for (const std::string& line : out) {
            file += line + "\n";
        }
        if (rest) {
            file += "---\n" + Trim(*rest) + "\n";
        }
        if (!WriteTextFile(path, file)) {
            return "could not write " + path.u8string();
        }
        return {};
    }

    // Applies one line of settings.txt. Returns what is wrong with it, empty when it was applied.
    std::string Set(const std::string& key, const std::string& value, const std::filesystem::path& dir, PromptFiles& prompt_files) {
        if (key == "agent_name") {
            agent_name = value;
        } else if (key == "endpoint") {
            endpoint = value;
        } else if (key == "model") {
            model = value;
        } else if (key == "embed_model") {
            embed_model = value;
        } else if (key == "num_ctx") {
            return SetNumber(value, kNumCtx, num_ctx);
        } else if (key == "num_predict") {
            return SetNumber(value, kNumPredict, num_predict);
        } else if (key == "embed_num_ctx") {
            return SetNumber(value, kEmbedNumCtx, embed_num_ctx);
        } else if (key == "max_tool_rounds") {
            return SetNumber(value, kMaxToolRounds, max_tool_rounds);
        } else if (key == "code_timeout_ms") {
            return SetNumber(value, kCodeTimeoutMs, code_timeout_ms);
        } else if (key == "autonomous_max_steps") {
            return SetNumber(value, kAutonomousMaxSteps, autonomous_max_steps);
        } else if (key == "kb_max_passages") {
            return SetNumber(value, kKbMaxPassages, kb_max_passages);
        } else if (key == "kb_max_result_chars") {
            return SetNumber(value, kKbMaxResultChars, kb_max_result_chars);
        } else if (key == "scripts_dir") {
            scripts_dir = Folder(Resolve(dir, value));
        } else if (key == "knowledge_dir") {
            knowledge_dir = Folder(Resolve(dir, value));
        } else if (key == "rag_index") {
            rag_index = Resolve(dir, value).u8string();
        } else if (key == "workspace_dir") {
            workspace_dir = Folder(Resolve(dir, value));
        } else if (key == "file_task_actions") {
            file_task_actions = WordList(value);
        } else if (key == "file_task_objects") {
            file_task_objects = WordList(value);
        } else if (key == "search_words") {
            search_words = WordList(value);
        } else if (key == "file_delete_actions") {
            file_delete_actions = WordList(value);
        } else if (key == "system_prompt_file") {
            prompt_files.system = value;
        } else if (key == "autonomous_prompt_file") {
            prompt_files.autonomous = value;
        } else if (key == "internet_words") {
            internet_words = WordList(value);
        } else if (key == "words_before_web") {
            words_before_web = WordList(value);
        } else if (key == "web_sources") {
            web_sources = WordList(value);
        } else if (key == "web_pages_to_read") {
            return SetNumber(value, kWebPagesToRead, web_pages_to_read);
        } else if (key == "web_max_result_chars") {
            return SetNumber(value, kWebMaxResultChars, web_max_result_chars);
        } else if (key == "tool_write_file") {
            tool_write_file = value;
        } else if (key == "tool_delete_file") {
            tool_delete_file = value;
        } else if (key == "tool_execute_code") {
            tool_execute_code = value;
        } else if (key == "tool_search_knowledge_base") {
            tool_search_knowledge_base = value;
        } else if (key == "tool_search_internet") {
            tool_search_internet = value;
        } else if (key == "tool_finish") {
            tool_finish = value;
        } else {
            return "unknown setting '" + key + "'";
        }
        return {};
    }

    // `range`: the setting's built-in default, whose min and max a value must be within.
    template <class Number>
    static std::string SetNumber(const std::string& value, const NumberDefault<Number>& range, Number& out) {
        errno = 0;
        char* end = nullptr;
        const long long n = std::strtoll(value.c_str(), &end, 10);
        if (value.empty() || *end != '\0' || errno == ERANGE) {
            return "'" + value + "' is not a whole number";
        }
        if (n < range.min || n > range.max) {
            return std::to_string(n) + " is out of range (" + std::to_string(range.min) + " to " +
                   std::to_string(range.max) + ")";
        }
        out = static_cast<Number>(n);
        return {};
    }

    static std::string Trim(const std::string& s) {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {};
        }
        return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    }

    // "a, B ,c" -> {"a", "b", "c"}
    static std::vector<std::string> WordList(const std::string& value) {
        std::vector<std::string> words;
        std::istringstream in(value);
        std::string word;
        while (std::getline(in, word, ',')) {
            word = Trim(word);
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!word.empty()) {
                words.push_back(word);
            }
        }
        return words;
    }

    // A list of built-in defaults, kInternetWords for one, as the vector its setting holds.
    template <size_t N>
    static std::vector<std::string> Words(const char* const (&words)[N]) {
        return std::vector<std::string>(words, words + N);
    }

    /*
     * `name` as a file name: characters Windows does not allow become '_', and the ends lose spaces and dots.
     * Empty when nothing is left, or for a name Windows keeps for a device (CON, NUL, COM1...).
     */
    static std::string SafeFileName(const std::string& name) {
        std::string s;
        for (const unsigned char c : name) {
            s += (c < 32 || std::strchr("<>:\"/\\|?*", c)) ? '_' : static_cast<char>(c);
        }
        const size_t first = s.find_first_not_of(" .");
        if (first == std::string::npos) {
            return {};
        }
        s = s.substr(first, s.find_last_not_of(" .") - first + 1);

        std::string upper = s;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const bool numbered = upper.size() == 4 && (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) &&
                              upper[3] >= '1' && upper[3] <= '9';
        if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" || numbered) {
            return {};
        }
        return s;
    }

    // {"a", "b"} -> "a, b", the form WordList() reads.
    static std::string JoinWords(const std::vector<std::string>& words) {
        std::string s;
        for (const std::string& word : words) {
            if (!s.empty()) {
                s += ", ";
            }
            s += word;
        }
        return s;
    }

    /*
     * A prompt file named in settings.txt (relative to the config folder) or, when it is missing or empty, its
     * factory copy of the same name in config\defaults\. `from`: the file read. False when neither has text.
     */
    static bool ReadPromptFile(const std::filesystem::path& dir, const std::string& name, std::string& text,
                               std::filesystem::path& from) {
        const std::filesystem::path paths[] = {Resolve(dir, name),
                                               DefaultsFolder() / std::filesystem::u8path(name).filename()};
        for (const std::filesystem::path& path : paths) {
            if (ReadTextFile(path, text) && !Trim(text).empty()) {
                from = path;
                return true;
            }
        }
        return false;
    }

    // A relative path is relative to the config folder.
    static std::filesystem::path Resolve(const std::filesystem::path& dir, const std::string& value) {
        std::filesystem::path p = std::filesystem::u8path(value);
        if (p.is_relative()) {
            p = dir / p;
        }
        return p.lexically_normal();
    }

    // WARNING: callers append file names straight to a folder, so a folder must end with a backslash.
    static std::string Folder(const std::filesystem::path& p) {
        std::string s = p.u8string();
        if (!s.empty() && s.back() != '\\' && s.back() != '/') {
            s += '\\';
        }
        return s;
    }

    /*
     * The whole file, with \r\n turned into \n. False if unreadable.
     *
     * NOTE: a UTF-8 BOM is skipped: Notepad adds one, and left in, it sticks to the first line, which then
     * reads as an unknown key.
     */
    static bool ReadTextFile(const std::filesystem::path& path, std::string& text) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return false;
        }
        std::ostringstream raw;
        raw << in.rdbuf();
        const std::string s = raw.str();
        text.clear();
        text.reserve(s.size());
        for (size_t i = (s.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0; i < s.size(); ++i) {
            if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') {
                continue;
            }
            text += s[i];
        }
        return true;
    }
};
