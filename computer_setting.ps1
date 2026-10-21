
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

function Set-PersonalPowerShellProfile {
    # A managed block lets the cleaner remove our settings without erasing an
    # existing personal profile. Both editions use their own all-hosts profile.
    $ProfileBlock = @'
# BEGIN study_package PowerShell settings
function global:mac {
    ssh -o PubkeyAuthentication=no taeyoung@127.0.0.1 @args
}

if ($Host.Name -ne 'Windows PowerShell ISE Host') {
    try {
        Import-Module PSReadLine -ErrorAction Stop
        Set-PSReadLineOption -EditMode Windows -BellStyle None
        Set-PSReadLineKeyHandler -Key Tab -Function MenuComplete
        Set-PSReadLineKeyHandler -Key UpArrow -Function HistorySearchBackward
        Set-PSReadLineKeyHandler -Key DownArrow -Function HistorySearchForward
        $ReadLineOptions = Get-Command Set-PSReadLineOption
        if ($ReadLineOptions.Parameters.ContainsKey('PredictionSource')) {
            Set-PSReadLineOption -PredictionSource History
        }
        else {
            Write-Warning 'History predictions need a newer PSReadLine. Close PowerShell and open it again after setup.'
        }
        if ($ReadLineOptions.Parameters.ContainsKey('PredictionViewStyle')) {
            Set-PSReadLineOption -PredictionViewStyle InlineView
        }
    }
    catch {
        Write-Warning "PSReadLine setup failed: $_"
    }
    if (Get-Command oh-my-posh -ErrorAction SilentlyContinue) {
        try {
            oh-my-posh init pwsh | Invoke-Expression
        }
        catch {
            Write-Warning "oh-my-posh setup failed: $_"
        }
    }
}
# END study_package PowerShell settings
'@

    $DocumentsDir = [Environment]::GetFolderPath('MyDocuments')
    if ([string]::IsNullOrWhiteSpace($DocumentsDir)) {
        throw 'The current-user Documents folder could not be located.'
    }
    $ProfilePaths = @(
        $PROFILE.CurrentUserAllHosts
        (Join-Path $DocumentsDir 'WindowsPowerShell\profile.ps1')
        (Join-Path $DocumentsDir 'PowerShell\profile.ps1')
    ) | Select-Object -Unique
    $Pattern = '(?ms)^# BEGIN study_package PowerShell settings\r?\n.*?^# END study_package PowerShell settings(?:\r?\n|$)'
    foreach ($ProfilePath in $ProfilePaths) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $ProfilePath) -Force | Out-Null
        $Existing = ''
        if (Test-Path -LiteralPath $ProfilePath) {
            $Existing = [IO.File]::ReadAllText($ProfilePath)
        }
        if ($Existing.Contains('# BEGIN study_package PowerShell settings') -or $Existing.Contains('# END study_package PowerShell settings')) {
            if ([regex]::Matches($Existing, $Pattern).Count -ne 1 -or
                [regex]::Matches($Existing, [regex]::Escape('# BEGIN study_package PowerShell settings')).Count -ne 1 -or
                [regex]::Matches($Existing, [regex]::Escape('# END study_package PowerShell settings')).Count -ne 1) {
                throw "Invalid managed profile block: $ProfilePath"
            }
            $Existing = [regex]::Replace($Existing, $Pattern, '')
        }
        if ($Existing.Length -gt 0 -and !$Existing.EndsWith("`n")) {
            $Existing += [Environment]::NewLine
        }
        # BOM keeps existing non-ASCII content readable in Windows PowerShell 5.1.
        [IO.File]::WriteAllText($ProfilePath, $Existing + $ProfileBlock + [Environment]::NewLine, (New-Object Text.UTF8Encoding($true)))
        Write-Host "PowerShell profile configured: $ProfilePath" -ForegroundColor DarkGray
    }
    # Run in global scope even when setup is invoked via & or Invoke-Step.
    . ([scriptblock]::Create($ProfileBlock))
}

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

Invoke-Step "Installing PSReadLine history predictions..." {
    $InstallReadLine = {
        $ErrorActionPreference = 'Stop'
        $ReadLine = Get-Module PSReadLine -ListAvailable |
            Sort-Object Version -Descending | Select-Object -First 1
        if (!$ReadLine -or $ReadLine.Version -lt [version]'2.1.0') {
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            if (!(Get-PackageProvider NuGet -ListAvailable -ErrorAction SilentlyContinue)) {
                Install-PackageProvider NuGet -MinimumVersion 2.8.5.201 -Scope CurrentUser -Force | Out-Null
            }
            Install-Module PSReadLine -MinimumVersion 2.1.0 -Scope CurrentUser -Force -AllowClobber
        }
    }
    # Each edition has a distinct CurrentUser module directory. Separate clean
    # processes also avoid replacing PSReadLine binaries loaded by this shell.
    foreach ($Shell in @('powershell.exe', 'pwsh.exe')) {
        $ShellCommand = Get-Command $Shell -ErrorAction SilentlyContinue
        if ($ShellCommand) {
            & $ShellCommand.Source -NoProfile -NonInteractive -Command $InstallReadLine.ToString()
            if ($LASTEXITCODE -ne 0) {
                throw "PSReadLine installation failed in $Shell."
            }
        }
    }
}

Invoke-Step "Configuring PowerShell completion, prompt, and mac shortcut..." {
    Set-PersonalPowerShellProfile
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

Invoke-Step "Installing Windows Local Auth Cleaner v0.2.0 from GitHub Release..." {
    $CleanerReleaseTag = 'v0.2.0'
    $ReleaseBase = "https://github.com/kimpossible-TY/study_package/releases/download/$CleanerReleaseTag"
    $InstallerPath = Join-Path ([IO.Path]::GetTempPath()) ("windows-auth-cleaner-install-{0}.ps1" -f [Guid]::NewGuid().ToString('N'))
    try {
        Invoke-WebRequest -Uri "$ReleaseBase/install.ps1" -OutFile $InstallerPath -UseBasicParsing
        & $InstallerPath -Tag $CleanerReleaseTag
    }
    finally {
        if (Test-Path -LiteralPath $InstallerPath) {
            Remove-Item -LiteralPath $InstallerPath -Force
        }
    }
}

Write-Host "`nPersonal Windows development environment configured successfully." -ForegroundColor Green
Write-Host "Installed with Scoop in the current user account." -ForegroundColor Green
Write-Host "PowerToys was installed from the Scoop extras bucket for 64-bit Windows." -ForegroundColor Green
Write-Host "PowerShell profiles saved for Windows PowerShell and PowerShell 7. Open a new shell to load all settings." -ForegroundColor Green
Write-Host "Mac SSH shortcut: mac (connect Dev Tunnel first). Run wac before leaving to remove managed profile settings." -ForegroundColor Green
