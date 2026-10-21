$ErrorActionPreference = 'Stop'
$Root = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Root | Out-Null
$OriginalPath = $env:Path
$OriginalUserProfile = $env:USERPROFILE
try {
    $env:USERPROFILE = $Root
    $global:CleanerTestFixtureUserPath = 'existing-entry'
    $global:CleanerTestPayload = [Text.Encoding]::ASCII.GetBytes('fixture executable v0.2.0')
    $global:CleanerTestBadChecksum = $false
    $global:CleanerTestRequestedUrls = @()
    $Installer = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '..\windows_local_auth_cleaner\install.ps1'))
    # Redirect registry PATH writes to a fixture variable. No real user settings change.
    $Installer = $Installer.Replace('[Environment]::GetEnvironmentVariable("Path", "User")', '$global:CleanerTestFixtureUserPath')
    $Installer = $Installer.Replace('[Environment]::SetEnvironmentVariable("Path", $NewUserPath, "User")', '$global:CleanerTestFixtureUserPath = $NewUserPath')
    $FixtureInstaller = Join-Path $Root 'install.ps1'
    [IO.File]::WriteAllText($FixtureInstaller, $Installer, (New-Object Text.UTF8Encoding($true)))
    function Invoke-WebRequest {
        param($Uri, $OutFile, [switch]$UseBasicParsing)
        $global:CleanerTestRequestedUrls += $Uri
        if ($Uri.EndsWith('.sha256')) {
            $Hasher = [Security.Cryptography.SHA256]::Create()
            try { $Hash = ([BitConverter]::ToString($Hasher.ComputeHash($global:CleanerTestPayload))).Replace('-', '').ToLowerInvariant() }
            finally { $Hasher.Dispose() }
            if ($global:CleanerTestBadChecksum) { $Hash = '0' * 64 }
            [IO.File]::WriteAllText($OutFile, "$Hash  windows-local-auth-cleaner.exe`n")
        }
        else { [IO.File]::WriteAllBytes($OutFile, $global:CleanerTestPayload) }
    }
    $InstallDir = Join-Path $Root 'installed'
    & $FixtureInstaller -Tag v0.2.0 -InstallDir $InstallDir
    $Exe = Join-Path $InstallDir 'windows-local-auth-cleaner.exe'
    $Expected = [Convert]::ToBase64String($global:CleanerTestPayload)
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($Exe)) -ne $Expected) { throw 'Executable was not installed.' }
    $Shim = [IO.File]::ReadAllText((Join-Path $InstallDir 'wac.cmd'))
    if (!$Shim.Contains('"%~dp0windows-local-auth-cleaner.exe" %*')) { throw 'wac wrapper is invalid.' }
    if (($env:Path -split [IO.Path]::PathSeparator)[0] -ne $InstallDir) { throw 'New installation must precede stale Scoop shims.' }
    $DownloadsExe = Join-Path $Root 'Downloads\windows-local-auth-cleaner.exe'
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($DownloadsExe)) -ne $Expected) { throw 'Downloads copy missing.' }
    & $FixtureInstaller -Tag v0.2.0 -InstallDir $InstallDir
    if (@($global:CleanerTestFixtureUserPath -split [IO.Path]::PathSeparator | Where-Object { $_ -eq $InstallDir }).Count -ne 1) { throw 'PATH entry duplicated.' }
    $global:CleanerTestBadChecksum = $true
    $global:CleanerTestPayload = [Text.Encoding]::ASCII.GetBytes('bad update')
    $Refused = $false
    try { & $FixtureInstaller -Tag v0.2.0 -InstallDir $InstallDir } catch { $Refused = $true }
    if (!$Refused) { throw 'Checksum mismatch was accepted.' }
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($Exe)) -ne $Expected) { throw 'Failed update replaced the previous executable.' }
    if (@(Get-ChildItem $InstallDir -Filter 'download-*').Count) { throw 'Temporary downloads left behind.' }
    if (@($global:CleanerTestRequestedUrls | Where-Object { $_ -notlike 'https://github.com/kimpossible-TY/study_package/releases/download/v0.2.0/*' }).Count) { throw 'Installer used an unexpected release URL.' }
    Write-Host 'Installer version URL, checksum, update, PATH, and Downloads tests passed.'
}
finally {
    Remove-Variable -Name CleanerTest* -Scope Global -ErrorAction SilentlyContinue
    $env:Path = $OriginalPath
    $env:USERPROFILE = $OriginalUserProfile
    Remove-Item -LiteralPath $Root -Recurse -Force
}
