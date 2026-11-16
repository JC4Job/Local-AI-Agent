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
    Reads the user's prompt from the command-line arguments, piped stdin or the console.
    Control flow of the CLI application is in cli_main.cpp, which uses this helper to get the input.
    captures multi-line input if the last character of a line is a continuation character (\ or `).
    captures the file exist s in the command-line arguments, piped stdin or the console.
    used for command line interface applications that require user input, such as chatbots or interactive scripts.
*/ 
class CLIHelper {
public:
    static bool StdinIsConsole() {
        return _isatty(_fileno(stdin)) != 0;
    }

    /*
        `first`: the first argument that is part of the input; 2 skips an option before it, as in
        `agent.exe --auto "goal"`. `label`: the console prompt.
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
            // PowerShell starts what it pipes with a UTF-8 BOM, even an empty string.
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
        The command-line arguments from `first` on, as UTF-8, joined by spaces. argv holds them in the ANSI code
        page, where "São" is not valid UTF-8 and breaks the JSON sent to the model, so they are read again as
        UTF-16. Empty if that fails.
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
        Like bash (\) and PowerShell (`):
            A line whose last character is the terminator continues on the next one. 
            It must be the very last character, so "C:\ " with a trailing space stays one line.
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
