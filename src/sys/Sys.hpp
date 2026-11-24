#pragma once
// Platform layer: the Windows calls the rest of the program goes through.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>   // GetOpenFileNameW (comdlg32), left out by WIN32_LEAN_AND_MEAN
#include <shellapi.h>  // ShellExecuteW (shell32)

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
 * Puts the file `from` in the place of `to`, replacing it in one step, so a reader finds the old file or the new
 * one, never half of one. False if Windows refuses (no permission, `to` open without sharing).
 */
inline bool Sys_ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

/*
 * A file the user picks in Windows' Open dialog, over `owner`; empty when they cancel. `filter` is pairs of
 * label and pattern, each ending in \0, the whole ending in another \0: L"Text\0*.txt\0All files\0*.*\0".
 *
 * NOTE: OFN_NOCHANGEDIR: the dialog would otherwise move the current directory to wherever the user browsed.
 *
 * WARNING: modal: the window's frames stop until the dialog closes. Needs comdlg32 at link time.
 */
inline std::filesystem::path Sys_OpenFileDialog(HWND owner, const wchar_t* title, const wchar_t* filter) {
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
    ofn.lpstrTitle  = title;
    ofn.Flags       = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }
    return std::filesystem::path(file);
}

// Opens `folder` in Explorer. False when Windows cannot. Needs shell32 at link time.
inline bool Sys_OpenFolder(const std::filesystem::path& folder) {
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

/*
 * True when the console window was opened for this process alone, as when it is started from Explorer: the
 * window closes as soon as the process ends, before anyone can read it.
 */
inline bool Sys_OwnsConsole() {
    DWORD ids[2];
    return GetConsoleProcessList(ids, 2) == 1;
}
