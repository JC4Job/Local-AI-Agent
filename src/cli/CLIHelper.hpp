#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include "common/MacroWrap.h"

/*
 * CLIHelper - reads the user's prompt from the command-line arguments, piped stdin or the console, for command
 * line applications that need user input (chatbots, interactive scripts). The control flow is in cli_main.cpp,
 * which uses this to get the input.
 *
 * Piped input can be a whole file (agent.exe < question.txt). At the console, a line whose last character is a
 * continuation character (\ or `) continues on the next one, so the input can span several lines.
 */
class CLIHelper {
public:
    static bool StdinIsConsole() {
        return _isatty(_fileno(stdin)) != 0;
    }

    /*
     * `first`: the first argument that is part of the input; 2 skips an option before it, as in
     * `agent.exe --auto "goal"`. `label`: the console prompt.
     */
    static std::string CaptureCLIInput(int argc, char* argv[], int first = 1, const char* label = "User > ") {
        if (argc > first) {
            const std::string utf8 = Utf8Arguments(first);
            if (!utf8.empty()) {
                return utf8;
            }
            std::stringstream ss;
            for (int i = first; i < argc; ++i) {
                ss << argv[i] << (i < argc - 1 ? " " : "");
            }
            return ss.str();
        }
        if (!StdinIsConsole()) {
            std::stringstream ss;
            ss << std::cin.rdbuf();
            // WARNING: PowerShell starts what it pipes with a UTF-8 BOM, even an empty string: strip it.
            std::string piped = ss.str();
            if (piped.compare(0, 3, "\xEF\xBB\xBF") == 0) {
                piped.erase(0, 3);
            }
            return piped;
        }
        TextUtilities::print(label);
        std::string input, line;
        while (std::getline(std::cin, line)) {
            const bool more = StripContinuation(line);
            input += line;
            if (!more) {
                break;
            }
            input += '\n';
            TextUtilities::print(">> ");
        }
        return input;
    }

private:
    /*
     * The command-line arguments from `first` on, as UTF-8, joined by spaces. Empty if that fails.
     *
     * WARNING: argv holds them in the ANSI code page, where "São" is not valid UTF-8 and breaks the JSON sent
     * to the model, so they are read again as UTF-16.
     */
    static std::string Utf8Arguments(int first) {
        int count = 0;
        LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) {
            return {};
        }
        std::string out;
        for (int i = first; i < count; ++i) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, args[i], -1, nullptr, 0, nullptr, nullptr);
            if (len <= 1) {
                continue;
            }
            std::string arg(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, args[i], -1, &arg[0], len, nullptr, nullptr);
            if (!out.empty()) {
                out += ' ';
            }
            out += arg;
        }
        LocalFree(args);
        return out;
    }

    /*
     * Like bash (\) and PowerShell (`): a line whose last character is the continuation character continues
     * on the next one.
     *
     * NOTE: it must be the very last character, so "C:\ " with a trailing space stays one line.
     */
    static bool StripContinuation(std::string& line) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || (line.back() != '\\' && line.back() != '`')) {
            return false;
        }
        line.pop_back();
        return true;
    }
};
