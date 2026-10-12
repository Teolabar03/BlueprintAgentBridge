# Starts the headless bridge server for a project.
# Usage: scripts\serve.ps1 [-Project path\to\Game.uproject] [-Port 8765] [-Engine "..."]
param(
    [string]$Project = "",
    [int]$Port = 8765,
    [string]$Engine = ""
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"
if ($Project -eq "") { $Project = "$Root\test-project\BPBridgeTest.uproject" }
$Engine = Resolve-EngineDir $Engine $Project
$Cmd = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
Write-Host "Serving $Project on http://127.0.0.1:$Port (POST /rpc, GET /health, POST /shutdown)"
& $Cmd $Project -run=BlueprintAgentBridge -serve -port=$Port -unattended -nosplash -nullrhi -NoLogTimes -stdout -FullStdOutLogOutput
