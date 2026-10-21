$ErrorActionPreference = 'Stop'
$ScriptPath = Join-Path $PSScriptRoot '..\computer_setting.ps1'
$Tokens = $null
$ParseErrors = $null
$Ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref]$Tokens, [ref]$ParseErrors)
if ($ParseErrors.Count) { throw ($ParseErrors | Out-String) }
$Definition = $Ast.Find({ param($Node) $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -eq 'Set-PersonalPowerShellProfile' }, $true)
$FixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $FixtureRoot | Out-Null
try {
    # Extract only the profile function; never execute installs or touch real profiles.
    $FunctionText = $Definition.Extent.Text.Replace("[Environment]::GetFolderPath('MyDocuments')", "'" + $FixtureRoot.Replace("'", "''") + "'")
    . ([scriptblock]::Create($FunctionText))
    $PROFILE = [pscustomobject]@{ CurrentUserAllHosts = (Join-Path $FixtureRoot 'PowerShell\profile.ps1') }
    $ProfilePath = $PROFILE.CurrentUserAllHosts
    New-Item -ItemType Directory -Path (Split-Path $ProfilePath) -Force | Out-Null
    $Personal = "# personal 설정`r`nfunction personal { 'kept' }`r`n"
    [IO.File]::WriteAllText($ProfilePath, $Personal, (New-Object Text.UTF8Encoding($true)))
    # Test settings without mutating the test shell's real key handlers/prompt.
    function Import-Module {}
    function Set-PSReadLineOption { param($EditMode, $BellStyle, $PredictionSource, $PredictionViewStyle) }
    function Set-PSReadLineKeyHandler { param($Key, $Function) }
    function Get-Command {
        param($Name, $ErrorAction)
        if ($Name -eq 'Set-PSReadLineOption') {
            [pscustomobject]@{ Parameters = @{ PredictionSource = $true; PredictionViewStyle = $true } }
        }
    }
    function ssh { $script:SshArguments = @($args) }
    Set-PersonalPowerShellProfile
    $First = [IO.File]::ReadAllText($ProfilePath)
    Set-PersonalPowerShellProfile
    $Second = [IO.File]::ReadAllText($ProfilePath)
    if ($First -cne $Second) { throw 'Repeated setup changed the profile.' }
    if (!$Second.StartsWith($Personal)) { throw 'Existing profile content changed.' }
    foreach ($Edition in @('WindowsPowerShell', 'PowerShell')) {
        $Text = [IO.File]::ReadAllText((Join-Path $FixtureRoot "$Edition\profile.ps1"))
        if ([regex]::Matches($Text, '# BEGIN study_package PowerShell settings').Count -ne 1) { throw 'Expected exactly one managed block.' }
        if (!$Text.Contains('Set-PSReadLineOption -PredictionSource History')) { throw 'History predictions missing.' }
    }
    mac -p 2222
    if (($script:SshArguments -join '|') -ne '-o|PubkeyAuthentication=no|taeyoung@127.0.0.1|-p|2222') { throw 'Global mac did not forward SSH arguments.' }
    [IO.File]::AppendAllText($ProfilePath, '# BEGIN study_package PowerShell settings')
    $Malformed = [IO.File]::ReadAllText($ProfilePath)
    $Refused = $false
    try { Set-PersonalPowerShellProfile } catch { $Refused = $true }
    if (!$Refused -or [IO.File]::ReadAllText($ProfilePath) -cne $Malformed) { throw 'Malformed profile must be left unchanged.' }
    Write-Host 'PowerShell parse, profile persistence, idempotence, and global mac tests passed.'
}
finally {
    Remove-Item -LiteralPath $FixtureRoot -Recurse -Force
    Remove-Item Function:\mac -ErrorAction SilentlyContinue
}
