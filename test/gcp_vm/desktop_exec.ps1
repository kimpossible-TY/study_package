param(
    [Parameter(Mandatory = $true)]
    [string]$CommandLine,

    [string]$WorkingDirectory = "C:\mac-mirror-test",

    [switch]$Background,

    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"

$workDir = "C:\mac-mirror-test"
if (-not (Test-Path $workDir)) {
    New-Item -ItemType Directory -Force -Path $workDir | Out-Null
}

$runId = [Guid]::NewGuid().ToString("N").Substring(0, 12)
$taskName = "MacMirrorInteractive_$runId"
$principal = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\tester" -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 2)

if ($Background) {
    $runnerPs1 = Join-Path $workDir "bg_$runId.ps1"
    $scriptBody = @"
Set-Location -LiteralPath '$WorkingDirectory'
Invoke-Expression @'
$CommandLine
'@
"@
    Set-Content -LiteralPath $runnerPs1 -Value $scriptBody -Encoding UTF8
    $action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$runnerPs1`"" -WorkingDirectory $WorkingDirectory
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings -Force | Out-Null
    Start-ScheduledTask -TaskName $taskName
    Start-Sleep -Milliseconds 600
    Write-Output "Started interactive task $taskName"
    exit 0
}

$outFile = Join-Path $workDir "out_$runId.txt"
$rcFile = Join-Path $workDir "rc_$runId.txt"
$runnerPs1 = Join-Path $workDir "sync_$runId.ps1"

$scriptBody = @"
`$ErrorActionPreference = 'Continue'
try {
    Set-Location -LiteralPath '$WorkingDirectory'
    & {
        $CommandLine
    } *> '$outFile'
    if (`$LASTEXITCODE -ne `$null) { `$rc = [int]`$LASTEXITCODE } else { `$rc = 0 }
} catch {
    (`$_ | Out-String) | Out-File -FilePath '$outFile' -Append -Encoding UTF8
    `$rc = 1
}
Set-Content -LiteralPath '$rcFile' -Value `$rc -Encoding ASCII
"@
Set-Content -LiteralPath $runnerPs1 -Value $scriptBody -Encoding UTF8

$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$runnerPs1`"" -WorkingDirectory $WorkingDirectory
Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings -Force | Out-Null

try {
    Start-ScheduledTask -TaskName $taskName
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not (Test-Path -LiteralPath $rcFile)) {
        if ((Get-Date) -gt $deadline) {
            throw "Timed out waiting for interactive desktop command ($TimeoutSeconds s)"
        }
        Start-Sleep -Milliseconds 150
    }
    if (Test-Path -LiteralPath $outFile) {
        Get-Content -LiteralPath $outFile -Raw | Write-Host -NoNewline
    }
    $exitCode = [int]((Get-Content -LiteralPath $rcFile -Raw).Trim())
    exit $exitCode
} finally {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath $workDir -Filter "*_$runId.*" -ErrorAction SilentlyContinue | ForEach-Object { $_.Delete() }
}
