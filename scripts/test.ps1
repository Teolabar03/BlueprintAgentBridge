# Runs the plugin automation tests headless.
# Usage: scripts\test.ps1 [-Engine "..."] [-Filter BlueprintAgentBridge]
param(
    [string]$Engine = "",
    [string]$Filter = "BlueprintAgentBridge"
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"
$Engine = Resolve-EngineDir $Engine "$Root\test-project\BPBridgeTest.uproject"
$Cmd = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$Project = "$Root\test-project\BPBridgeTest.uproject"
& $Cmd $Project -ExecCmds="Automation RunTests $Filter;Quit" -TestExit="Automation Test Queue Empty" -unattended -nopause -nullrhi -nosplash -NoLogTimes -log -stdout -FullStdOutLogOutput 2>&1 |
    Select-String -Pattern "Test Started|Test Completed|LogAutomationController: Error|LogAutomationTest: Error|Assertion failed"
$LogDir = "$Root\test-project\Saved\Logs"
Write-Host "Full log: $LogDir\BPBridgeTest.log"
