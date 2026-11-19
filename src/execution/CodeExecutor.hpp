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
#include <string>
#include <vector>

/*
 * CodeExecutor - runs the code the model writes (execute_code) and the search scripts.
 *
 * The model generates the code as text; this wraps it into direct Win32 calls (CreateProcess, a job object,
 * pipes), for performance: no high-level language runtime in between. Each run gets a temp script, a time
 * limit, a memory cap and a process cap for its whole process tree, and its output back as UTF-8.
 *
 * WARNING: the limits contain a runaway script; they are not a sandbox. The code runs as the user, with the
 * user's files and network: whether it may run is decided before this, by CLISecurityChecking and the user.
 */

class CodeExecutor {
public:
    struct Limits {
        DWORD  timeout_ms       = 10000;               // whole process tree; 0 = no timeout
        SIZE_T memory_bytes     = 512u * 1024 * 1024;  // committed memory of the whole job; 0 = no cap
        DWORD  max_processes    = 32;                  // concurrent processes in the job; 0 = no cap
        size_t max_output_bytes = 16 * 1024;           // per stream, the rest is discarded; 0 = no cap
    };

    struct ExecutionResult {
        int         exit_code = -1;
        std::string stdout_str;   // UTF-8, CRLF normalized to LF
        std::string stderr_str;   // UTF-8, CRLF normalized to LF
        bool        timed_out = false;
        bool        truncated = false;  // stdout or stderr exceeded max_output_bytes
        DWORD       elapsed_ms = 0;
        std::string error;              // non-empty when the process could not be started

        bool Started() const { return error.empty(); }
    };

    // Writes `code` to a temp script and runs it. Languages: python, powershell, cmd.
    static ExecutionResult ExecuteCode(const std::string& language, const std::string& code) {
        return ExecuteCode(language, code, Limits());
    }

    /*
     * `working_dir`: the folder the code runs in, where relative paths in it go. Empty: this program's own
     * working folder.
     *
     * WARNING: it must exist: CreateProcess fails otherwise.
     */
    static ExecutionResult ExecuteCode(const std::string& language, const std::string& code, const Limits& limits,
                                       const std::string& working_dir = {}) {
        ExecutionResult result;
        const std::string lang = ToLower(language);

        std::string extension, content, launcher;
        if (lang == "python" || lang == "py") {
            extension = ".py";
            content   = code;
            launcher  = "python -X utf8 ";
        } else if (lang == "powershell" || lang == "pwsh" || lang == "ps1") {
            extension = ".ps1";
            content   = "\xEF\xBB\xBF" + code;  // WARNING: Windows PowerShell 5.1 reads BOM-less scripts as ANSI
            launcher  = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ";
        } else if (lang == "cmd" || lang == "batch" || lang == "bat") {
            extension = ".cmd";
            content   = "@echo off\r\n" + ToCrlf(code);  // WARNING: cmd mis-parses labels in LF-only files
        } else {
            result.error = "Unsupported language '" + language + "'. Supported: python, powershell, cmd.";
            return result;
        }

        std::wstring script = WriteTempScript(extension, content, result.error);
        if (script.empty()) {
            return result;
        }

        /*
         * chcp first so the child's console, and every tool the script starts, emits UTF-8.
         *
         * NOTE: /s strips only the outer quotes, so the quoted script path survives intact.
         */
        const std::string command =
            "cmd.exe /d /s /c \"chcp 65001 >nul & " + launcher + "\"" + Narrow(script) + "\"\"";

        result = ExecuteCommand(command, limits, working_dir);
        DeleteFileW(script.c_str());
        return result;
    }

    // Runs a raw command line (UTF-8) with no shell in between.
    static ExecutionResult ExecuteCommand(const std::string& command) {
        return ExecuteCommand(command, Limits());
    }

    static ExecutionResult ExecuteCommand(const std::string& command, const Limits& limits,
                                          const std::string& working_dir = {}) {
        ExecutionResult result;
        const ULONGLONG started = GetTickCount64();

        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;

        Handle out_rd, out_wr, err_rd, err_wr;
        if (!CreatePipe(out_rd.put(), out_wr.put(), &sa, 0) ||
            !CreatePipe(err_rd.put(), err_wr.put(), &sa, 0)) {
            result.error = LastErrorMessage("CreatePipe");
            return result;
        }
        // WARNING: our read ends must not leak into the child, or the pipes never reach EOF.
        SetHandleInformation(out_rd.get(), HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err_rd.get(), HANDLE_FLAG_INHERIT, 0);

        // Code that reads stdin gets EOF instead of blocking on the user's console until timeout.
        Handle nul_in(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  OPEN_EXISTING, 0, nullptr));

        Handle job(CreateJobObjectW(nullptr, nullptr));
        if (!job) {
            result.error = LastErrorMessage("CreateJobObject");
            return result;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        auto& basic = jeli.BasicLimitInformation;
        basic.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (limits.memory_bytes) {
            basic.LimitFlags  |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            jeli.JobMemoryLimit = limits.memory_bytes;
        }
        if (limits.max_processes) {
            basic.LimitFlags         |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
            basic.ActiveProcessLimit  = limits.max_processes;
        }
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            result.error = LastErrorMessage("SetInformationJobObject");
            return result;
        }

        STARTUPINFOW si{};
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = nul_in.get();
        si.hStdOutput = out_wr.get();
        si.hStdError  = err_wr.get();

        std::wstring wide = Widen(command);
        std::vector<wchar_t> cmdline(wide.begin(), wide.end());
        cmdline.push_back(L'\0');
        const std::wstring dir = Widen(working_dir);

        // WARNING: suspended until it is in the job, so no grandchild can be spawned outside the limits.
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, dir.empty() ? nullptr : dir.c_str(),
                            &si, &pi)) {
            result.error = LastErrorMessage("CreateProcess");
            return result;
        }
        Handle process(pi.hProcess), main_thread(pi.hThread);

        // WARNING: drop our copies of the child's ends: EOF arrives only once the last process holding them exits.
        out_wr.reset();
        err_wr.reset();
        nul_in.reset();

        if (!AssignProcessToJobObject(job.get(), process.get())) {
            result.error = LastErrorMessage("AssignProcessToJobObject");
            TerminateProcess(process.get(), 1);
            return result;
        }

        // WARNING: drain both pipes concurrently, or a child that fills a pipe buffer deadlocks.
        const size_t cap = limits.max_output_bytes ? limits.max_output_bytes : std::string::npos;
        PipeReader out_reader(out_rd.get(), cap), err_reader(err_rd.get(), cap);
        PipeReader* readers[2] = {&out_reader, &err_reader};
        HANDLE threads[2] = {
            CreateThread(nullptr, 0, &PipeReader::Run, &out_reader, 0, nullptr),
            CreateThread(nullptr, 0, &PipeReader::Run, &err_reader, 0, nullptr),
        };

        if (!threads[0] || !threads[1]) {
            result.error = LastErrorMessage("CreateThread");
        } else {
            ResumeThread(main_thread.get());
            const DWORD wait = limits.timeout_ms ? limits.timeout_ms : INFINITE;
            if (WaitForSingleObject(process.get(), wait) == WAIT_TIMEOUT) {
                result.timed_out = true;
            } else {
                DWORD code = 0;
                GetExitCodeProcess(process.get(), &code);
                result.exit_code = static_cast<int>(code);
            }
        }

        /*
         * Kill whatever is left in the tree (the timed-out process, background children it spawned,
         * or the never-resumed process) so every write end closes and the readers finish.
         */
        TerminateJobObject(job.get(), 1);
        JoinReaders(threads, readers);

        result.truncated  = out_reader.truncated || err_reader.truncated;
        result.stdout_str = NormalizeOutput(std::move(out_reader.data), out_reader.truncated);
        result.stderr_str = NormalizeOutput(std::move(err_reader.data), err_reader.truncated);
        result.elapsed_ms = static_cast<DWORD>(GetTickCount64() - started);
        return result;
    }

private:
    class Handle {
    public:
        Handle() = default;
        explicit Handle(HANDLE h) { reset(h); }
        ~Handle() { reset(); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        void reset(HANDLE h = nullptr) {
            if (h_) {
                CloseHandle(h_);
            }
            h_ = (h == INVALID_HANDLE_VALUE) ? nullptr : h;
        }
        HANDLE get() const { return h_; }
        HANDLE* put() { reset(); return &h_; }
        explicit operator bool() const { return h_ != nullptr; }

    private:
        HANDLE h_ = nullptr;
    };

    struct PipeReader {
        PipeReader(HANDLE p, size_t c) : pipe(p), cap(c) {}

        HANDLE            pipe;
        size_t            cap;
        std::string       data;
        bool              truncated = false;
        std::atomic<bool> stop{false};

        static DWORD WINAPI Run(LPVOID param) {
            auto* self = static_cast<PipeReader*>(param);
            char buf[4096];
            DWORD n = 0;
            // WARNING: keep draining past the cap, or the child blocks on a full pipe.
            while (!self->stop.load() && ReadFile(self->pipe, buf, sizeof(buf), &n, nullptr)) {
                const size_t room = self->cap - std::min(self->cap, self->data.size());
                if (n > room) {
                    self->truncated = true;
                }
                self->data.append(buf, std::min<size_t>(n, room));
            }
            return 0;
        }
    };

    static void JoinReaders(HANDLE (&threads)[2], PipeReader* (&readers)[2]) {
        HANDLE live[2];
        DWORD count = 0;
        for (HANDLE t : threads) {
            if (t) {
                live[count++] = t;
            }
        }
        /*
         * Immediate in practice: the job is dead, so nothing holds the write ends. Only a handle
         * duplicated into a process outside the job could keep a read blocked; cancel it then.
         */
        DWORD wait = 2000;
        while (count && WaitForMultipleObjects(count, live, TRUE, wait) == WAIT_TIMEOUT) {
            for (PipeReader* r : readers) {
                r->stop = true;
            }
            for (DWORD i = 0; i < count; ++i) {
                CancelSynchronousIo(live[i]);
            }
            wait = 100;
        }
        for (DWORD i = 0; i < count; ++i) {
            CloseHandle(live[i]);
        }
    }

    static std::wstring WriteTempScript(const std::string& extension, const std::string& content,
                                        std::string& error) {
        wchar_t dir[MAX_PATH + 1];
        const DWORD len = GetTempPathW(MAX_PATH + 1, dir);
        if (len == 0 || len > MAX_PATH) {
            error = LastErrorMessage("GetTempPath");
            return {};
        }

        static std::atomic<unsigned> counter{0};
        for (int attempt = 0; attempt < 16; ++attempt) {
            std::wstring path = std::wstring(dir, len) + L"agent_exec_" +
                                std::to_wstring(GetCurrentProcessId()) + L"_" +
                                std::to_wstring(GetTickCount64()) + L"_" +
                                std::to_wstring(counter++) + Widen(extension);

            Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr));
            if (!file) {
                if (GetLastError() == ERROR_FILE_EXISTS) {
                    continue;
                }
                error = LastErrorMessage("Creating temp script");
                return {};
            }
            DWORD written = 0;
            if (!WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr) ||
                written != content.size()) {
                error = LastErrorMessage("Writing temp script");
                file.reset();
                DeleteFileW(path.c_str());
                return {};
            }
            return path;
        }
        error = "Creating temp script failed: no free file name.";
        return {};
    }

    // WARNING: child output is raw bytes, and the JSON sent to the model needs valid UTF-8.
    static std::string NormalizeOutput(std::string s, bool truncated) {
        if (truncated) {
            DropCutUtf8Sequence(s);
        }
        if (s.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            s.erase(0, 3);
        }
        if (!IsValidUtf8(s)) {
            // Some tool ignored chcp 65001; decode it as the OEM console code page instead.
            s = Narrow(Widen(s, CP_OEMCP));
        }
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') {
                continue;
            }
            out.push_back(s[i]);
        }
        return out;
    }

    // The output cap can split a multi-byte character; drop the incomplete tail.
    static void DropCutUtf8Sequence(std::string& s) {
        size_t i = s.size(), continuation = 0;
        while (i > 0 && continuation < 3 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
            --i;
            ++continuation;
        }
        if (i == 0) {
            return;
        }
        const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
        const size_t needed = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
        if (needed > continuation) {
            s.resize(i - 1);
        }
    }

    static bool IsValidUtf8(const std::string& s) {
        return s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                                static_cast<int>(s.size()), nullptr, 0) > 0;
    }

    static std::wstring Widen(const std::string& s, UINT codepage = CP_UTF8) {
        if (s.empty()) {
            return {};
        }
        const int n = MultiByteToWideChar(codepage, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(n, L'\0');
        MultiByteToWideChar(codepage, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
        return out;
    }

    static std::string Narrow(const std::wstring& w) {
        if (w.empty()) {
            return {};
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                          nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
        return out;
    }

    static std::string ToLower(std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    static std::string ToCrlf(const std::string& s) {
        std::string out;
        out.reserve(s.size() + s.size() / 16);
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) {
                out.push_back('\r');
            }
            out.push_back(s[i]);
        }
        return out;
    }

    static std::string LastErrorMessage(const char* step) {
        const DWORD err = GetLastError();
        wchar_t* buf = nullptr;
        const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
        std::string text = n ? Narrow(std::wstring(buf, n)) : std::string();
        if (buf) {
            LocalFree(buf);
        }
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' ||
                                 text.back() == '.')) {
            text.pop_back();
        }
        return std::string(step) + " failed (error " + std::to_string(err) + ")" +
               (text.empty() ? "" : ": " + text);
    }
};
