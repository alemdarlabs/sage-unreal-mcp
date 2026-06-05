[CmdletBinding()]
param(
  [ValidateSet('x86', 'x64', 'arm', 'arm64')]
  [string]$Architecture = 'x64',

  [ValidateSet('x86', 'x64', 'arm', 'arm64')]
  [string]$HostArchitecture = 'x64',

  [string]$GitHubEnvPath = $env:GITHUB_ENV,

  [string]$GitHubPathPath = $env:GITHUB_PATH
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

function Get-SpecialFolderPath {
  param(
    [Parameter(Mandatory = $true)]
    [System.Environment+SpecialFolder]$Folder
  )

  return [Environment]::GetFolderPath($Folder)
}

function Resolve-VsDevCmd {
  $ProgramFilesX86 = Get-SpecialFolderPath -Folder ([System.Environment+SpecialFolder]::ProgramFilesX86)
  if (-not $ProgramFilesX86) {
    $ProgramFilesX86 = ${env:ProgramFiles(x86)}
  }

  if ($ProgramFilesX86) {
    $VsWhere = Join-Path $ProgramFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $VsWhere) {
      $VsWhereOutput = & $VsWhere `
        -latest `
        -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath

      $VsWhereExitCode = Get-NativeExitCode
      if ($VsWhereExitCode -ne 0) {
        throw "vswhere.exe failed with exit code $VsWhereExitCode."
      }

      $VsInstall = $VsWhereOutput | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -First 1
      if ($VsInstall) {
        $VsDevCmd = Join-Path $VsInstall.Trim() 'Common7\Tools\VsDevCmd.bat'
        if (Test-Path -LiteralPath $VsDevCmd) {
          return $VsDevCmd
        }

        throw "VsDevCmd.bat was not found at '$VsDevCmd'."
      }
    }
  }

  $ProgramFiles = Get-SpecialFolderPath -Folder ([System.Environment+SpecialFolder]::ProgramFiles)
  $Fallbacks = @(
    Join-Path $ProgramFiles 'Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat',
    Join-Path $ProgramFiles 'Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat',
    Join-Path $ProgramFiles 'Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat',
    Join-Path $ProgramFiles 'Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
  )

  foreach ($Candidate in $Fallbacks) {
    if (Test-Path -LiteralPath $Candidate) {
      return $Candidate
    }
  }

  throw 'Visual Studio with VC++ x64 tools was not found.'
}

function Get-NativeExitCode {
  $Variable = Get-Variable -Name LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue
  if (-not $Variable -or $null -eq $Variable.Value) {
    return 0
  }

  return [int]$Variable.Value
}

function Add-GitHubEnvValue {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [Parameter(Mandatory = $true)]
    [string]$Value
  )

  if ([string]::IsNullOrWhiteSpace($Value)) {
    return
  }

  if (-not $GitHubEnvPath) {
    Set-Item -Path "Env:$Name" -Value $Value
    return
  }

  $Delimiter = "SAGE_ENV_$([Guid]::NewGuid().ToString('N'))"
  Add-Content -LiteralPath $GitHubEnvPath -Value "$Name<<$Delimiter"
  Add-Content -LiteralPath $GitHubEnvPath -Value $Value
  Add-Content -LiteralPath $GitHubEnvPath -Value $Delimiter
}

function Add-GitHubPathEntry {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Entry
  )

  if ([string]::IsNullOrWhiteSpace($Entry)) {
    return
  }

  if (-not $GitHubPathPath) {
    $env:Path = "$Entry;$env:Path"
    return
  }

  Add-Content -LiteralPath $GitHubPathPath -Value $Entry
}

$VsDevCmd = Resolve-VsDevCmd
$Output = & cmd.exe /s /c "`"$VsDevCmd`" -arch=$Architecture -host_arch=$HostArchitecture && set"
$CmdExitCode = Get-NativeExitCode
if ($CmdExitCode -ne 0) {
  exit $CmdExitCode
}

$EnvValues = @{}
foreach ($Line in $Output) {
  $Index = $Line.IndexOf('=')
  if ($Index -le 0) {
    continue
  }

  $Name = $Line.Substring(0, $Index)
  $Value = $Line.Substring($Index + 1)
  $EnvValues[$Name.ToUpperInvariant()] = @{
    Name = $Name
    Value = $Value
  }
}

foreach ($RequiredName in @('PATH', 'VCTOOLSINSTALLDIR', 'WINDOWSSDKDIR')) {
  if (-not $EnvValues.ContainsKey($RequiredName)) {
    throw "VsDevCmd.bat did not produce required environment variable '$RequiredName'."
  }
}

foreach ($PathEntry in ($EnvValues['PATH'].Value -split ';')) {
  Add-GitHubPathEntry -Entry $PathEntry
}

$AllowedEnvironmentVariables = @(
  'CL',
  'DevEnvDir',
  'EXTERNAL_INCLUDE',
  'Framework40Version',
  'FrameworkDir',
  'FrameworkDir64',
  'FrameworkVersion',
  'FrameworkVersion64',
  'INCLUDE',
  'LIB',
  'LIBPATH',
  'NETFXSDKDir',
  'UCRTVersion',
  'UniversalCRTSdkDir',
  'VCIDEInstallDir',
  'VCINSTALLDIR',
  'VCPKG_VISUAL_STUDIO_PATH',
  'VCToolsInstallDir',
  'VCToolsRedistDir',
  'VCToolsVersion',
  'VisualStudioVersion',
  'VSINSTALLDIR',
  'WindowsLibPath',
  'WindowsSdkBinPath',
  'WindowsSdkDir',
  'WindowsSDKLibVersion',
  'WindowsSdkVerBinPath',
  'WindowsSDKVersion',
  '__DOTNET_ADD_64BIT',
  '__DOTNET_PREFERRED_BITNESS'
)

foreach ($Name in $AllowedEnvironmentVariables) {
  $Key = $Name.ToUpperInvariant()
  if ($EnvValues.ContainsKey($Key)) {
    Add-GitHubEnvValue -Name $EnvValues[$Key].Name -Value $EnvValues[$Key].Value
  }
}

$ToolsetVersion = 'unknown'
if ($EnvValues.ContainsKey('VCTOOLSVERSION')) {
  $ToolsetVersion = $EnvValues['VCTOOLSVERSION'].Value
}

Write-Host "MSVC environment ready. VCToolsVersion=$ToolsetVersion"
