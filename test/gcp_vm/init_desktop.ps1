$ErrorActionPreference = "Stop"

# Keep desktop clean of Server Manager and network prompt popups during GUI tests
if (Test-Path "HKLM:\SOFTWARE\Microsoft\ServerManager") {
    Set-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\ServerManager" -Name "DoNotOpenServerManagerAtLogon" -Value 1 -Type DWord
}
New-Item -Path "HKLM:\SYSTEM\CurrentControlSet\Control\Network\NewNetworkWindowOff" -Force | Out-Null

powercfg /change monitor-timeout-ac 0
powercfg /change standby-timeout-ac 0

# Register and start local SOCKS5 test proxy on 127.0.0.1:1055 -> 127.0.0.1:15900
$taskName = "MacMirrorSocks5Bridge"
$existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($existing) {
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}
Get-Process -Name "socks5_bridge" -ErrorAction SilentlyContinue | Stop-Process -Force

$action = New-ScheduledTaskAction -Execute "C:\mac-mirror-test\socks5_bridge.exe" -Argument "1055 15900" -WorkingDirectory "C:\mac-mirror-test"
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1)

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Start-Sleep -Seconds 1

netstat -ano | Select-String ":1055"
