
# ==============================================================================
# Windows user-level development environment bootstrapper
# ==============================================================================

param(
    [switch]$SkipMacMirror,
    [switch]$Update,
    [string]$MacHost,
    [string]$MacPeer,
    [string]$TailscaleAuthKey
)

$ErrorActionPreference = "Stop"

$MacMirrorViewerReleaseTag = "v1.0.0"
$MacMirrorViewerUrl = "https://github.com/kimpossible-TY/mac-mirror/releases/download/$MacMirrorViewerReleaseTag/mac-mirror-viewer.exe"
$TigerVncViewerUrl = "https://sourceforge.net/projects/tigervnc/files/stable/1.16.2/vncviewer64-1.16.2.exe/download"
$TailscaleStableMetaUrl = "https://pkgs.tailscale.com/stable/?mode=json"
$TailscaleStableBaseUrl = "https://pkgs.tailscale.com/stable"
$MacMirrorTunnelUrl = "https://raw.githubusercontent.com/kimpossible-TY/mac-mirror/main/mac_vnc_tunnel.py"

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
$PersonalPaths = @(
    (Join-Path $env:USERPROFILE 'scoop\shims'),
    (Join-Path $env:LOCALAPPDATA 'Programs\devtunnel'),
    (Join-Path $env:LOCALAPPDATA 'Programs\tailscale'),
    (Join-Path $env:LOCALAPPDATA 'Programs\tigervnc'),
    (Join-Path $env:LOCALAPPDATA 'Programs\mac-mirror'),
    (Join-Path $env:LOCALAPPDATA 'Programs\windows-local-auth-cleaner')
)
foreach ($Path in $PersonalPaths) {
    if ((Test-Path -LiteralPath $Path) -and ($env:Path -split [IO.Path]::PathSeparator -notcontains $Path)) {
        $env:Path = "$Path$([IO.Path]::PathSeparator)$env:Path"
    }
}

function global:mac {
    ssh -o PubkeyAuthentication=no taeyoung@127.0.0.1 @args
}

function global:mac-mirror {
    $TunnelScript = Join-Path $env:LOCALAPPDATA 'Programs\mac-mirror\mac_vnc_tunnel.py'
    $PythonExe = Join-Path $env:USERPROFILE 'scoop\shims\python.exe'
    if (!(Test-Path -LiteralPath $PythonExe -PathType Leaf)) {
        $PythonExe = 'python'
    }
    & $PythonExe $TunnelScript --connect @args
}

function global:mac-mirror-stop {
    $TunnelScript = Join-Path $env:LOCALAPPDATA 'Programs\mac-mirror\mac_vnc_tunnel.py'
    $PythonExe = Join-Path $env:USERPROFILE 'scoop\shims\python.exe'
    if (!(Test-Path -LiteralPath $PythonExe -PathType Leaf)) {
        $PythonExe = 'python'
    }
    & $PythonExe $TunnelScript --stop @args
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

function Test-WindowsTerminalVersion {
    # SIXEL image support used by FlowReader starts with Windows Terminal 1.22.
    # Check both channels so a newer Preview does not hide an outdated stable app.
    $MinimumVersion = [version]'1.22.0.0'
    try {
        $Terminals = @(Get-AppxPackage -Name 'Microsoft.WindowsTerminal*' -ErrorAction Stop |
            Where-Object { $_.Name -in @('Microsoft.WindowsTerminal', 'Microsoft.WindowsTerminalPreview') })
        if ($Terminals.Count -eq 0) {
            Write-Warning 'Windows Terminal was not found. Install Windows Terminal 1.22 or newer from Microsoft Store to use FlowReader.'
            return
        }
        foreach ($Terminal in $Terminals) {
            $InstalledVersion = [version]$Terminal.Version
            Write-Host "$($Terminal.Name) version: $InstalledVersion" -ForegroundColor DarkGray
            if ($InstalledVersion -lt $MinimumVersion) {
                $PackageId = 'Microsoft.WindowsTerminal'
                if ($Terminal.Name -eq 'Microsoft.WindowsTerminalPreview') {
                    $PackageId = 'Microsoft.WindowsTerminal.Preview'
                }
                Write-Warning "Please update $($Terminal.Name) to version 1.22 or newer. FlowReader requires SIXEL image support."
                Write-Warning 'If you do not update, you will not be able to use some features, including FlowReader.'
                Write-Host "Update in Microsoft Store, or run: winget upgrade --id $PackageId --exact" -ForegroundColor Yellow
                Write-Host 'After updating, close all Windows Terminal windows and reopen Windows Terminal before using FlowReader.' -ForegroundColor Yellow
            }
        }
    }
    catch {
        Write-Warning "Windows Terminal version could not be checked: $_"
        Write-Warning 'Check Settings > About in Windows Terminal and update to version 1.22 or newer to use FlowReader.'
    }
}

function Enable-Tls12 {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
}

function Update-ProcessPath {
    $PathParts = @(
        [Environment]::GetEnvironmentVariable("Path", "Machine"),
        [Environment]::GetEnvironmentVariable("Path", "User"),
        (Join-Path $env:USERPROFILE "scoop\shims"),
        (Join-Path $env:LOCALAPPDATA "Programs\devtunnel"),
        (Join-Path $env:LOCALAPPDATA "Programs\tailscale"),
        (Join-Path $env:LOCALAPPDATA "Programs\tigervnc"),
        (Join-Path $env:LOCALAPPDATA "Programs\mac-mirror"),
        (Join-Path $env:LOCALAPPDATA "Programs\windows-local-auth-cleaner")
    )

    $env:Path = ($PathParts | Where-Object { $_ }) -join [IO.Path]::PathSeparator
}

function Add-UserPathEntry {
    param([Parameter(Mandatory = $true)][string]$Directory)

    $UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $UserPathParts = @($UserPath -split [IO.Path]::PathSeparator | Where-Object { $_ })
    if ($UserPathParts -notcontains $Directory) {
        $NewUserPath = (@($UserPathParts) + $Directory) -join [IO.Path]::PathSeparator
        [Environment]::SetEnvironmentVariable("Path", $NewUserPath, "User")
    }
    Update-ProcessPath
}

function Test-PeExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [long]$MinBytes = 1048576
    )

    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    $Item = Get-Item -LiteralPath $Path
    if ($Item.Length -lt $MinBytes) {
        return $false
    }
    $Stream = [IO.File]::OpenRead($Path)
    try {
        $Header = New-Object byte[] 2
        $Read = $Stream.Read($Header, 0, 2)
        return ($Read -eq 2 -and $Header[0] -eq 0x4D -and $Header[1] -eq 0x5A)
    }
    finally {
        $Stream.Dispose()
    }
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

function Test-ScoopPackageInstalled {
    param([Parameter(Mandatory = $true)][string]$Name)

    $null = scoop prefix $Name 2>$null
    return ($LASTEXITCODE -eq 0)
}

function Install-ScoopPackage {
    param([Parameter(Mandatory = $true)][string]$Name)

    if (Test-ScoopPackageInstalled $Name) {
        Write-Host "Already installed (skipping download): $Name" -ForegroundColor DarkGray
        return
    }

    Write-Host "Installing: $Name" -ForegroundColor Cyan
    scoop install $Name
}

function Install-DevTunnel {
    $InstallDir = Join-Path $env:LOCALAPPDATA "Programs\devtunnel"
    $Executable = Join-Path $InstallDir "devtunnel.exe"
    if ((Test-Command "devtunnel") -and !(Test-Path -LiteralPath $Executable -PathType Leaf)) {
        Write-Host "Dev Tunnel CLI is already available (skipping download)." -ForegroundColor DarkGray
        return
    }

    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null

    if ((Test-Path -LiteralPath $Executable -PathType Leaf) -and !$Update) {
        Add-UserPathEntry -Directory $InstallDir
        Write-Host "Dev Tunnel CLI is already installed (skipping download): $Executable" -ForegroundColor DarkGray
        return
    }

    $DownloadPath = Join-Path $InstallDir ("devtunnel-{0}.exe" -f [Guid]::NewGuid().ToString("N"))
    try {
        Enable-Tls12
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

    Add-UserPathEntry -Directory $InstallDir

    Write-Host "Dev Tunnel CLI installed: $Executable" -ForegroundColor DarkGray
    Write-Host "To sign in using your own phone/browser: devtunnel user login -g -d" -ForegroundColor DarkGray
}

function Install-TailscaleUserspace {
    $InstallDir = Join-Path $env:LOCALAPPDATA "Programs\tailscale"
    $StateDir = Join-Path $env:USERPROFILE ".tailscale"
    $TailscaleExe = Join-Path $InstallDir "tailscale.exe"
    $TailscaledExe = Join-Path $InstallDir "tailscaled.exe"

    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    New-Item -ItemType Directory -Path $StateDir -Force | Out-Null

    if ((Test-Path -LiteralPath $TailscaleExe -PathType Leaf) -and (Test-Path -LiteralPath $TailscaledExe -PathType Leaf) -and !$Update) {
        Add-UserPathEntry -Directory $InstallDir
        Write-Host "Tailscale userspace binaries are already installed (skipping download): $InstallDir" -ForegroundColor DarkGray
        return
    }

    Enable-Tls12
    $Meta = Invoke-RestMethod -Uri $TailscaleStableMetaUrl -UseBasicParsing
    $Arch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "amd64" }
    $MsiName = $Meta.MSIs.$Arch
    if ([string]::IsNullOrWhiteSpace($MsiName)) {
        throw "Could not resolve Tailscale MSI filename for architecture '$Arch'."
    }

    $TempRoot = Join-Path ([IO.Path]::GetTempPath()) ("tailscale-extract-{0}" -f [Guid]::NewGuid().ToString("N"))
    $ExtractDir = Join-Path $TempRoot "extract"
    $MsiPath = Join-Path $TempRoot $MsiName
    New-Item -ItemType Directory -Path $ExtractDir -Force | Out-Null

    try {
        Invoke-WebRequest `
            -Uri "$TailscaleStableBaseUrl/$MsiName" `
            -UseBasicParsing `
            -OutFile $MsiPath

        $MsiProcess = Start-Process -FilePath "msiexec.exe" `
            -ArgumentList "/a `"$MsiPath`" /qn TARGETDIR=`"$ExtractDir`"" `
            -Wait -PassThru -NoNewWindow
        if ($MsiProcess.ExitCode -ne 0) {
            throw "Failed to extract Tailscale MSI without admin privileges (msiexec exit code: $($MsiProcess.ExitCode))."
        }

        $FoundDaemon = Get-ChildItem -LiteralPath $ExtractDir -Recurse -Filter "tailscaled.exe" -File | Select-Object -First 1
        if (!$FoundDaemon) {
            throw "tailscaled.exe was not found in the extracted Tailscale MSI."
        }

        $SourceDir = $FoundDaemon.DirectoryName
        Get-ChildItem -LiteralPath $SourceDir -File | Where-Object { $_.Extension -in @(".exe", ".dll") } | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $InstallDir $_.Name) -Force
        }

        if (!(Test-Path -LiteralPath $TailscaleExe -PathType Leaf) -or !(Test-Path -LiteralPath $TailscaledExe -PathType Leaf)) {
            throw "Tailscale binaries were not extracted cleanly into $InstallDir."
        }

        $null = & $TailscaleExe version
        if ($LASTEXITCODE -ne 0) {
            throw "The extracted Tailscale CLI could not be started."
        }
    }
    finally {
        if (Test-Path -LiteralPath $TempRoot) {
            Remove-Item -LiteralPath $TempRoot -Recurse -Force
        }
    }

    Add-UserPathEntry -Directory $InstallDir
    Write-Host "Tailscale userspace binaries installed: $InstallDir" -ForegroundColor DarkGray
}

function Install-MacMirrorViewer {
    $InstallDir = Join-Path $env:LOCALAPPDATA "Programs\mac-mirror"
    $Executable = Join-Path $InstallDir "mac-mirror-viewer.exe"
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null

    if (![string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        $LocalCandidates = @(
            (Join-Path $PSScriptRoot "mac_mirror_viewer\build\mac-mirror-viewer.exe"),
            (Join-Path $PSScriptRoot "mac-mirror-viewer.exe"),
            (Join-Path $PSScriptRoot "dist\mac-mirror-viewer.exe")
        )
        foreach ($Candidate in $LocalCandidates) {
            if (Test-PeExecutable -Path $Candidate -MinBytes 262144) {
                Copy-Item -LiteralPath $Candidate -Destination $Executable -Force
                Add-UserPathEntry -Directory $InstallDir
                Write-Host "Mac Mirror Viewer installed from local build: $Executable" -ForegroundColor DarkGray
                return $true
            }
        }
    }

    Enable-Tls12
    $ExpectedSha256 = $null
    try {
        $ShaText = (Invoke-RestMethod -Uri "$MacMirrorViewerUrl.sha256" -UseBasicParsing)
        if ($ShaText -match '([0-9a-fA-F]{64})') {
            $ExpectedSha256 = $Matches[1].ToLowerInvariant()
        }
    }
    catch {
        $ExpectedSha256 = $null
    }

    if (Test-PeExecutable -Path $Executable -MinBytes 262144) {
        $LocalSha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
        if ([string]::IsNullOrWhiteSpace($ExpectedSha256) -or $LocalSha256 -eq $ExpectedSha256) {
            Add-UserPathEntry -Directory $InstallDir
            Write-Host "Mac Mirror Viewer is already up to date: $Executable" -ForegroundColor DarkGray
            return $true
        }
        Write-Host "Updating Mac Mirror Viewer to latest release..." -ForegroundColor Cyan
    }

    $DownloadPath = Join-Path $InstallDir ("mac-mirror-viewer-{0}.exe" -f [Guid]::NewGuid().ToString("N"))
    try {
        Invoke-WebRequest `
            -Uri $MacMirrorViewerUrl `
            -UseBasicParsing `
            -MaximumRedirection 10 `
            -OutFile $DownloadPath
        if (Test-PeExecutable -Path $DownloadPath -MinBytes 262144) {
            if (![string]::IsNullOrWhiteSpace($ExpectedSha256)) {
                $DownloadedSha256 = (Get-FileHash -LiteralPath $DownloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($DownloadedSha256 -ne $ExpectedSha256) {
                    throw "SHA-256 verification failed for mac-mirror-viewer.exe (expected $ExpectedSha256, got $DownloadedSha256)."
                }
            }
            Move-Item -LiteralPath $DownloadPath -Destination $Executable -Force
            Add-UserPathEntry -Directory $InstallDir
            Write-Host "Mac Mirror Viewer installed from GitHub Release: $Executable" -ForegroundColor DarkGray
            return $true
        }
    }
    catch {
        Write-Host "Mac Mirror Viewer release download skipped ($($_.Exception.Message))." -ForegroundColor DarkGray
    }
    finally {
        if (Test-Path -LiteralPath $DownloadPath) {
            Remove-Item -LiteralPath $DownloadPath -Force
        }
    }

    if (![string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        $ViewerSrcRoot = Join-Path $PSScriptRoot "mac_mirror_viewer"
        $GccCmd = Get-Command "gcc.exe" -ErrorAction SilentlyContinue
        $GxxCmd = Get-Command "g++.exe" -ErrorAction SilentlyContinue
        if ((Test-Path -LiteralPath (Join-Path $ViewerSrcRoot "CMakeLists.txt") -PathType Leaf) -and $GccCmd -and $GxxCmd) {
            $BuildTemp = Join-Path ([IO.Path]::GetTempPath()) ("mac-mirror-build-{0}" -f [Guid]::NewGuid().ToString("N"))
            New-Item -ItemType Directory -Path $BuildTemp -Force | Out-Null
            try {
                $MinizObj = Join-Path $BuildTemp "miniz.o"
                $BuiltExe = Join-Path $BuildTemp "mac-mirror-viewer.exe"
                & $GccCmd.Source -O2 -c (Join-Path $ViewerSrcRoot "core_client\miniz.c") -o $MinizObj
                if ($LASTEXITCODE -eq 0) {
                    $CppFiles = @(
                        (Get-ChildItem -LiteralPath (Join-Path $ViewerSrcRoot "core_client") -Filter "*.cpp" -File | Select-Object -ExpandProperty FullName),
                        (Get-ChildItem -LiteralPath (Join-Path $ViewerSrcRoot "win_ui") -Filter "*.cpp" -File | Select-Object -ExpandProperty FullName)
                    ) | ForEach-Object { $_ }
                    & $GxxCmd.Source -std=c++17 -O2 `
                        -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 `
                        "-I$(Join-Path $ViewerSrcRoot 'include')" `
                        "-I$(Join-Path $ViewerSrcRoot 'core_client')" `
                        "-I$(Join-Path $ViewerSrcRoot 'win_ui')" `
                        @CppFiles $MinizObj `
                        -mwindows -static -static-libgcc -static-libstdc++ -s `
                        -ld3d11 -ldxgi -ld2d1 -ldwrite -lws2_32 -lbcrypt -ldwmapi -limm32 -lole32 -lshell32 -lgdi32 -luser32 -luuid `
                        -o $BuiltExe
                    if ($LASTEXITCODE -eq 0 -and (Test-PeExecutable -Path $BuiltExe -MinBytes 262144)) {
                        Move-Item -LiteralPath $BuiltExe -Destination $Executable -Force
                        Add-UserPathEntry -Directory $InstallDir
                        Write-Host "Mac Mirror Viewer built from local C++17 source: $Executable" -ForegroundColor DarkGray
                        return $true
                    }
                }
            }
            catch {
                Write-Warning "Local Mac Mirror Viewer build skipped: $_"
            }
            finally {
                if (Test-Path -LiteralPath $BuildTemp) {
                    Remove-Item -LiteralPath $BuildTemp -Recurse -Force
                }
            }
        }
    }

    Write-Host "Mac Mirror Viewer binary not available yet; falling back to TigerVNC Viewer 1.16.2." -ForegroundColor Yellow
    return $false
}

function Install-TigerVncViewer {
    $InstallDir = Join-Path $env:LOCALAPPDATA "Programs\tigervnc"
    $Executable = Join-Path $InstallDir "vncviewer.exe"
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null

    if (Test-PeExecutable -Path $Executable) {
        Add-UserPathEntry -Directory $InstallDir
        Write-Host "TigerVNC Viewer is already installed: $Executable" -ForegroundColor DarkGray
        return
    }

    Enable-Tls12
    $DownloadPath = Join-Path $InstallDir ("vncviewer-{0}.exe" -f [Guid]::NewGuid().ToString("N"))
    try {
        Invoke-WebRequest `
            -Uri $TigerVncViewerUrl `
            -UseBasicParsing `
            -UserAgent "curl/8.0" `
            -MaximumRedirection 10 `
            -OutFile $DownloadPath
        if (!(Test-PeExecutable -Path $DownloadPath)) {
            throw "The downloaded TigerVNC Viewer file is not a valid Windows executable. Check $TigerVncViewerUrl."
        }
        Move-Item -LiteralPath $DownloadPath -Destination $Executable -Force
    }
    finally {
        if (Test-Path -LiteralPath $DownloadPath) {
            Remove-Item -LiteralPath $DownloadPath -Force
        }
    }

    Add-UserPathEntry -Directory $InstallDir
    Write-Host "TigerVNC Viewer 1.16.2 installed: $Executable" -ForegroundColor DarkGray
}

function Install-MacMirrorTool {
    $InstallDir = Join-Path $env:LOCALAPPDATA "Programs\mac-mirror"
    $TargetScript = Join-Path $InstallDir "mac_vnc_tunnel.py"
    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null

    $LocalScript = $null
    if (![string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        $Candidate = Join-Path $PSScriptRoot "mac_vnc_tunnel.py"
        if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
            $LocalScript = $Candidate
        }
    }

    if ($LocalScript) {
        $SameLocal = (Test-Path -LiteralPath $TargetScript -PathType Leaf) -and
            ((Get-FileHash -LiteralPath $LocalScript -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $TargetScript -Algorithm SHA256).Hash)
        if ($SameLocal) {
            Write-Host "Mac VNC tunnel helper is already up to date: $TargetScript" -ForegroundColor DarkGray
        }
        else {
            Copy-Item -LiteralPath $LocalScript -Destination $TargetScript -Force
            Write-Host "Mac VNC tunnel helper installed from local repo: $TargetScript" -ForegroundColor DarkGray
        }
    }
    elseif ((Test-Path -LiteralPath $TargetScript -PathType Leaf) -and !$Update -and ([IO.File]::ReadAllText($TargetScript, [Text.Encoding]::UTF8).Contains("def socks5_connect("))) {
        Write-Host "Mac VNC tunnel helper is already installed (skipping download): $TargetScript" -ForegroundColor DarkGray
    }
    else {
        Enable-Tls12
        $DownloadPath = Join-Path $InstallDir ("mac_vnc_tunnel-{0}.py" -f [Guid]::NewGuid().ToString("N"))
        try {
            Invoke-WebRequest `
                -Uri $MacMirrorTunnelUrl `
                -UseBasicParsing `
                -OutFile $DownloadPath
            $ScriptText = [IO.File]::ReadAllText($DownloadPath, [Text.Encoding]::UTF8)
            if ([string]::IsNullOrWhiteSpace($ScriptText) -or !$ScriptText.Contains("def socks5_connect(")) {
                throw "The downloaded mac_vnc_tunnel.py file failed validation."
            }
            Move-Item -LiteralPath $DownloadPath -Destination $TargetScript -Force
        }
        finally {
            if (Test-Path -LiteralPath $DownloadPath) {
                Remove-Item -LiteralPath $DownloadPath -Force
            }
        }
        Write-Host "Mac VNC tunnel helper installed: $TargetScript" -ForegroundColor DarkGray
    }

    $Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $ConnectCmd = "@echo off`r`nsetlocal`r`nset `"PYTHON_EXE=%USERPROFILE%\scoop\shims\python.exe`"`r`nif not exist `"%PYTHON_EXE%`" set `"PYTHON_EXE=python`"`r`n`"%PYTHON_EXE%`" `"%~dp0mac_vnc_tunnel.py`" --connect %*`r`n"
    $StopCmd = "@echo off`r`nsetlocal`r`nset `"PYTHON_EXE=%USERPROFILE%\scoop\shims\python.exe`"`r`nif not exist `"%PYTHON_EXE%`" set `"PYTHON_EXE=python`"`r`n`"%PYTHON_EXE%`" `"%~dp0mac_vnc_tunnel.py`" --stop %*`r`n"
    [IO.File]::WriteAllText((Join-Path $InstallDir "mac-mirror.cmd"), $ConnectCmd, $Utf8NoBom)
    [IO.File]::WriteAllText((Join-Path $InstallDir "mac-mirror-stop.cmd"), $StopCmd, $Utf8NoBom)

    Add-UserPathEntry -Directory $InstallDir
}

Write-Host "`nPreparing personal Windows development environment..." -ForegroundColor Cyan

Invoke-Step "Checking Windows Terminal version for FlowReader..." {
    Test-WindowsTerminalVersion
}

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
    $AllScoopPackages = @("git") + $ScoopPackages + $ExtraPackages
    $MissingScoopPackages = @($AllScoopPackages | Where-Object { !(Test-ScoopPackageInstalled $_) })
    if (($MissingScoopPackages.Count -gt 0 -or $Update) -and (Test-ScoopBucketsClean)) {
        scoop update
    }
    elseif ($MissingScoopPackages.Count -eq 0) {
        Write-Host "All Scoop packages are already installed (skipping scoop update)." -ForegroundColor DarkGray
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

Invoke-Step "Installing Microsoft Dev Tunnel CLI for the current user..." {
    Install-DevTunnel
}

Invoke-Step "Installing Tailscale userspace CLI (no admin rights required)..." {
    Install-TailscaleUserspace
}

Invoke-Step "Installing Mac Mirror Viewer (with TigerVNC 1.16.2 fallback)..." {
    if (!(Install-MacMirrorViewer)) {
        Install-TigerVncViewer
    }
}

Invoke-Step "Installing Mac VNC tunnel & launcher helper (mac_vnc_tunnel.py)..." {
    Install-MacMirrorTool
}

Invoke-Step "Configuring PowerShell completion, prompt, and mac shortcuts..." {
    Set-PersonalPowerShellProfile
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

Invoke-Step "Installing Windows Local Auth Cleaner v0.3.0 from GitHub Release..." {
    $CleanerReleaseTag = 'v0.3.0'
    $ReleaseBase = "https://github.com/kimpossible-TY/windows-local-auth-cleaner/releases/download/$CleanerReleaseTag"
    $CleanerDir = Join-Path $env:LOCALAPPDATA "Programs\windows-local-auth-cleaner"
    $CleanerExe = Join-Path $CleanerDir "windows-local-auth-cleaner.exe"
    $CleanerShim = Join-Path $CleanerDir "wac.cmd"
    $DownloadsCleanerExe = Join-Path $env:USERPROFILE "Downloads\windows-local-auth-cleaner.exe"

    $NeedsInstall = $true
    if ((Test-Path -LiteralPath $CleanerExe -PathType Leaf) -and (Test-Path -LiteralPath $CleanerShim -PathType Leaf) -and !$Update) {
        try {
            Enable-Tls12
            $ShaText = Invoke-RestMethod -Uri "$ReleaseBase/windows-local-auth-cleaner.exe.sha256" -UseBasicParsing
            if ($ShaText -match '([0-9a-fA-F]{64})') {
                $ExpectedHash = $Matches[1].ToLowerInvariant()
                $LocalHash = (Get-FileHash -LiteralPath $CleanerExe -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($LocalHash -eq $ExpectedHash) {
                    $NeedsInstall = $false
                    Add-UserPathEntry -Directory $CleanerDir
                    if (!(Test-Path -LiteralPath $DownloadsCleanerExe -PathType Leaf) -or ((Get-FileHash -LiteralPath $DownloadsCleanerExe -Algorithm SHA256).Hash.ToLowerInvariant() -ne $LocalHash)) {
                        New-Item -ItemType Directory -Path (Split-Path -Parent $DownloadsCleanerExe) -Force | Out-Null
                        Copy-Item -LiteralPath $CleanerExe -Destination $DownloadsCleanerExe -Force
                    }
                    Write-Host "Windows Local Auth Cleaner $CleanerReleaseTag is already up to date (skipping download): $CleanerExe" -ForegroundColor DarkGray
                }
            }
        }
        catch {
            Write-Host "Checksum check skipped ($($_.Exception.Message))." -ForegroundColor DarkGray
        }
    }

    if ($NeedsInstall) {
        $LocalInstaller = $null
        if (![string]::IsNullOrWhiteSpace($PSScriptRoot)) {
            $CandidateInstaller = Join-Path $PSScriptRoot "windows_local_auth_cleaner\install.ps1"
            if (Test-Path -LiteralPath $CandidateInstaller -PathType Leaf) {
                $LocalInstaller = $CandidateInstaller
            }
        }

        if ($LocalInstaller) {
            & $LocalInstaller -Tag $CleanerReleaseTag
        }
        else {
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
    }
}

if (!$SkipMacMirror) {
    Invoke-Step "Starting Tailscale userspace daemon and connecting Mac screen mirroring..." {
        $MirrorArgs = @()
        if (![string]::IsNullOrWhiteSpace($MacHost)) {
            $MirrorArgs += @("--host", $MacHost)
        }
        if (![string]::IsNullOrWhiteSpace($MacPeer)) {
            $MirrorArgs += @("--peer", $MacPeer)
        }
        if (![string]::IsNullOrWhiteSpace($TailscaleAuthKey)) {
            $MirrorArgs += @("--auth-key", $TailscaleAuthKey)
        }
        try {
            mac-mirror @MirrorArgs
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "Mac screen mirroring did not finish connecting (exit code $LASTEXITCODE). Run 'mac-mirror' once Tailscale sign-in and Mac share acceptance are ready."
            }
        }
        catch {
            Write-Warning "Mac screen mirroring setup warning: $_"
            Write-Warning "All tools are installed. Run 'mac-mirror' whenever you are ready to connect."
        }
    }
}

Write-Host "`nPersonal Windows development environment configured successfully." -ForegroundColor Green
Write-Host "Installed with Scoop in the current user account." -ForegroundColor Green
Write-Host "PowerToys was installed from the Scoop extras bucket for 64-bit Windows." -ForegroundColor Green
Write-Host "PowerShell profiles saved for Windows PowerShell and PowerShell 7. Open a new shell to load all settings." -ForegroundColor Green
Write-Host "Mac screen mirroring shortcut: mac-mirror (stop & sign out before leaving: mac-mirror-stop --logout)." -ForegroundColor Green
Write-Host "Mac SSH shortcut: mac (connect Dev Tunnel first). Run wac before leaving to remove managed profile settings." -ForegroundColor Green

if (!$SkipMacMirror) {
    Invoke-Step "Opening Lagrangian web viewer in Microsoft Edge via Tailscale SOCKS5..." {
        $EdgeCandidates = @()
        foreach ($ProgramRoot in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
            if (![string]::IsNullOrWhiteSpace($ProgramRoot)) {
                $EdgeCandidates += (Join-Path $ProgramRoot "Microsoft\Edge\Application\msedge.exe")
            }
        }
        $EdgeExe = $EdgeCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        if (!$EdgeExe) {
            $EdgeCmd = Get-Command "msedge.exe" -ErrorAction SilentlyContinue
            if ($EdgeCmd) {
                $EdgeExe = $EdgeCmd.Source
            }
        }

        if ($EdgeExe) {
            $TempRoot = if (![string]::IsNullOrWhiteSpace($env:TEMP)) { $env:TEMP } else { [IO.Path]::GetTempPath() }
            $EdgeUserDataDir = Join-Path $TempRoot "ts-edge"
            $EdgeArgs = "--proxy-server=`"socks5://127.0.0.1:1055`" --user-data-dir=`"$EdgeUserDataDir`" `"https://kimtaeyoungs-macbook-air-daemon.tail8adc61.ts.net:8769/`""
            try {
                Start-Process -FilePath $EdgeExe -ArgumentList $EdgeArgs
            }
            catch {
                Write-Warning "Failed to launch Microsoft Edge for Lagrangian web viewer: $_"
            }
        }
        else {
            Write-Warning "Microsoft Edge (msedge.exe) was not found; skipping automatic browser launch."
        }
    }
}
