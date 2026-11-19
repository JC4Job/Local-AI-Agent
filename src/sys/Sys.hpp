#pragma once
// Platform layer: the Windows calls the rest of the program goes through.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <string>

// The running executable agent.exe.
inline std::filesystem::path Sys_EXEPath() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    const DWORD len  = GetModuleFileNameW(nullptr, buffer, size);
    if (len == 0 || len == size) {
        return {};  // WARNING: failed or cut short: a partial path would point somewhere else
    }
    return std::filesystem::path(std::wstring(buffer, len));
}

/*
 * The base path: the folder holding config\, scripts\ and data\. That is the executable's folder, or its
 * parent when the executable is in bin\; the current directory when the executable's path is unknown.
 */
inline std::filesystem::path Sys_DefaultBasePath() {
    namespace fs        = std::filesystem;
    const fs::path exe  = Sys_EXEPath();
    if (exe.empty()) {
        std::error_code ec;
        return fs::current_path(ec);
    }
    const fs::path exe_dir = exe.parent_path();
    for (const char* folder : {"config", "scripts", "data"}) {
        for (const fs::path& candidate : {exe_dir, exe_dir.parent_path()}) {
            std::error_code ec;
            if (fs::is_directory(candidate / folder, ec)) {
                return candidate;
            }
        }
    }
    return exe_dir;
}

// Windows' fonts folder (%WINDIR%\Fonts): Windows is not always on the same drive or in the same folder.
inline std::filesystem::path Sys_FontsFolder() {
    wchar_t buffer[MAX_PATH];
    const UINT len = GetWindowsDirectoryW(buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, len)) / L"Fonts";
}

// This computer's name, as %COMPUTERNAME% gives it, in UTF-8. Empty if Windows does not give it.
inline std::string Sys_MachineName() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD   len = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(buffer, &len)) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, len)).u8string();
}

/*
 * True when the console window was opened for this process alone, as when it is started from Explorer: the
 * window closes as soon as the process ends, before anyone can read it.
 */
inline bool Sys_OwnsConsole() {
    DWORD ids[2];
    return GetConsoleProcessList(ids, 2) == 1;
}
