# Shared helpers for the scripts in this folder.
function Resolve-EngineDir {
    param([string]$Explicit, [string]$ProjectFile)
    if ($Explicit -ne "" -and (Test-Path (Join-Path $Explicit "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"))) { return $Explicit }
    if ($env:BAB_ENGINE_DIR -and (Test-Path (Join-Path $env:BAB_ENGINE_DIR "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"))) { return $env:BAB_ENGINE_DIR }
    $Association = "5.6"
    if ($ProjectFile -and (Test-Path $ProjectFile)) {
        $Json = Get-Content $ProjectFile -Raw | ConvertFrom-Json
        if ($Json.EngineAssociation) { $Association = $Json.EngineAssociation }
    }
    $Candidates = @()
    try {
        $Reg = Get-ItemProperty -Path "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association" -ErrorAction Stop
        if ($Reg.InstalledDirectory) { $Candidates += $Reg.InstalledDirectory }
    } catch {}
    $Candidates += "C:\Program Files\Epic Games\UE_$Association"
    foreach ($Candidate in $Candidates) {
        if (Test-Path (Join-Path $Candidate "Engine\Binaries\Win64\UnrealEditor-Cmd.exe")) { return $Candidate }
    }
    throw "Unreal Engine $Association not found. Pass -Engine or set BAB_ENGINE_DIR."
}
