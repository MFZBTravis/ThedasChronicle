# Builds build\Release\ThedasChronicleHook.asi
# Usage: .\build.ps1 [-Config Release|Debug] [-GameDir "C:\Program Files\EA Games\Dragon Age Inquisition"]
param(
    [string]$Config = "Release",
    [string]$GameDir = ""
)

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}

$configureArgs = @("-S", $PSScriptRoot, "-B", "$PSScriptRoot\build", "-A", "x64")
if ($GameDir) { $configureArgs += "-DDAI_DIR=$($GameDir -replace '\\','/')" }

& $cmake @configureArgs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build "$PSScriptRoot\build" --config $Config
exit $LASTEXITCODE
