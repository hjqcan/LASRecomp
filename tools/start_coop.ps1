param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$Game,
    [Parameter(Mandatory=$true)][string]$Library,
    [Parameter(Mandatory=$true)][string]$SaveDirectory,
    [Parameter(Mandatory=$true)][string]$DataRoot
)
$ErrorActionPreference = 'Stop'
$coopExe = (Resolve-Path -LiteralPath $Exe).Path
$coopGame = (Resolve-Path -LiteralPath $Game).Path
$coopLibrary = (Resolve-Path -LiteralPath $Library).Path
$coopSource = (Resolve-Path -LiteralPath $SaveDirectory).Path
if (!(Test-Path -LiteralPath (Join-Path $coopGame 'exefs/main') -PathType Leaf)) {
    throw 'Choose an extracted game folder with exefs/main.'
}
$coopData = [System.IO.Path]::GetFullPath($DataRoot)
[System.IO.Directory]::CreateDirectory($coopData) | Out-Null
$coopPreset = Join-Path $coopData 'local-coop.laschallenge'
[System.IO.File]::WriteAllText($coopPreset, "format=1`nname=local-coop`ngame_version=1.0.0`ngoal_level=Lv09WindFishsEgg`nseconds=3600`ndamage=1`nno_hit=0`nohko=0`n")
$coopSettings = Join-Path $coopData 'settings.ini'
if (!(Test-Path -LiteralPath $coopSettings)) {
    [System.IO.File]::WriteAllText($coopSettings, "ui_language=zh-Hans`nlanguage=zh-Hans`nrando_seed=`nfullscreen=0`n")
}
$coopStart = New-Object System.Diagnostics.ProcessStartInfo
$coopStart.FileName = $coopExe
$coopStart.WorkingDirectory = Split-Path -LiteralPath $coopExe
$coopStart.Arguments = '"' + $coopGame + '" --play'
$coopStart.UseShellExecute = $false
$coopStart.CreateNoWindow = $true
# Keep test drivers and save overrides from a parent shell out of this interactive session.
foreach ($coopKey in @($coopStart.EnvironmentVariables.Keys)) {
    if ($coopKey.StartsWith('LARECOMP_')) { $coopStart.EnvironmentVariables.Remove($coopKey) }
}
$coopStart.EnvironmentVariables['LARECOMP_COOP'] = '1'
$coopStart.EnvironmentVariables['LARECOMP_DATA_ROOT'] = $coopData
$coopStart.EnvironmentVariables['LARECOMP_SETTINGS'] = $coopSettings
$coopStart.EnvironmentVariables['LARECOMP_GAME_LIB'] = $coopLibrary
$coopStart.EnvironmentVariables['LARECOMP_SAVE_DIR'] = $coopSource
$coopStart.EnvironmentVariables['LARECOMP_CHALLENGE_PRESET'] = $coopPreset
$coopStart.EnvironmentVariables['LARECOMP_NO_UPDATE_CHECK'] = '1'
$coopStart.RedirectStandardOutput = $true
$coopStart.RedirectStandardError = $true
$coopProcess = [System.Diagnostics.Process]::Start($coopStart)
$coopOutput = $coopProcess.StandardOutput.ReadToEndAsync()
$coopErrors = $coopProcess.StandardError.ReadToEndAsync()
$coopProcess.WaitForExit()
[System.IO.File]::WriteAllText((Join-Path $coopData 'runtime.log'), $coopOutput.Result + $coopErrors.Result)
if ($coopProcess.ExitCode -ne 0) { throw "Co-op exited with code $($coopProcess.ExitCode). See $coopData\runtime.log" }
