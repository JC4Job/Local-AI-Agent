/*
    restore_config.exe: puts the agent's configuration back to the defaults built into the program, for when it
    is broken (a bad value, a deleted line, a prompt edited into nonsense).

    It writes config\settings.txt with the defaults in src/agent/AgentConfig.hpp, config\system_prompt.txt and
    config\autonomous_prompt.txt from their factory copies in config\defaults\ (the short prompts built into the
    program if a copy is missing), and removes this computer's own settings, config\machines\<computer name>.txt,
    so its values come from settings.txt again. Nothing is lost: every file it replaces or removes is copied first
    to config\backup\<date and time>\. Other computers' files in config\machines\ are left alone, and so are the
    agents made in the window (config\agents\), the knowledge base and its index.

    Usage: restore_config.exe          shows what it will do and asks first
           restore_config.exe --yes    does it without asking (for scripts)
    Exit code: 0 when the defaults were written, 1 when nothing was changed or it failed, 2 for a wrong option.
*/

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "agent/AgentConfig.hpp"
#include "cli/CLIHelper.hpp"
#include "common/Utils.hpp"

namespace {

namespace fs = std::filesystem;

// One file the restore writes, and its default text.
struct DefaultFile {
    const char* name;
    std::string text;
};

// Asks at the console. True on "yes" (or y, sim, s); anything else, an empty line or end of input is no.
bool AskToGoOn() {
    TextUtilities::print("Type yes to restore the defaults, anything else to cancel: ");
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        TextUtilities::print("\n");
        return false;
    }
    answer.erase(std::remove_if(answer.begin(), answer.end(), [](unsigned char c) { return std::isspace(c); }), answer.end());
    std::transform(answer.begin(), answer.end(), answer.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return answer == "yes" || answer == "y" || answer == "sim" || answer == "s";
}

int Restore(bool ask) {
    const fs::path dir     = fileSystem.RelativePathToOSPath("config");
    const fs::path machine = ChatAgentConfig::MachineSettingsPath();
    const fs::path backup  = ChatAgentConfig::BackupFolder();
    std::vector<DefaultFile> files = {{"settings.txt", ChatAgentConfig::DefaultSettingsText()}};
    std::string plan = "Restores the agent's configuration in " + dir.u8string() + " to the defaults:\n"
                       "  writes settings.txt\n";
    const DefaultFile prompts[] = {
        {"system_prompt.txt",     ChatAgentConfig::kFallbackSystemPrompt},
        {"autonomous_prompt.txt", ChatAgentConfig::kFallbackAutonomousPrompt},
    };
    for (const DefaultFile& prompt : prompts) {
        std::string text;
        if (ChatAgentConfig::ReadDefaultPrompt(prompt.name, text)) {
            plan += "  writes " + std::string(prompt.name) + " from defaults\\" + prompt.name + "\n";
            files.push_back({prompt.name, text});
        } else {
            plan += "  writes " + std::string(prompt.name) + " with the SHORT built-in prompt: defaults\\" +
                    prompt.name + " is missing (copy config\\defaults\\ from the package to get the full one)\n";
            files.push_back(prompt);
        }
    }
    std::error_code ec;
    const bool has_machine = fs::exists(machine, ec);
    if (has_machine) {
        plan += "  removes this computer's settings, " + machine.lexically_relative(dir).u8string() +
                ": its values come from settings.txt again\n";
    }
    plan += "Every file it replaces or removes is copied first to " + backup.u8string() + "\n\n";
    TextUtilities::print(plan);

    if (ask) {
        if (!CLIHelper::StdinIsConsole()) {
            TextUtilities::print("No console to answer at: run restore_config.exe --yes to restore without asking.\n"
                                 "Nothing was changed.\n");
            return 1;
        }
        if (!AskToGoOn()) {
            TextUtilities::print("Cancelled: nothing was changed.\n");
            return 1;
        }
    }

    // The backup comes first, all of it: if one copy fails, nothing is changed.
    std::vector<fs::path> to_back_up;
    for (const DefaultFile& file : files) {
        if (fs::exists(dir / file.name, ec)) {
            to_back_up.push_back(dir / file.name);
        }
    }
    if (has_machine) {
        to_back_up.push_back(machine);
    }
    for (const fs::path& from : to_back_up) {
        const fs::path to = backup / from.lexically_relative(dir);
        fs::create_directories(to.parent_path(), ec);
        if (ec || !fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
            std::cerr << "Could not back up " << from.u8string() << " to " << to.u8string() << ": "
                      << ec.message() << "\nNothing was changed.\n";
            return 1;
        }
        TextUtilities::print("  backed up " + from.lexically_relative(dir).u8string() + "\n");
    }

    for (const DefaultFile& file : files) {
        const fs::path path = dir / file.name;
        if (!ChatAgentConfig::WriteTextFile(path, file.text)) {
            std::cerr << "Could not write " << path.u8string() << ". The files as they were are in "
                      << backup.u8string() << "\n";
            return 1;
        }
        TextUtilities::print("  wrote the default " + std::string(file.name) + "\n");
    }
    if (has_machine) {
        if (!fs::remove(machine, ec)) {
            std::cerr << "Could not remove " << machine.u8string() << ": " << ec.message() << "\n";
            return 1;
        }
        TextUtilities::print("  removed " + machine.lexically_relative(dir).u8string() + "\n");
    }

    TextUtilities::print("\nDone. Restart agent.exe / agent_gui.exe to use the defaults.\n");
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    bool ask = true;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--yes" || option == "-y") {
            ask = false;
        } else {
            std::cerr << "Unknown option " << option << "\nUsage: restore_config.exe [--yes]\n";
            return 2;
        }
    }

    const int code = Restore(ask);
    // Started from Explorer, the window would close before the result could be read.
    if (Sys_OwnsConsole()) {
        TextUtilities::print("\nPress Enter to close.");
        std::string line;
        std::getline(std::cin, line);
    }
    return code;
}
