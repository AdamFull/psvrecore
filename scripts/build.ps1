# Configure, build and (optionally) test a preset without opening a developer prompt.
#
#   scripts\build.ps1                                  # windows-x64-debug, build + tests
#   scripts\build.ps1 -Preset windows-x64-release -NoTest
#   scripts\build.ps1 -Preset android-arm64-release    # ANDROID_NDK_HOME, or the newest NDK in the Android SDK
#   scripts\build.ps1 -Define PSVRECORE_FUZZ_BLOCKS=20000  # cache variables for the configure step
#
# -Root builds another project with the same presets: a game repository calls this script with its own root.
param(
    [string]$Preset = 'windows-x64-debug',
    [switch]$NoTest,
    [string[]]$Target,
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string[]]$Define
)
$ErrorActionPreference = 'Stop'
$root = $Root
# (from powershell -File, a list arrives as one comma-separated string)
$Target = @($Target | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ x64 tools was not found.' }

if ($Preset -like 'windows-*') {
    # Import the x64 developer environment (Windows SDK, CRT) into this PowerShell session.
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    $env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars itself calls vswhere
    cmd /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}
if ($Preset -like 'android-*' -and -not $env:ANDROID_NDK_HOME) {
    $sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { 'C:\Android\Sdk' }
    $ndk = Get-ChildItem (Join-Path $sdk 'ndk') -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } | Select-Object -Last 1
    if (-not $ndk) { throw 'No Android NDK found: set ANDROID_NDK_HOME.' }
    $env:ANDROID_NDK_HOME = $ndk.FullName
}

# Prefer standalone LLVM and VS-bundled CMake/Ninja when they are not already on PATH.
$extra = @(
    "$env:ProgramFiles\LLVM\bin",
    "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin",
    "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
) | Where-Object { Test-Path $_ }
$env:PATH = ($extra + $env:PATH) -join ';'

# Native tools report warnings on stderr; under 'Stop' PowerShell 5.1 would abort on them. Exit codes are checked.
$ErrorActionPreference = 'Continue'
Push-Location $root
try {
    $defines = @($Define | Where-Object { $_ } | ForEach-Object { "-D$_" })
    cmake --preset $Preset @defines
    if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    if ($Target) { cmake --build --preset $Preset --target @Target }
    else { cmake --build --preset $Preset }
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
    if (-not $NoTest -and -not $Target -and $Preset -notlike 'android-*') {
        ctest --preset $Preset
        if ($LASTEXITCODE) { throw "tests failed ($LASTEXITCODE)" }
    }
} finally {
    Pop-Location
}
