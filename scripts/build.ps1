# Builds the test project editor target (which compiles the plugin).
# Usage: scripts\build.ps1 [-Engine "C:\Program Files\Epic Games\UE_5.6"] [-Configuration Development]
param(
    [string]$Engine = "",
    [string]$Configuration = "Development"
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"
$Engine = Resolve-EngineDir $Engine "$Root\test-project\BPBridgeTest.uproject"
$Build = Join-Path $Engine "Engine\Build\BatchFiles\Build.bat"
& $Build BPBridgeTestEditor Win64 $Configuration -Project="$Root\test-project\BPBridgeTest.uproject" -WaitMutex -NoHotReload
exit $LASTEXITCODE
