# Starts the local models the agent uses in Ollama: the embedding model and the chat model named in
# config\settings.txt and this computer's config\machines\<computer name>.txt (defaults: bge-m3 with num_ctx
# 1024, llama3.2 with num_ctx 16384).
# Each is loaded with the options the agent sends: Ollama reloads a model whose num_ctx changes.
# The chat model goes last: on a computer with a small GPU Ollama keeps only one of them loaded at a time.
# Nothing is downloaded: a missing model is reported with the command that pulls it.
# Usage: .\run.ps1                 start Ollama if needed and load the models
#        .\run.ps1 -KeepAlive 2h   keep them loaded that long while idle (default 30m)
param([string]$KeepAlive = '30m')

$ErrorActionPreference = 'Stop'

# The agent's settings, read as the agent reads them: config\settings.txt, then this computer's
# config\machines\<computer name>.txt over it (the Settings window of agent_gui.exe writes it).
# These defaults match src/agent/AgentConfig.hpp, for keys both files leave out.
$settings = @{
    model = 'llama3.2'; embed_model = 'bge-m3'; endpoint = 'http://127.0.0.1:11434/api/chat'
    num_ctx = '16384'; num_predict = '4096'; embed_num_ctx = '1024'
}
$machine = if ($env:COMPUTERNAME) { $env:COMPUTERNAME } else { 'local' }
$machineFile = Join-Path $PSScriptRoot "config\machines\$machine.txt"
foreach ($settingsFile in (Join-Path $PSScriptRoot 'config\settings.txt'), $machineFile) {
    if (-not (Test-Path $settingsFile)) { continue }
    if ($settingsFile -eq $machineFile) { Write-Host "settings for this computer from $machineFile" }
    foreach ($line in Get-Content $settingsFile -Encoding UTF8) {
        $line = $line.Trim()
        if (-not $line -or $line.StartsWith('#') -or -not $line.Contains('=')) { continue }
        $key, $value = $line.Split([char[]]'=', 2) | ForEach-Object { $_.Trim() }
        if ($settings.ContainsKey($key)) { $settings[$key] = $value }
    }
}
$server = $settings.endpoint -replace '/api/.*$', ''

$models = @()
if ($settings.embed_model) {
    $models += [pscustomobject]@{ Name = $settings.embed_model; Api = 'embed'
                                  Options = @{ num_ctx = [int]$settings.embed_num_ctx } }
}
$models += [pscustomobject]@{ Name = $settings.model; Api = 'chat'
                              Options = @{ num_ctx = [int]$settings.num_ctx; num_predict = [int]$settings.num_predict } }

function Fail([string]$message) {
    Write-Host $message -ForegroundColor Red
    exit 1
}

function Test-Ollama {
    try { Invoke-RestMethod "$server/api/version" -TimeoutSec 2 | Out-Null; return $true } catch { return $false }
}

if (-not (Test-Ollama)) {
    if (-not (Get-Command ollama -ErrorAction SilentlyContinue)) { Fail "ollama is not on PATH" }
    Write-Host "starting ollama serve"
    Start-Process ollama -ArgumentList serve -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds(60)
    while (-not (Test-Ollama)) {
        if ((Get-Date) -gt $deadline) { Fail "Ollama did not answer at $server within 60 s" }
        Start-Sleep -Milliseconds 500
    }
}

$pulled = @((Invoke-RestMethod "$server/api/tags").models.name)
$missing = @($models | Where-Object { $pulled -notcontains $_.Name -and $pulled -notcontains "$($_.Name):latest" })
if ($missing) {
    Fail ("not pulled: " + ($missing.Name -join ', ') + "`n" + (($missing | ForEach-Object { "  ollama pull $($_.Name)" }) -join "`n"))
}

foreach ($m in $models) {
    Write-Host "loading $($m.Name)"
    $body = @{ model = $m.Name; keep_alive = $KeepAlive; options = $m.Options }
    # An empty chat only loads the model; /api/embed needs some input.
    if ($m.Api -eq 'chat') { $body.messages = @() } else { $body.input = @('warm-up') }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        Invoke-RestMethod -Method Post "$server/api/$($m.Api)" -Body ($body | ConvertTo-Json -Depth 5) `
            -ContentType 'application/json' -TimeoutSec 600 | Out-Null
    } catch {
        Fail "could not load $($m.Name): $($_.Exception.Message)"
    }
    Write-Host ("  ready in {0:N1} s" -f $timer.Elapsed.TotalSeconds)
}

Write-Host "`nloaded in Ollama ($server):"
foreach ($p in (Invoke-RestMethod "$server/api/ps").models) {
    $gpu = if ($p.size) { [math]::Round(100 * $p.size_vram / $p.size) } else { 0 }
    Write-Host ("  {0,-18} ctx {1,-5} {2:N1} GB, {3}% on GPU, until {4:HH:mm}" -f
        $p.name, $p.context_length, ($p.size / 1GB), $gpu, [datetime]$p.expires_at)
}
