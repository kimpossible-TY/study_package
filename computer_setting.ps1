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
scoop install neovim powertoys make gcc d2coding-nerd-font oh-my-posh

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
# 4. WSL2 State Verification & Rootless Docker Installation
# ==============================================================================
Write-Host "`nEvaluating WSL2 runtime and constructing Rootless Docker daemon..." -ForegroundColor Cyan

# Verify if the host OS exposes the core WSL subsystem interface command
if (!(Get-Command wsl -ErrorAction SilentlyContinue)) {
    Write-Host "🚨 WSL is not installed or enabled on this system architecture." -ForegroundColor Red
    Write-Host "Please configure WSL layer features or request system provisioning." -ForegroundColor Yellow
} else {
    # Check for registered active default Linux instances inside the layer subsystem
    $wslStatus = wsl --list --quiet 2>$null
    if ([string]::IsNullOrEmpty($wslStatus)) {
        Write-Host "⚠️ WSL core is alive, but zero Linux distributions (e.g., Ubuntu) are mapped." -ForegroundColor Yellow
        Write-Host "Please run 'wsl --install -d Ubuntu' in a separate terminal shell if permitted." -ForegroundColor DarkYellow
    } else {
        Write-Host "✅ Valid WSL2 target platform verified. Initiating Rootless Docker setup block..." -ForegroundColor Green
        
        # Embedded isolated provisioning script targeted for the internal Linux ecosystem runtime
        $DockerScript = @'
if ! command -v docker &> /dev/null; then
    echo "🐳 Downloading and executing official Rootless Docker installation utility..."
    curl -fsSL https://get.docker.com/rootless | sh
    
    # Commit permanent path bindings directly to global user bash runtime profile
    echo 'export PATH=$HOME/bin:$PATH' >> ~/.bashrc
    echo 'export DOCKER_HOST=unix://$XDG_RUNTIME_DIR/docker.sock' >> ~/.bashrc
    
    # Inject active bindings immediately into the immediate parent process subshell
    export PATH=$HOME/bin:$PATH
    export DOCKER_HOST=unix://$XDG_RUNTIME_DIR/docker.sock
    
    # Enable session lingering to allow daemon permanence across system lockups and user exit hooks
    loginctl enable-linger $USER
else
    echo "✅ Docker binary stack is already operational inside the WSL runtime target."
fi

# Assertively query the state machine of the background user-space systemd engine
if ! systemctl --user is-active --quiet docker; then
    systemctl --user start docker
    echo "🐳 Rootless Docker user service successfully ignited."
fi
'@

        # Pipe and stream the multi-line string straight into the standard bash receiver mapping inside WSL
        $DockerScript | wsl bash
        Write-Host "✨ Rootless Docker orchestration within the WSL environment is complete!" -ForegroundColor Green
    }
}

# ==============================================================================
# 5. Execution Finish & Post-Install Action Reminders
# ==============================================================================
Write-Host "`n✨ System configuration script executed successfully!" -ForegroundColor Green
Write-Host "------------------------------------------------------------------------" -ForegroundColor Gray
Write-Host "💡 CRITICAL POST-INSTALLATION STEPS:" -ForegroundColor Yellow
Write-Host "1. Close this terminal shell and spawn a fresh PowerShell window."
Write-Host "2. Right-click the terminal window title bar -> Go to 'Properties' or 'Defaults'."
Write-Host "3. Navigate to the 'Font' tab and select 'D2CodingLigature Nerd Font' to map icons properly."
Write-Host "4. Inside VS Code, connect to the WSL platform context before issuing standard"
Write-Host "   'Dev Containers: Reopen in Container' executions to avoid bridge connection drops."
Write-Host "------------------------------------------------------------------------" -ForegroundColor Gray
