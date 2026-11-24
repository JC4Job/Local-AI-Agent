/*
 * Window front end. The chatbot itself is ChatAgent (ChatAgent.hpp); ChatWindow draws it with Dear ImGui,
 * and GuiHost provides the native window and Direct3D 11. The renderer (renderer/RenderSystem.hpp) draws the
 * agents' avatars on the same device. Build with build.ps1.
 */

#include <memory>
#include <string>

#include "gui/GuiHost.hpp"
#include "gui/ChatWindow.hpp"
#include "agent/AgentConfig.hpp"
#include "common/Utils.hpp"
#include "renderer/RenderSystem.hpp"
#include "renderer/dx11/RenderBackendDX11.hpp"

int main() {
    GuiHost host;
    ChatAgentConfig config;
    const std::wstring title = L"Agent Chat " + Widen(config.agent_name);
    if (!host.Create(title.c_str(), 960, 720)) {
        MessageBoxW(nullptr, L"Could not create the window: Direct3D 11 is not available.", title.c_str(), MB_ICONERROR);
        return 1;
    }

    // NOTE: a renderer that fails leaves the window working: each avatar square says why it is empty.
    std::string render_error;
    renderSystem.Init(std::make_unique<RenderBackendDX11>(host.Device(), host.Context()), render_error);

    {
        // Owns the agents: the default one and those made in the window (config\agents\).
        ChatWindow chat;
        host.Run([&chat] {
            chat.Draw();
            renderSystem.EndFrame();  // the avatars' passes, before ImGui draws their pictures
        });
    }
    // WARNING: after the tabs have let their targets go, before the window lets the device go.
    renderSystem.Shutdown();
    return 0;
}
