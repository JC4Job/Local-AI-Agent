#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

using json = nlohmann::json;

/*
    Lets another thread stop a request in progress (a Stop button). Cancel() closes the request's handle, which
    is how WinHTTP aborts a synchronous request: the call blocked on it returns at once with an error, even while
    still waiting for the first piece of the reply. Ollama stops generating when the connection closes.
    A cancelled token stays cancelled until Reset(), so a request started after Cancel() is not sent.
*/
class CancelToken {
public:
    void Cancel() {
        std::lock_guard<std::mutex> lock(mu_);
        cancelled_ = true;
        if (active_) {
            WinHttpCloseHandle(active_);
            active_             = nullptr;
            closed_active_      = true;
        }
    }
    void Reset() {
        std::lock_guard<std::mutex> lock(mu_);
        cancelled_ = false;
    }
    bool Cancelled() const {
        std::lock_guard<std::mutex> lock(mu_);
        return cancelled_;
    }

private:
    friend class OllamaClient;

    // The request Cancel() should close. False when already cancelled: do not send it.
    bool Attach(HINTERNET request) {
        std::lock_guard<std::mutex> lock(mu_);
        if (cancelled_) {
            return false;
        }
        active_         = request;
        closed_active_  = false;
        return true;
    }
    // Forgets the request. True when Cancel() closed it, so the owner must not close it again.
    bool Detach() {
        std::lock_guard<std::mutex> lock(mu_);
        const bool closed = closed_active_;
        active_           = nullptr;
        closed_active_    = false;
        return closed;
    }

    mutable std::mutex mu_;
    bool      cancelled_        = false;
    bool      closed_active_    = false;
    HINTERNET active_           = nullptr;
};


class OllamaClient {
public:
    // `options` is sent as Ollama's "options" object on every request (num_ctx, num_predict, temperature...).
    explicit OllamaClient(std::string endpoint = "http://127.0.0.1:11434/api/chat", json options = json::object()) : endpoint_url_(std::move(endpoint)), options_(std::move(options)) {}

    // Replaces the options sent with every request. Ollama reloads the model when num_ctx changes.
    void SetOptions(json options) { options_ = std::move(options); }

    // Receives the reply text as the model writes it, a few characters at a time.
    using TextHandler = std::function<void(const std::string&)>;

    /*
        The reply is streamed (Ollama's "stream": true): `on_text`, when given, receives each piece of text as the
        model writes it. The return value is the whole reply, shaped like a non-streamed one:
        {"message": {"role", "content", "tool_calls"?}, "done_reason", ...}.
        Streaming also keeps a long reply from timing out: the receive timeout applies to each piece, not to the
        whole reply, which on a slow machine can take longer than the timeout.
        Returns {"error": ...} when Ollama reports one, and an empty object when it cannot be reached. A reply cut
        short (connection lost, error mid-way, `cancel` triggered) has what arrived, with done_reason "interrupted".
    */
    json Chat(const std::string& model, const json& messages, const json& tools = json::array(),
              const TextHandler& on_text = nullptr, CancelToken* cancel = nullptr) const {
        json payload = {
            {"model", model},
            {"messages", messages},
            {"stream", true}
        };

        if (!tools.empty()) {
            payload["tools"] = tools;
        }
        if (!options_.empty()) {
            payload["options"] = options_;
        }

        // One JSON object per line. A read can end mid-line: the rest waits in `pending` for the next read.
        StreamedReply   reply;
        std::string     pending;
        // replace: an invalid UTF-8 byte (text from a program writing in another code page) must not throw here
        const std::string body = payload.dump(-1, ' ', false, json::error_handler_t::replace);
        PostRequest(endpoint_url_, body, kDefaultTimeoutMs, [&](const char* data, size_t size) {
            pending.append(data, size);
            size_t start = 0;
            for (size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1) {
                reply.Add(pending.substr(start, nl - start), on_text);
            }
            pending.erase(0, start);
        }, cancel);
        // An error reply (unknown model, no tool support...) is a single JSON object without a newline.
        if (!pending.empty()) {
            reply.Add(pending, on_text);
        }
        return reply.Result();
    }

    /*
        One vector per input, in the same order, from an embedding model (POST /api/embed on the chat endpoint's
        server). Returns an error message, empty on success. `options` is sent as given, not the chat options:
        Ollama reloads a model whose options change, so callers keep them the same from call to call.
        `timeout_ms` bounds the wait for the reply.
    */
    std::string Embed(const std::string& model, const std::vector<std::string>& inputs,
                      std::vector<std::vector<float>>& vectors, const json& options = json::object(),
                      DWORD timeout_ms = kDefaultTimeoutMs) const {
        vectors.clear();
        json payload = {{"model", model}, {"input", inputs}};
        if (!options.empty()) {
            payload["options"] = options;
        }

        const std::string url = ServerUrl("/api/embed");
        // replace: a malformed UTF-8 byte in a document must not throw here
        const std::string raw = PerformPostRequest(
            url, payload.dump(-1, ' ', false, json::error_handler_t::replace), timeout_ms);
        if (raw.empty()) {
            return "no reply from " + url + " (Ollama not running, or busy for more than " + std::to_string(timeout_ms / 1000) + " s)";
        }
        const json parsed = json::parse(raw, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            return "invalid reply from " + url;
        }
        if (parsed.contains("error")) {
            const json& error = parsed["error"];
            return error.is_string() ? error.get<std::string>() : error.dump();
        }
        auto list = parsed.find("embeddings");
        if (list == parsed.end() || !list->is_array() || list->size() != inputs.size()) {
            return "Ollama returned " + std::to_string(list != parsed.end() && list->is_array() ? list->size() : 0) + " embeddings for " + std::to_string(inputs.size()) + " inputs";
        }
        vectors.reserve(list->size());
        for (const json& item : *list) {
            if (!item.is_array()) {
                return "Ollama returned an embedding that is not a list of numbers";
            }
            std::vector<float> v;
            v.reserve(item.size());
            for (const json& x : item) {
                if (!x.is_number()) {
                    return "Ollama returned an embedding that is not a list of numbers";
                }
                v.push_back(x.get<float>());
            }
            vectors.push_back(std::move(v));
        }
        return {};
    }

    /*
        The models pulled in Ollama (GET /api/tags), by name, sorted. Returns an error message, empty on
        success. It loads no model, so it answers at once even while another request is generating.
    */
    std::string ListModels(std::vector<std::string>& names, DWORD timeout_ms = 3000) const {
        names.clear();
        const std::string url = ServerUrl("/api/tags");
        std::string raw;
        const bool ok = PostRequest(url, std::string(), timeout_ms,
                                    [&raw](const char* data, size_t size) { raw.append(data, size); },
                                    nullptr, L"GET");
        if (!ok || raw.empty()) {
            return "no reply from " + url + " (is Ollama running?)";
        }
        const json parsed = json::parse(raw, nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_object() || !parsed.contains("models") || !parsed["models"].is_array()) {
            return "invalid reply from " + url;
        }
        for (const json& model : parsed["models"]) {
            if (model.is_object() && model.contains("name") && model["name"].is_string()) {
                names.push_back(model["name"].get<std::string>());
            }
        }
        std::sort(names.begin(), names.end());
        return {};
    }

private:
    static constexpr DWORD kDefaultTimeoutMs = 600000;

    std::string endpoint_url_;
    json        options_;

    // Puts a streamed reply back together, one line (chunk) at a time.
    struct StreamedReply {
        std::string content, thinking, error;
        json        tool_calls  = json::array();
        json        last;              // the final chunk ("done": true): done_reason and timings
        bool        got_message = false;
        bool        done        = false;

        static std::string Text(const json& obj, const char* key) {
            auto it = obj.find(key);
            return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
        }

        void Add(const std::string& line, const TextHandler& on_text) {
            if (line.find_first_not_of(" \t\r") == std::string::npos) {
                return;
            }
            json chunk = json::parse(line, nullptr, /*allow_exceptions=*/false);
            if (chunk.is_discarded() || !chunk.is_object()) {
                return;
            }
            if (chunk.contains("error")) {
                const json& e = chunk["error"];
                error = e.is_string() ? e.get<std::string>() : e.dump();
                return;
            }
            auto msg = chunk.find("message");
            if (msg != chunk.end() && msg->is_object()) {
                got_message = true;
                const std::string piece = Text(*msg, "content");
                if (!piece.empty()) {
                    content += piece;
                    if (on_text) {
                        on_text(piece);
                    }
                }
                thinking += Text(*msg, "thinking");
                auto calls = msg->find("tool_calls");
                if (calls != msg->end() && calls->is_array()) {
                    for (const json& call : *calls) {
                        tool_calls.push_back(call);
                    }
                }
            }
            if (chunk.value("done", false)) {
                done = true;
                last = std::move(chunk);
            }
        }

        json Result() const {
            if (!got_message && !done) {
                return error.empty() ? json::object() : json{{"error", error}};
            }
            json result = done ? last : json::object();
            json message = {{"role", "assistant"}, {"content", content}};
            if (!tool_calls.empty()) {
                message["tool_calls"] = tool_calls;
            }
            if (!thinking.empty()) {
                message["thinking"] = thinking;
            }
            result["message"] = std::move(message);
            if (!done) {
                result["done_reason"] = "interrupted";
                if (!error.empty()) {
                    result["error"] = error;
                }
            }
            return result;
        }
    };

    // Another API path on the endpoint's server: "http://host:11434/api/chat" -> "http://host:11434/api/embed".
    std::string ServerUrl(const std::string& path) const {
        const size_t api = endpoint_url_.find("/api/");
        return (api == std::string::npos ? endpoint_url_ : endpoint_url_.substr(0, api)) + path;
    }

    struct HandleCloser {
        void operator()(HINTERNET h) const {
            if (h) {
                WinHttpCloseHandle(h);
            }
        }
    };
    using Handle = std::unique_ptr<void, HandleCloser>;

    static std::wstring Widen(const std::string& s) {
        if (s.empty()) {
            return {};
        }
        int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
        return out;
    }

    // `cancel`: a request stopped on purpose fails too, and there is nothing to report about it.
    static bool Fail(const char* step, const CancelToken* cancel = nullptr) {
        // WARNING:capture before any I/O can overwrite it
        DWORD err = GetLastError();
        if (cancel && cancel->Cancelled()) {
            return false;
        }
        std::cerr << "[OllamaClient] " << step << " failed (WinHTTP error " << err << ")\n";
        return false;
    }

    // The whole response body; empty when the request fails.
    std::string PerformPostRequest(const std::string& target_url, const std::string& json_payload,
                                   DWORD receive_timeout_ms) const {
        std::string body;
        const bool ok = PostRequest(target_url, json_payload, receive_timeout_ms,
                                    [&body](const char* data, size_t size) { body.append(data, size); });
        return ok ? body : std::string();
    }

    using DataHandler = std::function<void(const char* data, size_t size)>;

    /*
        Hands the response body to `on_data` piece by piece, as it arrives. False when the request fails, possibly
        after some pieces were handed over. The receive timeout applies to each wait for data. `cancel`, when
        given, can stop the request from another thread. `verb`: POST, or GET with an empty payload (ListModels).
    */
    bool PostRequest(const std::string& target_url, const std::string& json_payload, DWORD receive_timeout_ms,
                     const DataHandler& on_data, CancelToken* cancel = nullptr, const wchar_t* verb = L"POST") const {
        std::wstring url = Widen(target_url);

        URL_COMPONENTS uc{};
        uc.dwStructSize         = sizeof(uc);
        uc.dwHostNameLength     = static_cast<DWORD>(-1);
        uc.dwUrlPathLength      = static_cast<DWORD>(-1);
        uc.dwExtraInfoLength    = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
            return Fail("WinHttpCrackUrl");
        }

        std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        std::wstring path = uc.dwUrlPathLength ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) : L"/";
        if (uc.dwExtraInfoLength) {
            path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
        }

        Handle session(WinHttpOpen(L"OllamaClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) {
            return Fail("WinHttpOpen");
        }

        WinHttpSetTimeouts(session.get(), 0, 60000, 60000, static_cast<int>(receive_timeout_ms));

        Handle connection(WinHttpConnect(session.get(), host.c_str(), uc.nPort, 0));
        if (!connection) {
            return Fail("WinHttpConnect");
        }

        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        Handle request(WinHttpOpenRequest(connection.get(), verb, path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!request) {
            return Fail("WinHttpOpenRequest");
        }

        /*
            From here on, `cancel` may close the request handle from another thread. When it did, the handle must
            not be closed a second time: the guard (destroyed before `request`) takes it out of `request` first.
        */
        if (cancel && !cancel->Attach(request.get())) {
            return false;
        }
        struct DetachGuard {
            CancelToken* cancel;
            Handle&      request;
            ~DetachGuard() {
                if (cancel && cancel->Detach()) {
                    request.release();
                }
            }
        } guard{cancel, request};

        DWORD body_size = static_cast<DWORD>(json_payload.size());
        if (!WinHttpSendRequest(request.get(), L"Content-Type: application/json\r\n", static_cast<DWORD>(-1L), const_cast<char*>(json_payload.data()), body_size, body_size, 0)) {
            return Fail("WinHttpSendRequest", cancel);
        }
        if (!WinHttpReceiveResponse(request.get(), nullptr)) {
            return Fail("WinHttpReceiveResponse", cancel);
        }

        std::vector<char> buffer;
        for (;;) {
            // Blocks until data arrives (a streamed reply sends it as the model writes) or the body ends (0).
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available)) {
                return Fail("WinHttpQueryDataAvailable", cancel);
            }
            if (available == 0) {
                break;
            }

            buffer.resize(available);
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), available, &read)) {
                return Fail("WinHttpReadData", cancel);
            }
            if (read) {
                on_data(buffer.data(), read);
            }
        }
        return true;
    }
};
