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

    $Installed = scoop list 2>$null | Select-String -Pattern "^\s*$([regex]::Escape($Name))\s"
    if ($Installed) {
        Write-Host "Already installed: $Name" -ForegroundColor DarkGray
        return
    }

    Write-Host "Installing: $Name" -ForegroundColor Cyan
    scoop install $Name
}

Write-Host "`nPreparing personal Windows development environment..." -ForegroundColor Cyan

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
    }
}
else {
    Write-Host "Scoop is already installed." -ForegroundColor DarkGray
}

Invoke-Step "Installing Git for Scoop..." {
    Install-ScoopPackage "git"
}

Invoke-Step "Updating Scoop..." {
    scoop update
}

Invoke-Step "Installing base Scoop packages..." {
    foreach ($Package in $ScoopPackages) {
        Install-ScoopPackage $Package
    }
}

Invoke-Step "Adding Scoop extras bucket..." {
    if (!(scoop bucket list | Select-String -Pattern "^\s*extras\s*$")) {
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

Write-Host "`nPersonal Windows development environment configured successfully." -ForegroundColor Green
Write-Host "Installed with Scoop in the current user account." -ForegroundColor Green
Write-Host "PowerToys was installed from the Scoop extras bucket for 64-bit Windows." -ForegroundColor Green
