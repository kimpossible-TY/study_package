# ==============================================================================
# Windows bootstrapper for a WSL-owned development environment
# ==============================================================================

$ErrorActionPreference = "Stop"

$DistroName = "Ubuntu"

function Test-Command {
    param([Parameter(Mandatory = $true)][string]$Name)

    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Invoke-Wsl {
    param([Parameter(Mandatory = $true)][string]$Command)

    wsl.exe -d $DistroName -- bash -lc $Command
}

Write-Host "`nPreparing WSL development environment..." -ForegroundColor Cyan

if (!(Test-Command "wsl.exe")) {
    throw "wsl.exe is not available on this Windows installation. Enable WSL first, then rerun this script."
}

$InstalledDistros = @(wsl.exe --list --quiet 2>$null)
if ($InstalledDistros -notcontains $DistroName) {
    Write-Host "Installing WSL distribution: $DistroName" -ForegroundColor Cyan
    wsl.exe --install -d $DistroName
    Write-Host "`nWSL installation has been requested. Reboot if Windows asks for it, complete the Ubuntu first-run user setup, then rerun this script." -ForegroundColor Yellow
    exit 0
}

Write-Host "Updating packages and installing developer tools inside WSL..." -ForegroundColor Cyan
Invoke-Wsl @'
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

sudo apt-get update
sudo apt-get install -y \
  build-essential \
  ca-certificates \
  curl \
  gpg \
  make \
  neovim \
  unzip

if ! command -v gh >/dev/null 2>&1; then
  sudo mkdir -p -m 755 /etc/apt/keyrings
  curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg |
    sudo tee /etc/apt/keyrings/githubcli-archive-keyring.gpg >/dev/null
  sudo chmod go+r /etc/apt/keyrings/githubcli-archive-keyring.gpg
  echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" |
    sudo tee /etc/apt/sources.list.d/github-cli.list >/dev/null
  sudo apt-get update
  sudo apt-get install -y gh
fi

mkdir -p "$HOME/.local/bin" "$HOME/.config/nvim"

if ! command -v oh-my-posh >/dev/null 2>&1; then
  curl -fsSL https://ohmyposh.dev/install.sh | bash -s -- -d "$HOME/.local/bin"
fi

if [ ! -f "$HOME/.config/nvim/init.lua" ]; then
  curl -fsSL \
    https://raw.githubusercontent.com/kimpossible-TY/dotfiles/main/nvim/.config/nvim/init.lua \
    -o "$HOME/.config/nvim/init.lua"
fi

BASHRC="$HOME/.bashrc"
START="# >>> study_package WSL dev shell >>>"

if ! grep -Fq "$START" "$BASHRC"; then
  cat >> "$BASHRC" <<'BASHRC_BLOCK'

# >>> study_package WSL dev shell >>>
export PATH="$HOME/.local/bin:$PATH"

if command -v nvim >/dev/null 2>&1; then
  alias vi='nvim'
  alias vim='nvim'
fi

if command -v oh-my-posh >/dev/null 2>&1; then
  eval "$(oh-my-posh init bash)"
fi
# <<< study_package WSL dev shell <<<
BASHRC_BLOCK
fi
'@

Write-Host "`nWSL development environment configured successfully." -ForegroundColor Green
Write-Host "All downloaded tools and configuration files were installed inside $DistroName." -ForegroundColor Green
Write-Host "Open it with: wsl -d $DistroName" -ForegroundColor Gray
