# Builds the programs and puts them in dist\agent\, ready to copy to another computer. The programs find
# config\, scripts\ and data\ next to bin\ (src/framework/FileSystem.hpp), so the folder works wherever it goes.
#   bin\       agent_gui.exe, agent.exe, restore_config.exe (linked statically: no DLLs to install)
#   config\    settings.txt, the prompts (and their factory copies, defaults\) and the agents made in the window. Not machines\ (each computer saves
#              its own with Settings) nor backup\.
#   scripts\   the search scripts (Python)
#   data\      the knowledge base and its index, unless -NoData
#   run.ps1, README.txt
# Last, it checks that no file in it holds this computer's user folder or user name.
# Usage: .\package.ps1            build and package
#        .\package.ps1 -NoData    without the knowledge base
#        .\package.ps1 -Zip       also dist\agent.zip
param([switch]$NoData, [switch]$Zip)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

& .\build.ps1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$out = Join-Path $PSScriptRoot 'dist\agent'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\bin", "$out\config", "$out\scripts", "$out\data\knowledge" | Out-Null

Copy-Item bin\agent_gui.exe, bin\agent.exe, bin\restore_config.exe "$out\bin"
Copy-Item config\*.txt "$out\config"
# The factory copies of the prompts: read when a prompt file is missing, and what restore_config.exe writes back.
New-Item -ItemType Directory -Force "$out\config\defaults" | Out-Null
Copy-Item config\defaults\*.txt "$out\config\defaults"
if (Test-Path config\agents) {
    New-Item -ItemType Directory -Force "$out\config\agents" | Out-Null
    Copy-Item config\agents\*.txt "$out\config\agents"
}
Copy-Item scripts\*.py "$out\scripts"
if (-not $NoData) {
    if (Test-Path data\knowledge) { Copy-Item data\knowledge\* "$out\data\knowledge" -Recurse }
    if (Test-Path data\rag\knowledge.idx) {
        New-Item -ItemType Directory -Force "$out\data\rag" | Out-Null
        Copy-Item data\rag\knowledge.idx "$out\data\rag"
    }
}
Copy-Item run.ps1 $out

# The models the README names: the ones settings.txt asks for.
$models = @{ model = 'llama3.2'; embed_model = 'bge-m3' }
foreach ($line in Get-Content config\settings.txt -Encoding UTF8) {
    $line = $line.Trim()
    if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
    $key, $value = $line.Split([char[]]'=', 2) | ForEach-Object { $_.Trim() }
    if ($models.ContainsKey($key)) { $models[$key] = $value }
}
$pulls = (@($models.model, $models.embed_model) | Where-Object { $_ } | ForEach-Object { "      ollama pull $_" }) -join "`r`n"

$readme = @"
Local AI agent, with a window (bin\agent_gui.exe) and a console (bin\agent.exe), on models run by Ollama.

Copy this whole folder anywhere: the programs find config\, scripts\ and data\ next to bin\.

What the computer needs
- Windows 10 or 11, 64-bit. The programs need no other DLLs.
- Ollama (https://ollama.com), with the models config\settings.txt names:
$pulls
- Python 3 on PATH, as "python": the knowledge base and internet searches and execute_code use it.
  scripts\reader.py, which turns a PDF into a knowledge base document, also needs:
      python -m pip install pymupdf

First run
1. powershell -ExecutionPolicy Bypass -File run.ps1
                            starts Ollama if needed and loads the models
2. bin\agent_gui.exe        the window. Settings sets the context and reply sizes for this computer;
                            + in the tab bar makes more agents.
3. bin\agent.exe --index    gives the knowledge base passages their vectors for search by meaning
                            (slow the first time; Ctrl+C stops it and keeps the progress)

If the configuration breaks: bin\restore_config.exe writes the defaults back, and keeps a copy of what it
replaces in config\backup\.
"@
[IO.File]::WriteAllText("$out\README.txt", $readme.Replace("`r`n", "`n").Replace("`n", "`r`n"), (New-Object Text.UTF8Encoding $false))

# No file may hold this computer's user folder or user name, in UTF-8/ANSI or UTF-16.
$needles = @($env:USERPROFILE, $env:USERNAME) | Where-Object { $_ }
$found = foreach ($file in Get-ChildItem $out -Recurse -File) {
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    $texts = [Text.Encoding]::UTF8.GetString($bytes), [Text.Encoding]::Unicode.GetString($bytes)
    foreach ($needle in $needles) {
        foreach ($text in $texts) {
            if ($text.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $file.FullName.Substring($out.Length + 1)
                break
            }
        }
    }
}
if ($found) {
    Write-Host "These files hold this computer's user folder or user name:" -ForegroundColor Red
    $found | Sort-Object -Unique | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}

if ($Zip) {
    Compress-Archive -Path $out -DestinationPath (Join-Path $PSScriptRoot 'dist\agent.zip') -Force
}
$size = (Get-ChildItem $out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("packaged in {0} ({1:N1} MB), checked: no path or name of this computer" -f $out, $size)
