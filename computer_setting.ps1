# ==============================================================================
# 1. Environment & Execution Policy Setup
# ==============================================================================
# Relaxation of script execution policy for the current user session
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# Install Scoop (Minimalist Windows Package Manager)
irm get.scoop.sh | iex

# Install Git and GitHub CLI first to unlock the 'extras' bucket and repositories
scoop install gh git

# Add the 'extras' bucket for richer developer utilities
scoop bucket add extras

# Install standard core packages
scoop install neovim powertoys make gcc oh-my-posh

# ==============================================================================
# 2. PowerShell Profile, Autocompletion & Theme Configuration
# ==============================================================================
Write-Host "`nConfiguring PowerShell Profile, Autocompletion, and Theme..." -ForegroundColor Cyan

# Force install the latest PSReadLine for high-performance context caching without admin rights
Install-Module -Name PSReadLine -RequiredVersion 2.2.6 -Force -SkipPublisherCheck -AllowClobber -Scope CurrentUser

# Ensure the profile file and its directory structurally exist
New-Item -Type File -Path $PROFILE -Force

# Generate comprehensive configuration content for the PowerShell Profile
$ProfileContent = @"
# Import advanced terminal line editing
Import-Module PSReadLine -RequiredVersion 2.2.6 -Force
Set-PSReadLineOption -PredictionSource History
Set-PSReadLineOption -PredictionViewStyle ListView

# Neovim alias configuration
if (Get-Command nvim -ErrorAction SilentlyContinue) {
    Set-Alias vi nvim
    Set-Alias vim nvim
}

# Initialize Oh My Posh with a highly readable default theme
oh-my-posh init pwsh --config '`$env:USERPROFILE\AppData\Local\Programs\oh-my-posh\themes\jandedobbeleer.omp.json' | Invoke-Expression
"@

# Write content to the profile using cross-platform UTF8 encoding
Set-Content -Path $PROFILE -Value $ProfileContent -Encoding UTF8

Write-Host "✅ PowerShell profile configured successfully." -ForegroundColor Green

# ==============================================================================
# 3. Neovim Configuration Synchronization
# ==============================================================================
Write-Host "`nSynchronizing Neovim configuration files..." -ForegroundColor Cyan

# Define local configuration matrix path
$NvimConfigPath = "$env:LOCALAPPDATA\nvim"
if (!(Test-Path $NvimConfigPath)) {
    New-Item -Type Directory -Path $NvimConfigPath -Force
}

# Pull down the customized init.lua setup securely if not already present
$InitLuaPath = Join-Path $NvimConfigPath "init.lua"
if (!(Test-Path $InitLuaPath)) {
    $RawUrl = "https://raw.githubusercontent.com/kimpossible-TY/dotfiles/main/nvim/.config/nvim/init.lua"
    try {
        Write-Host "Downloading init.lua from remote repository..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri $RawUrl -OutFile $InitLuaPath
        Write-Host "init.lua successfully deployed!" -ForegroundColor Green
    } catch {
        Write-Host "Failed to pull init.lua. Verify active network gateway or raw URL endpoint." -ForegroundColor Red
    }
} else {
    Write-Host "init.lua already local. Skipping downstream synchronization." -ForegroundColor Yellow
}

# ==============================================================================
# 4. Execution Finish & Post-Install Action Reminders
# ==============================================================================
Write-Host "`n System configuration script executed successfully!" -ForegroundColor Green
Write-Host "------------------------------------------------------------------------" -ForegroundColor Gray