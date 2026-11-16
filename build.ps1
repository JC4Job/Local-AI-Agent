# Builds the chatbot with MSYS2 UCRT64 g++. Executables go to bin/, Dear ImGui objects to build/.
#   bin/agent_gui.exe  window front end (src/gui/gui_main.cpp)
#   bin/agent.exe      console front end (src/cli/cli_main.cpp)
#   bin/restore_config.exe  writes the default config back when it is broken (src/tools/restore_config.cpp)
# Usage: .\build.ps1          build both
#        .\build.ps1 -Clean   recompile Dear ImGui too
param([switch]$Clean)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$imgui = 'third_party/imgui'
# -Isrc: project headers are included by their folder, e.g. "agent/ChatAgent.hpp".
$common = @('-std=c++17', '-O2', '-Isrc', "-I$imgui", "-I$imgui/backends")
# -static: the exes run without MSYS2's libstdc++/libgcc DLLs on PATH (e.g. double-clicked from Explorer).
$link = @('-static', '-lwinhttp')

if ($Clean -and (Test-Path build)) { Remove-Item -Recurse -Force build }
New-Item -ItemType Directory -Force build, bin | Out-Null

# Dear ImGui changes rarely: compile each source once and reuse the object until the source changes.
$imguiSources = @(
    "$imgui/imgui.cpp", "$imgui/imgui_draw.cpp", "$imgui/imgui_tables.cpp", "$imgui/imgui_widgets.cpp",
    "$imgui/backends/imgui_impl_win32.cpp", "$imgui/backends/imgui_impl_dx11.cpp",
    "$imgui/misc/cpp/imgui_stdlib.cpp"
)
$objects = foreach ($src in $imguiSources) {
    $obj = "build/" + [IO.Path]::GetFileNameWithoutExtension($src) + ".o"
    if (-not (Test-Path $obj) -or (Get-Item $src).LastWriteTime -gt (Get-Item $obj).LastWriteTime) {
        Write-Host "compiling $src"
        & g++ @common -c $src -o $obj
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    $obj
}

Write-Host "linking bin/agent_gui.exe"
& g++ @common -Wall src/gui/gui_main.cpp @objects -o bin/agent_gui.exe -mwindows @link -ld3d11 -ld3dcompiler -ldxgi -ldwmapi -lgdi32 -limm32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "linking bin/agent.exe"
& g++ @common -Wall src/cli/cli_main.cpp -o bin/agent.exe @link
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "linking bin/restore_config.exe"
& g++ @common -Wall src/tools/restore_config.cpp -o bin/restore_config.exe -static
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "done"
