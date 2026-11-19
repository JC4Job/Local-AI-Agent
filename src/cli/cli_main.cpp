/*
 * Console front end, for scripts and quick tests: `agent.exe "question"`, piped stdin, or an interactive prompt.
 * Same ChatAgent as the window; with no activity handler, tool activity is printed to the console.
 *
 * `agent.exe --index` updates the knowledge base index and embeds the passages that have no vector yet.
 * Ctrl+C stops it after the current batch, and the next run continues from there.
 *
 * `agent.exe --auto "goal"` works on the goal alone, step after step, until it is done (AutonomousAgent.hpp).
 * Without a goal after --auto, it is read from piped stdin or the console. Ctrl+C stops the run. The exit code
 * is 0 when the model called finish, 1 otherwise.
 *
 * Code the agent wants to run that CLISecurityChecking flags as potentially harmful is shown with the reasons,
 * and runs only if the user types yes.
 *
 * WARNING: --index can take hours the first time.
 *
 * NOTE: with the input piped in there is no one to ask, so flagged code does not run.
 */

#include <algorithm>
#include <atomic>
#include <cctype>

#include "agent/AutonomousAgent.hpp"
#include "agent/ChatAgent.hpp"
#include "common/Utils.hpp"
#include "cli/CLIHelper.hpp"

namespace {
std::atomic<bool>               g_stop_requested{false};
// The run Ctrl+C stops; set only while the handler is installed for it.
std::atomic<AutonomousAgent*>   g_autonomous{nullptr};

// First Ctrl+C: stop the indexing or the autonomous run cleanly. Second: the default, which ends the process at once.
BOOL WINAPI OnCtrlC(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    if (g_stop_requested.exchange(true)) {
        return FALSE;
    }
    if (AutonomousAgent* agent = g_autonomous.load()) {
        agent->Cancel();
    }
    return TRUE;
}

/*
 * The console's ConfirmHandler: shows the flagged code (or the file the agent wants to write, and its content,
 * or to delete) and why, and goes ahead only on "yes" (or y, sim, s). Anything else, an empty line or Ctrl+C
 * refuses.
 *
 * NOTE: input piped in cannot answer: the code is not run, the file not written nor deleted.
 */
bool ConfirmAtConsole(const std::string& language, const std::string& code, const CLISecurityChecking::Report& report) {
    const std::string rule(72, '=');
    const bool        file    = !report.file.empty();
    const bool        deletes = file && report.deletes;
    std::string       head;
    if (file) {
        head = std::string(report.AnyDanger() ? "!! " : "") + "The agent wants to " +
               (deletes ? "delete" : "write") + " the file " + report.file +
               (report.AnyDanger() ? ", which could HARM this computer.\n" : ".\n");
    } else {
        head = report.AnyDanger() ? "!! The agent wants to run " + language + " code that could HARM this computer.\n"
                                  : "The agent wants to run " + language + " code that changes this computer.\n";
    }
    std::string text = "\n" + rule + "\n" + head + "\nWhy it was flagged:\n" + report.Describe();
    if (!deletes) {
        text += (file ? "\nThe file's content:\n" : "\nThe code:\n") + code +
                (code.empty() || code.back() == '\n' ? "" : "\n");
    }
    text += rule + "\n";
    if (!CLIHelper::StdinIsConsole()) {
        TextUtilities::print(text + "No console to answer at (the input is piped in): " +
                             (deletes ? "not deleted.\n" : file ? "not written.\n" : "not run.\n"));
        return false;
    }
    TextUtilities::print(text + (deletes ? "Delete it? Type yes to delete it, anything else to refuse: "
                                 : file  ? "Write it? Type yes to write it, anything else to refuse: "
                                         : "Run it? Type yes to run it, anything else to refuse: "));

    std::string answer;
    if (!std::getline(std::cin, answer)) {
        std::cin.clear();
        TextUtilities::print("\n");
        return false;
    }
    answer.erase(std::remove_if(answer.begin(), answer.end(), [](unsigned char c) { return std::isspace(c); }), answer.end());
    std::transform(answer.begin(), answer.end(), answer.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return answer == "yes" || answer == "y" || answer == "sim" || answer == "s";
}

int IndexKnowledgeBase(ChatAgent& agent) {
    SetConsoleCtrlHandler(OnCtrlC, TRUE);
    TextUtilities::print("Indexing the knowledge base. Ctrl+C stops after the current batch and keeps the progress.\n");
    try {
        const std::string summary =
            agent.IndexKnowledgeBase([](const std::string& s) { TextUtilities::print(s + "\n"); }, &g_stop_requested);
        TextUtilities::print(summary + "\n");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Indexing failed: " << e.what() << "\n";
        return 1;
    }
}

int RunAutonomous(int argc, char* argv[]) {
    AutonomousAgent agent(nullptr, ChatAgentConfig::Load());
    agent.SetConfirmHandler(ConfirmAtConsole);

    const std::string goal = CLIHelper::CaptureCLIInput(argc, argv, 2, "Goal > ");
    if (goal.find_first_not_of(" \t\r\n") == std::string::npos) {
        std::cerr << "No goal given. Usage: agent.exe --auto \"goal\"\n";
        return 2;
    }

    // Each step's reply is printed as the model writes it.
    agent.SetStreamHandler([](const std::string& piece, bool first) {
        if (first) {
            TextUtilities::print("\nAgent > ");
        }
        TextUtilities::print(piece);
    });

    g_autonomous = &agent;
    SetConsoleCtrlHandler(OnCtrlC, TRUE);
    TextUtilities::print("=== Autonomous run. Ctrl+C stops it. ===\nGoal: " + goal + "\n");
    const AutonomousAgent::Result result = agent.Run(goal);
    SetConsoleCtrlHandler(OnCtrlC, FALSE);
    g_autonomous = nullptr;

    TextUtilities::print("\n\n=== " + std::string(AutonomousAgent::Describe(result.outcome)) + " after " +
                         std::to_string(result.steps) + " step(s) ===\n" + result.summary + "\n");
    return result.outcome == AutonomousAgent::Outcome::Finished ? 0 : 1;
}
}  // namespace

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    const std::string option = argc >= 2 ? argv[1] : "";
    if (option == "--auto") {
        return RunAutonomous(argc, argv);
    }

    // Settings and system prompt from config\settings.txt and config\system_prompt.txt (see AgentConfig.hpp).
    ChatAgent agent(nullptr, ChatAgentConfig::Load());
    agent.SetConfirmHandler(ConfirmAtConsole);
    if (argc == 2 && option == "--index") {
        return IndexKnowledgeBase(agent);
    }

    // The reply is printed as the model writes it. `streamed` is the text of the current call to the model.
    std::string streamed;
    agent.SetStreamHandler([&streamed](const std::string& piece, bool first) {
        if (first) {
            TextUtilities::print("\nAgent > ");
            streamed.clear();
        }
        streamed += piece;
        TextUtilities::print(piece);
    });

    TextUtilities::print("=== Native C++ Ollama Harness Loaded ===\n");

    while (true) {
        std::string input = CLIHelper::CaptureCLIInput(argc, argv);
        if (input.empty() || input == "exit" || input == "quit" || input == "done" || input == "sair") {
            break;
        }

        streamed.clear();
        const std::string reply = agent.Ask(input);
        // A streamed reply is on screen already; an error, or a reply that was not streamed, is not.
        if (!reply.empty() && reply == streamed) {
            TextUtilities::print("\n\n");
        } else {
            TextUtilities::print("\nAgent > " + reply + "\n\n");
        }

        if (argc > 1 || !CLIHelper::StdinIsConsole()) {
            break;
        }
    }
    return 0;
}
