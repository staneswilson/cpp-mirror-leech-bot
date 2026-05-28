<#
.SYNOPSIS
    Bootstraps a Windows development environment for CMLB.

.DESCRIPTION
    Installs Visual Studio 2022 build tools (if missing), CMake, Ninja, and clones
    vcpkg. Configures environment variables for the current PowerShell session.

    Requires running as an Administrator for system-wide installs. Falls back to
    user-scoped installs via winget where available.

.PARAMETER VcpkgRoot
    Path where vcpkg will be cloned. Defaults to %USERPROFILE%\vcpkg.

.EXAMPLE
    .\scripts\bootstrap_windows.ps1
#>
[CmdletBinding()]
param(
    [string]$VcpkgRoot = (Join-Path $env:USERPROFILE 'vcpkg')
)

$ErrorActionPreference = 'Stop'

function Write-Info($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Err($msg)  { Write-Host "err: $msg"   -ForegroundColor Red }

function Test-Command($name) {
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

function Install-WingetPackage($id) {
    if (-not (Test-Command winget)) {
        Write-Err "winget not available; install $id manually."
        return
    }
    Write-Info "Installing $id via winget"
    winget install --id=$id --silent --accept-package-agreements --accept-source-agreements
}

function Ensure-Tooling {
    if (-not (Test-Command cmake)) { Install-WingetPackage 'Kitware.CMake' }
    if (-not (Test-Command ninja))  { Install-WingetPackage 'Ninja-build.Ninja' }
    if (-not (Test-Command git))    { Install-WingetPackage 'Git.Git' }

    Write-Info 'Visual Studio 2022 Build Tools must be installed for MSVC.'
    Write-Info 'If missing: winget install --id Microsoft.VisualStudio.2022.BuildTools'
}

function Clone-Vcpkg {
    if (Test-Path (Join-Path $VcpkgRoot '.git')) {
        Write-Info "vcpkg already present at $VcpkgRoot"
        return
    }
    Write-Info "Cloning vcpkg into $VcpkgRoot"
    git clone --depth 1 https://github.com/microsoft/vcpkg.git $VcpkgRoot
    & (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
}

function Setup-PreCommit {
    if (-not (Test-Command pre-commit)) {
        Write-Info 'Installing pre-commit via pip'
        python -m pip install --user pre-commit
    }
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $hookCfg  = Join-Path $repoRoot '.pre-commit-config.yaml'
    if (Test-Path $hookCfg) {
        Write-Info 'Installing pre-commit hooks'
        Push-Location $repoRoot
        try { pre-commit install } finally { Pop-Location }
    }
}

function Print-NextSteps {
    Write-Host ''
    Write-Host 'Bootstrap complete.' -ForegroundColor Green
    Write-Host ''
    Write-Host 'Next steps for this session:'
    Write-Host "  `$env:VCPKG_ROOT = '$VcpkgRoot'"
    Write-Host '  cmake --preset msvc-debug'
    Write-Host '  cmake --build --preset msvc-debug'
    Write-Host '  ctest --preset msvc-debug --output-on-failure'
    Write-Host ''
}

Ensure-Tooling
Clone-Vcpkg
Setup-PreCommit
$env:VCPKG_ROOT = $VcpkgRoot
Print-NextSteps
