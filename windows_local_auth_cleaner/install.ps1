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
    The specific GitHub release tag to install (e.g. 'v0.2.0' or 'latest'). Defaults to 'latest'.

.PARAMETER InstallDir
    Target directory for the executable. Defaults to $env:LOCALAPPDATA\Programs\windows-local-auth-cleaner.

.PARAMETER Uninstall
    If specified, removes the installed binary, shims, and user PATH entry.

.EXAMPLE
    # One-line install via PowerShell:
    irm https://raw.githubusercontent.com/kimpossible-TY/study_package/main/windows_local_auth_cleaner/install.ps1 | iex

.EXAMPLE
    # Install specific version:
    .\install.ps1 -Tag "v0.2.0"

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
    $OtherEntries = @($env:Path -split [IO.Path]::PathSeparator | Where-Object { $_ -and $_ -ne $Dir })
    $env:Path = (@($Dir) + $OtherEntries) -join [IO.Path]::PathSeparator
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

    # Scoop-managed shims are owned by Scoop.

    Write-Host "✓ Windows Local Auth Cleaner uninstalled successfully.`n" -ForegroundColor Green
    return
}

Write-Host "`nInstalling Windows Local Auth Cleaner from GitHub Release ($Repo)..." -ForegroundColor Cyan

# Determine download URL
if ($Tag -eq "latest") {
    $ReleaseBase = "https://github.com/$Repo/releases/latest/download"
} else {
    $ReleaseBase = "https://github.com/$Repo/releases/download/$Tag"
}
$DownloadUrl = "$ReleaseBase/$BinaryName"

# Ensure destination directory exists
if (!(Test-Path -LiteralPath $InstallDir)) {
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
}

$ExePath = Join-Path $InstallDir $BinaryName
$AliasPath = Join-Path $InstallDir $AliasName

Write-Host "Downloading $BinaryName from $DownloadUrl..." -ForegroundColor DarkGray
$TemporaryExe = Join-Path $InstallDir ("download-{0}.exe" -f [Guid]::NewGuid().ToString('N'))
$TemporaryChecksum = "$TemporaryExe.sha256"
try {
    # Use TLS 1.2+
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $DownloadUrl -OutFile $TemporaryExe -UseBasicParsing
    Invoke-WebRequest -Uri "$DownloadUrl.sha256" -OutFile $TemporaryChecksum -UseBasicParsing
    $ChecksumText = Get-Content -LiteralPath $TemporaryChecksum -Raw
    if ($ChecksumText -notmatch '^\s*([A-Fa-f0-9]{64})\s+\*?windows-local-auth-cleaner\.exe\s*$') {
        throw 'The release checksum file has an invalid format.'
    }
    $ExpectedHash = $Matches[1]
    if ((Get-FileHash -LiteralPath $TemporaryExe -Algorithm SHA256).Hash -ne $ExpectedHash) {
        throw 'Release SHA-256 verification failed. The existing executable was not replaced.'
    }
    Move-Item -LiteralPath $TemporaryExe -Destination $ExePath -Force
}
finally {
    if (Test-Path -LiteralPath $TemporaryExe) {
        Remove-Item -LiteralPath $TemporaryExe -Force
    }
    if (Test-Path -LiteralPath $TemporaryChecksum) {
        Remove-Item -LiteralPath $TemporaryChecksum -Force
    }
}

# Create 'wac.cmd' short alias wrapper in the same directory
$CmdContent = "@echo off`r`n`"%~dp0$BinaryName`" %*`r`n"
[IO.File]::WriteAllText($AliasPath, $CmdContent, [Text.Encoding]::ASCII)

# Add to User PATH if not already present
$UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
$PathEntries = @($UserPath -split [IO.Path]::PathSeparator | Where-Object { $_ -and $_ -ne $InstallDir })
$NewUserPath = (@($InstallDir) + $PathEntries) -join [IO.Path]::PathSeparator
[Environment]::SetEnvironmentVariable("Path", $NewUserPath, "User")
Write-Host "Placed $InstallDir first in User PATH." -ForegroundColor DarkGray

Update-SessionPath $InstallDir

# Keep a convenient copy in Downloads. PATH takes precedence over old Scoop shims.
$DownloadsDir = Join-Path $env:USERPROFILE 'Downloads'
New-Item -ItemType Directory -Path $DownloadsDir -Force | Out-Null
Copy-Item -LiteralPath $ExePath -Destination (Join-Path $DownloadsDir $BinaryName) -Force

Write-Host "`n✓ Windows Local Auth Cleaner installed successfully!" -ForegroundColor Green
Write-Host "Location: $ExePath" -ForegroundColor DarkGray
Write-Host "Commands available in PATH:" -ForegroundColor Green
Write-Host "  • windows-local-auth-cleaner   (Runs one-shot cleanup immediately)" -ForegroundColor Cyan
Write-Host "  • wac                          (Short alias for one-shot cleanup)" -ForegroundColor Cyan
Write-Host "  • wac --dry-run                (Preview targets without deleting)" -ForegroundColor Cyan
Write-Host "  • wac --interactive            (Launch interactive Textual TUI)`n" -ForegroundColor Cyan
