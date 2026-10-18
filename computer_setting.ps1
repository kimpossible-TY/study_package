
# ==============================================================================
# Windows user-level development environment bootstrapper
# ==============================================================================

$ErrorActionPreference = "Stop"

$ScoopPackages = @(
    "7zip",
    "curl",
    "fd",
    "fzf",
    "gcc",
    "gh",
    "make",
    "neovim",
    "nodejs-lts",
    "oh-my-posh",
    "python",
    "ripgrep",
    "unzip"
)

$ExtraPackages = @(
    "powertoys"
)

function Test-Command {
    param([Parameter(Mandatory = $true)][string]$Name)

    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Update-ProcessPath {
    $PathParts = @(
        [Environment]::GetEnvironmentVariable("Path", "Machine"),
        [Environment]::GetEnvironmentVariable("Path", "User"),
        (Join-Path $env:USERPROFILE "scoop\shims"),
        (Join-Path $env:LOCALAPPDATA "Programs\devtunnel")
    )

    $env:Path = ($PathParts | Where-Object { $_ }) -join [IO.Path]::PathSeparator
}

function Test-GitWorkTreeClean {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (!(Test-Path -LiteralPath (Join-Path $Path ".git"))) {
        return $true
    }

    $Status = git -C $Path status --porcelain 2>$null
    return [string]::IsNullOrWhiteSpace(($Status -join ""))
}

function Test-ScoopBucketsClean {
    $BucketsDir = Join-Path $env:USERPROFILE "scoop\buckets"
    if (!(Test-Path -LiteralPath $BucketsDir)) {
        return $true
    }

    $DirtyBuckets = @(
        Get-ChildItem -LiteralPath $BucketsDir -Directory |
            Where-Object { !(Test-GitWorkTreeClean -Path $_.FullName) } |
            Select-Object -ExpandProperty Name
    )

    if ($DirtyBuckets.Count -gt 0) {
        Write-Host "Skipping Scoop update because bucket repo has local changes: $($DirtyBuckets -join ', ')" -ForegroundColor Yellow
        return $false
    }

    return $true
}

function Set-VSCodeGitPath {
    $GitPath = Join-Path $env:USERPROFILE "scoop\apps\git\current\cmd\git.exe"
    if (!(Test-Path -LiteralPath $GitPath)) {
        Write-Host "Git executable was not found for VS Code configuration: $GitPath" -ForegroundColor Yellow
        return
    }

    $VSCodeUserDir = Join-Path $env:APPDATA "Code\User"
    $SettingsPath = Join-Path $VSCodeUserDir "settings.json"

    New-Item -ItemType Directory -Path $VSCodeUserDir -Force | Out-Null

    if (Test-Path -LiteralPath $SettingsPath) {
        $SettingsText = [IO.File]::ReadAllText($SettingsPath, [Text.Encoding]::UTF8)
        if ([string]::IsNullOrWhiteSpace($SettingsText)) {
            $Settings = [pscustomobject]@{}
        }
        else {
            $Settings = $SettingsText | ConvertFrom-Json
        }
    }
    else {
        $Settings = [pscustomobject]@{}
    }

    $Settings | Add-Member -NotePropertyName "git.path" -NotePropertyValue $GitPath -Force
    $Settings | Add-Member -NotePropertyName "git.ignoreMissingGitWarning" -NotePropertyValue $false -Force

    $Json = $Settings | ConvertTo-Json -Depth 20
    $Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($SettingsPath, $Json + [Environment]::NewLine, $Utf8NoBom)

    Write-Host "VS Code Git path configured: $GitPath" -ForegroundColor DarkGray
}

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)][string]$Message,
        [Parameter(Mandatory = $true)][scriptblock]$ScriptBlock
    )

    Write-Host "`n$Message" -ForegroundColor Cyan
    & $ScriptBlock
}

function Install-ScoopPackage {
    param([Parameter(Mandatory = $true)][string]$Name)

    $null = scoop prefix $Name 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Already installed: $Name" -ForegroundColor DarkGray
        return
    }

    Write-Host "Installing: $Name" -ForegroundColor Cyan
    scoop install $Name
}

function Install-DevTunnel {
    $InstallDir = Join-Path $env:LOCALAPPDATA "Programs\devtunnel"
    $Executable = Join-Path $InstallDir "devtunnel.exe"
    if ((Test-Command "devtunnel") -and !(Test-Path -LiteralPath $Executable -PathType Leaf)) {
        Write-Host "Dev Tunnel CLI is already available." -ForegroundColor DarkGray
        return
    }

    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null

    if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) {
        $DownloadPath = Join-Path $InstallDir ("devtunnel-{0}.exe" -f [Guid]::NewGuid().ToString("N"))
        try {
            Invoke-WebRequest `
                -Uri "https://aka.ms/TunnelsCliDownload/win-x64" `
                -UseBasicParsing `
                -OutFile $DownloadPath
            & $DownloadPath --version
            if ($LASTEXITCODE -ne 0) {
                throw "The downloaded Dev Tunnel CLI could not be started."
            }
            Move-Item -LiteralPath $DownloadPath -Destination $Executable -Force
        }
        finally {
            if (Test-Path -LiteralPath $DownloadPath) {
                Remove-Item -LiteralPath $DownloadPath -Force
            }
        }
    }

    $UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $UserPathParts = @($UserPath -split [IO.Path]::PathSeparator | Where-Object { $_ })
    if ($UserPathParts -notcontains $InstallDir) {
        $NewUserPath = (@($UserPathParts) + $InstallDir) -join [IO.Path]::PathSeparator
        [Environment]::SetEnvironmentVariable("Path", $NewUserPath, "User")
    }
    Update-ProcessPath

    Write-Host "Dev Tunnel CLI installed: $Executable" -ForegroundColor DarkGray
    Write-Host "To sign in using your own phone/browser: devtunnel user login -g -d" -ForegroundColor DarkGray
}

Write-Host "`nPreparing personal Windows development environment..." -ForegroundColor Cyan

Update-ProcessPath

if (![Environment]::Is64BitOperatingSystem) {
    throw "This script requires 64-bit Windows because PowerToys is installed for 64-bit Windows."
}

Invoke-Step "Allowing local user PowerShell scripts..." {
    try {
        Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser -Force -ErrorAction Stop
    }
    catch {
        Write-Host "Execution policy is controlled by a more specific Windows policy. Continuing with the current policy." -ForegroundColor Yellow
    }
}

if (!(Test-Command "scoop")) {
    Invoke-Step "Installing Scoop for the current user..." {
        Invoke-RestMethod -Uri "https://get.scoop.sh" | Invoke-Expression
        Update-ProcessPath
    }
}
else {
    Write-Host "Scoop is already installed." -ForegroundColor DarkGray
}

Invoke-Step "Installing Git for Scoop..." {
    Install-ScoopPackage "git"
}

Invoke-Step "Configuring VS Code Git path..." {
    Set-VSCodeGitPath
}

Invoke-Step "Updating Scoop..." {
    if (Test-ScoopBucketsClean) {
        scoop update
    }
}

Invoke-Step "Installing base Scoop packages..." {
    foreach ($Package in $ScoopPackages) {
        Install-ScoopPackage $Package
    }
}

Invoke-Step "Installing Microsoft Dev Tunnel CLI for the current user..." {
    Install-DevTunnel
}

Invoke-Step "Adding Scoop extras bucket..." {
    $ExtrasBucketDir = Join-Path $env:USERPROFILE "scoop\buckets\extras"
    if (!(Test-Path -LiteralPath $ExtrasBucketDir)) {
        scoop bucket add extras
    }
    else {
        Write-Host "Bucket already added: extras" -ForegroundColor DarkGray
    }
}

Invoke-Step "Installing 64-bit Windows PowerToys..." {
    foreach ($Package in $ExtraPackages) {
        Install-ScoopPackage $Package
    }
}

Invoke-Step "Preparing Neovim configuration folder..." {
    $NvimConfigDir = Join-Path $env:LOCALAPPDATA "nvim"
    New-Item -ItemType Directory -Path $NvimConfigDir -Force | Out-Null

    $InitLua = Join-Path $NvimConfigDir "init.lua"
    if (!(Test-Path -LiteralPath $InitLua)) {
        Invoke-RestMethod `
            -Uri "https://raw.githubusercontent.com/kimpossible-TY/dotfiles/main/nvim/.config/nvim/init.lua" `
            -OutFile $InitLua
    }
    else {
        Write-Host "Neovim config already exists: $InitLua" -ForegroundColor DarkGray
    }
}

Invoke-Step "Installing Windows Local Auth Cleaner to Downloads via Scoop..." {
    $Manifest = Join-Path $PSScriptRoot "windows_local_auth_cleaner\windows-local-auth-cleaner.json"
    $DownloadsDir = Join-Path $env:USERPROFILE "Downloads"
    if (!(Test-Path -LiteralPath $DownloadsDir)) {
        New-Item -ItemType Directory -Path $DownloadsDir -Force | Out-Null
    }
    $DestinationExe = Join-Path $DownloadsDir "windows-local-auth-cleaner.exe"

    if (Test-Command "scoop") {
        if (Test-Path -LiteralPath $Manifest) {
            scoop install $Manifest
        }
        else {
            scoop install "https://raw.githubusercontent.com/kimpossible-TY/study_package/main/windows_local_auth_cleaner/windows-local-auth-cleaner.json"
        }

        # Ensure the binary is placed directly in the Downloads directory
        $InstalledExe = Join-Path $env:USERPROFILE "scoop\apps\windows-local-auth-cleaner\current\windows-local-auth-cleaner.exe"
        if (Test-Path -LiteralPath $InstalledExe) {
            Copy-Item -LiteralPath $InstalledExe -Destination $DestinationExe -Force
            Write-Host "Binary placed in Downloads directory: $DestinationExe" -ForegroundColor DarkGray
        }
    }
    else {
        Write-Host "Downloading binary directly to Downloads directory..." -ForegroundColor DarkGray
        Invoke-RestMethod `
            -Uri "https://github.com/kimpossible-TY/study_package/releases/latest/download/windows-local-auth-cleaner.exe" `
            -OutFile $DestinationExe
        Write-Host "Binary downloaded to Downloads directory: $DestinationExe" -ForegroundColor DarkGray
    }
}

Write-Host "`nPersonal Windows development environment configured successfully." -ForegroundColor Green
Write-Host "Installed with Scoop in the current user account." -ForegroundColor Green
Write-Host "PowerToys was installed from the Scoop extras bucket for 64-bit Windows." -ForegroundColor Green
