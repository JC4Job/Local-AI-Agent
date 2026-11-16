#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

#include <functional>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "sys/Sys.hpp"

// Declared here rather than in imgui_impl_win32.h, so that header does not need <windows.h>.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

/*
    A native window with a Direct3D 11 swap chain and a Dear ImGui context: the platform plumbing only.
    What is drawn in each frame is up to the callback given to Run(). Adapted from ImGui's
    examples/example_win32_directx11.
*/
class GuiHost {
public:
    GuiHost() = default;
    ~GuiHost() { Shutdown(); }
    GuiHost(const GuiHost&) = delete;
    GuiHost& operator=(const GuiHost&) = delete;

    // Width and height are in 96-DPI pixels and are scaled for the primary monitor.
    bool Create(const wchar_t* title, int width, int height) {
        ImGui_ImplWin32_EnableDpiAwareness();
        scale_ = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

        wc_ = {sizeof(wc_), CS_CLASSDC, &GuiHost::WndProc, 0L, 0L, GetModuleHandleW(nullptr),
               nullptr, LoadCursorA(nullptr, IDC_ARROW), nullptr, nullptr, L"AgentChatHost", nullptr};
        if (!RegisterClassExW(&wc_)) {
            wc_ = {};
            return false;
        }
        hwnd_ = CreateWindowW(wc_.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              static_cast<int>(width * scale_), static_cast<int>(height * scale_),
                              nullptr, nullptr, wc_.hInstance, this);
        if (!hwnd_ || !CreateDevice()) {
            Shutdown();
            return false;
        }
        ShowWindow(hwnd_, SW_SHOWDEFAULT);
        UpdateWindow(hwnd_);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // no imgui.ini next to the exe
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGui::StyleColorsSofia();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.FrameRounding  = 6.0f;
        style.ChildRounding  = 6.0f;
        style.FramePadding   = ImVec2(10.0f, 7.0f);
        style.ItemSpacing    = ImVec2(8.0f, 8.0f);
        style.ScaleAllSizes(scale_);
        LoadFont(io);

        ImGui_ImplWin32_Init(hwnd_);
        ImGui_ImplDX11_Init(device_, context_);
        imgui_ready_ = true;
        return true;
    }

    /*
        Runs the message loop until the window is closed, calling draw_frame between ImGui::NewFrame()
        and ImGui::Render() once per displayed frame.
    */
    void Run(const std::function<void()>& draw_frame) {
        for (;;) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) {
                    return;
                }
            }

            // Minimized or screen locked: don't spin.
            if (occluded_ && swap_chain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                Sleep(10);
                continue;
            }
            occluded_ = false;

            // Resizes are queued by WM_SIZE and applied here, outside the window procedure.
            if (resize_w_ != 0 && resize_h_ != 0) {
                CleanupRenderTarget();
                swap_chain_->ResizeBuffers(0, resize_w_, resize_h_, DXGI_FORMAT_UNKNOWN, 0);
                resize_w_ = resize_h_ = 0;
                CreateRenderTarget();
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            draw_frame();
            ImGui::Render();

            const float clear[4] = {0.09f, 0.07f, 0.13f, 1.0f};  // StyleColorsSofia's WindowBg
            context_->OMSetRenderTargets(1, &rtv_, nullptr);
            context_->ClearRenderTargetView(rtv_, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            occluded_ = (swap_chain_->Present(1, 0) == DXGI_STATUS_OCCLUDED);  // vsync
        }
    }

    HWND Window() const { return hwnd_; }

private:
    HWND                    hwnd_        = nullptr;
    WNDCLASSEXW             wc_{};
    ID3D11Device*           device_      = nullptr;
    ID3D11DeviceContext*    context_     = nullptr;
    IDXGISwapChain*         swap_chain_  = nullptr;
    ID3D11RenderTargetView* rtv_         = nullptr;
    UINT                    resize_w_    = 0;
    UINT                    resize_h_    = 0;
    bool                    occluded_    = false;
    bool                    imgui_ready_ = false;
    float                   scale_       = 1.0f;

    /*
        Segoe UI reads better than ImGui's built-in bitmap font and has the accents, dashes and curly quotes
        models write. The ranges must outlive the font atlas build, hence static.
    */
    void LoadFont(ImGuiIO& io) const {
        static const ImWchar ranges[] = {
            0x0020, 0x00FF,  // Basic Latin, Latin-1 (á, ç, ã...)
            0x0100, 0x024F,  // Latin Extended-A and B
            0x2000, 0x206F,  // General Punctuation (dashes, curly quotes, bullet, ellipsis)
            0x20A0, 0x20CF,  // Currency symbols
            0x2190, 0x21FF,  // Arrows
            0,
        };
        const std::filesystem::path font = Sys_FontsFolder() / L"segoeui.ttf";
        std::error_code ec;
        if (std::filesystem::is_regular_file(font, ec) &&
            io.Fonts->AddFontFromFileTTF(font.u8string().c_str(), 18.0f * scale_, nullptr, ranges)) {
            return;
        }
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * scale_;
        io.Fonts->AddFontDefault(&cfg);
    }

    bool CreateDevice() {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount                        = 2;
        sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator   = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow                       = hwnd_;
        sd.SampleDesc.Count                   = 1;
        sd.Windowed                           = TRUE;
        sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level;
        HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                                    D3D11_SDK_VERSION, &sd, &swap_chain_, &device_, &level,
                                                    &context_);
        if (res == DXGI_ERROR_UNSUPPORTED) {  // no GPU driver (e.g. some VMs): use the WARP software renderer
            res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                                D3D11_SDK_VERSION, &sd, &swap_chain_, &device_, &level,
                                                &context_);
        }
        if (res != S_OK) {
            return false;
        }
        CreateRenderTarget();
        return true;
    }

    void CreateRenderTarget() {
        ID3D11Texture2D* back_buffer = nullptr;
        if (SUCCEEDED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
            device_->CreateRenderTargetView(back_buffer, nullptr, &rtv_);
            back_buffer->Release();
        }
    }

    void CleanupRenderTarget() {
        if (rtv_) {
            rtv_->Release();
            rtv_ = nullptr;
        }
    }

    void CleanupDevice() {
        CleanupRenderTarget();
        if (swap_chain_) {
            swap_chain_->Release();
            swap_chain_ = nullptr;
        }
        if (context_) {
            context_->Release();
            context_ = nullptr;
        }
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
    }

    void Shutdown() {
        if (imgui_ready_) {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            imgui_ready_ = false;
        }
        CleanupDevice();
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (wc_.lpszClassName) {
            UnregisterClassW(wc_.lpszClassName, wc_.hInstance);
            wc_ = {};
        }
    }

    static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
            return true;
        }

        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<GuiHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) {
                return 0;
            }
            if (self) {
                self->resize_w_ = LOWORD(lparam);
                self->resize_h_ = HIWORD(lparam);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) {
                return 0;  // Alt would open the (absent) window menu
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
};
