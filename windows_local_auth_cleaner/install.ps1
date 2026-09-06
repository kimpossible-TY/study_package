#Requires -Version 5.1
<#
.SYNOPSIS
    Installs windows-local-auth-cleaner standalone executable from GitHub Releases.

.DESCRIPTION
    Downloads the standalone binary windows-local-auth-cleaner.exe from the official
    GitHub release repository (kimpossible-TY/study_package), places it in a user-level
    directory, creates the 'wac' short alias shim, and configures the user PATH so it
    can be executed from any terminal in one shot without requiring Python.

.PARAMETER Tag
    The specific GitHub release tag to install (e.g. 'v0.1.0' or 'latest'). Defaults to 'latest'.

.PARAMETER InstallDir
    Target directory for the executable. Defaults to $env:LOCALAPPDATA\Programs\windows-local-auth-cleaner.

.PARAMETER Uninstall
    If specified, removes the installed binary, shims, and user PATH entry.

.EXAMPLE
    # One-line install via PowerShell:
    irm https://raw.githubusercontent.com/kimpossible-TY/study_package/main/windows_local_auth_cleaner/install.ps1 | iex

.EXAMPLE
    # Install specific version:
    .\install.ps1 -Tag "v0.1.0"

.EXAMPLE
    # Uninstall:
    .\install.ps1 -Uninstall
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Tag = "latest",

    [Parameter()]
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Programs\windows-local-auth-cleaner"),

    [Parameter()]
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

$Repo = "kimpossible-TY/study_package"
$BinaryName = "windows-local-auth-cleaner.exe"
$AliasName = "wac.cmd"

function Update-SessionPath {
    param([string]$Dir)
    if ($env:Path -notlike "*$Dir*") {
        $env:Path = "$Dir;" + $env:Path
    }
}

if ($Uninstall) {
    Write-Host "`nUninstalling Windows Local Auth Cleaner..." -ForegroundColor Yellow

    if (Test-Path -LiteralPath $InstallDir) {
        Remove-Item -LiteralPath $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "Removed installation directory: $InstallDir" -ForegroundColor DarkGray
    }

    # Remove from User PATH
    $UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($UserPath) {
        $PathEntries = $UserPath -split [IO.Path]::PathSeparator | Where-Object { $_ -and $_ -ne $InstallDir }
        [Environment]::SetEnvironmentVariable("Path", ($PathEntries -join [IO.Path]::PathSeparator), "User")
        Write-Host "Removed $InstallDir from User PATH." -ForegroundColor DarkGray
    }

    # Remove optional scoop shim if present
    $ScoopShimDir = Join-Path $env:USERPROFILE "scoop\shims"
    if (Test-Path -LiteralPath $ScoopShimDir) {
        Remove-Item (Join-Path $ScoopShimDir $BinaryName) -Force -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $ScoopShimDir $AliasName) -Force -ErrorAction SilentlyContinue
    }

    Write-Host "✓ Windows Local Auth Cleaner uninstalled successfully.`n" -ForegroundColor Green
    return
}

Write-Host "`nInstalling Windows Local Auth Cleaner from GitHub Release ($Repo)..." -ForegroundColor Cyan

# Determine download URL
if ($Tag -eq "latest") {
    $DownloadUrl = "https://github.com/$Repo/releases/latest/download/$BinaryName"
} else {
    $DownloadUrl = "https://github.com/$Repo/releases/download/$Tag/$BinaryName"
}

# Ensure destination directory exists
if (!(Test-Path -LiteralPath $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}

$ExePath = Join-Path $InstallDir $BinaryName
$AliasPath = Join-Path $InstallDir $AliasName

Write-Host "Downloading $BinaryName from $DownloadUrl..." -ForegroundColor DarkGray
try {
    # Use TLS 1.2+
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $DownloadUrl -OutFile $ExePath -UseBasicParsing
} catch {
    Write-Error "Failed to download $BinaryName from $DownloadUrl. Ensure the release exists and network is available.`nError: $_"
    return
}

# Create 'wac.cmd' short alias wrapper in the same directory
$CmdContent = "@echo off`r`n`"%~dp0$BinaryName`" %*`r`n"
[IO.File]::WriteAllText($AliasPath, $CmdContent, [Text.Encoding]::ASCII)

# Add to User PATH if not already present
$UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
$PathEntries = if ($UserPath) { $UserPath -split [IO.Path]::PathSeparator | Where-Object { $_ } } else { @() }
if ($InstallDir -notin $PathEntries) {
    $NewUserPath = ($PathEntries + $InstallDir) -join [IO.Path]::PathSeparator
    [Environment]::SetEnvironmentVariable("Path", $NewUserPath, "User")
    Write-Host "Added $InstallDir to User PATH." -ForegroundColor DarkGray
}

Update-SessionPath $InstallDir

# If Scoop shims exist, also link into scoop shims for instant availability in scoop-managed terminals
$ScoopShimDir = Join-Path $env:USERPROFILE "scoop\shims"
if (Test-Path -LiteralPath $ScoopShimDir) {
    try {
        Copy-Item -LiteralPath $ExePath -Destination (Join-Path $ScoopShimDir $BinaryName) -Force
        Copy-Item -LiteralPath $AliasPath -Destination (Join-Path $ScoopShimDir $AliasName) -Force
        Write-Host "Also linked shims to Scoop: $ScoopShimDir" -ForegroundColor DarkGray
    } catch {
        # Optional best-effort
    }
}

Write-Host "`n✓ Windows Local Auth Cleaner installed successfully!" -ForegroundColor Green
Write-Host "Location: $ExePath" -ForegroundColor DarkGray
Write-Host "Commands available in PATH:" -ForegroundColor Green
Write-Host "  • windows-local-auth-cleaner   (Runs one-shot cleanup immediately)" -ForegroundColor Cyan
Write-Host "  • wac                          (Short alias for one-shot cleanup)" -ForegroundColor Cyan
Write-Host "  • wac --dry-run                (Preview targets without deleting)" -ForegroundColor Cyan
Write-Host "  • wac --interactive            (Launch interactive Textual TUI)`n" -ForegroundColor Cyan
