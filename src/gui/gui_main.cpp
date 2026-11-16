/*
    Window front end. The chatbot itself is ChatAgent (ChatAgent.hpp); ChatWindow draws it with Dear ImGui,
    and GuiHost provides the native window and Direct3D 11. Build with build.ps1.
*/

#include "gui/GuiHost.hpp"
#include "gui/ChatWindow.hpp"
#include "agent/AgentConfig.hpp"
#include "common/Utils.hpp"

int main() {
    GuiHost host;
    ChatAgentConfig config;
    const std::wstring title = L"Agent Chat " + Widen(config.agent_name);
    if (!host.Create(title.c_str(), 960, 720)) {
        MessageBoxW(nullptr, L"Could not create the window: Direct3D 11 is not available.", title.c_str(), MB_ICONERROR);
        return 1;
    }
    
    // owns the agents: the default one and those made in the window (config\agents\)
    ChatWindow chat;
    host.Run([&chat] { chat.Draw(); });
    return 0;
}
