# Helper: import VS 2022 vcvars64 env into the current PowerShell session.
# Use:   . .\scripts\dev-shell.ps1
# (dot-source so env changes persist in the caller).
#
# ASCII-only on purpose: PS 5.1 reads scripts as the OEM/ANSI codepage by
# default, and non-ASCII characters in source can crash the parser.

$ErrorActionPreference = 'Stop'

$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $VsWhere)) {
    throw "vswhere.exe not found at $VsWhere. Install VS 2022 (Community or BuildTools)."
}
$VsPath = & $VsWhere -latest -property installationPath
if (-not $VsPath) { throw "VS 2022 installation not detected by vswhere." }
$VcVars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $VcVars)) { throw "vcvars64.bat not found at $VcVars" }

$cmdLine = '"' + $VcVars + '" >NUL & set'
cmd /c $cmdLine | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
    }
}

# vcvars64 sets VCPKG_ROOT to the VS-bundled vcpkg; we want our manifest-mode user vcpkg.
$env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
if (-not $env:SAGE_UE_ROOT) { $env:SAGE_UE_ROOT = 'C:\Program Files\Epic Games\UE_5.7' }

$CMakeBin = 'C:\Program Files\CMake\bin'
$NinjaDir = $null
$NinjaProbe = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter 'ninja.exe' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if ($NinjaProbe) { $NinjaDir = $NinjaProbe.DirectoryName }
foreach ($p in @($CMakeBin, $NinjaDir)) {
    if ($p -and (Test-Path $p) -and ($env:Path -notlike "*$p*")) {
        $env:Path = "$p;$env:Path"
    }
}

Write-Host 'Sage dev shell ready:'
$cl    = (Get-Command cl.exe    -ErrorAction SilentlyContinue).Path
$cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Path
$ninja = (Get-Command ninja.exe -ErrorAction SilentlyContinue).Path
Write-Host "  cl.exe : $cl"
Write-Host "  cmake  : $cmake"
Write-Host "  ninja  : $ninja"
Write-Host "  vcpkg  : $env:VCPKG_ROOT"
Write-Host "  UE     : $env:SAGE_UE_ROOT"
