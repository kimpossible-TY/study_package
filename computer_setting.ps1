# 스크립트 실행 정책 완화
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# Scoop 설치
irm get.scoop.sh | iex

# add extras bucket
scoop bucket add extras

# install gh
scoop install gh git neovim powertoys

# 자동완성
# 관리자 권한 없이 현재 사용자 계정에 최신 PSReadLine(2.2.6)을 강제 설치합니다.
Install-Module -Name PSReadLine -RequiredVersion 2.2.6 -Force -SkipPublisherCheck -AllowClobber -Scope CurrentUser

# 프로필 파일과 폴더를 강제로 생성합니다.
New-Item -Type File -Path $PROFILE -Force

# 자동 제안 기능을 프로필 파일에 자동으로 써넣습니다.
$ProfileContent = @"
Import-Module PSReadLine -RequiredVersion 2.2.6
Set-PSReadLineOption -PredictionSource History
Set-PSReadLineOption -PredictionViewStyle ListView

#Neovim alias 설정
if (Get-Command nvim -ErrorAction SilentlyContinue) {
    Set-Alias vi nvim
    Set-Alias vim nvim
}
"@
Set-Content -Path $PROFILE -Value $ProfileContent -Encoding UTF8

# Neovim 설정 폴더 생성
$NvimConfigPath = "$env:LOCALAPPDATA\nvim"
if (!(Test-Path $NvimConfigPath)) {
    New-Item -Type Directory -Path $NvimConfigPath -Force
}

# GitHub에서 init.lua 가져오기 (파일이 없는 경우에만)
$InitLuaPath = Join-Path $NvimConfigPath "init.lua"
if (!(Test-Path $InitLuaPath)) {
    $RawUrl = "https://raw.githubusercontent.com/kimpossible-TY/dotfiles/main/nvim/.config/nvim/init.lua"
    try {
        Write-Host "Downloading init.lua from GitHub..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri $RawUrl -OutFile $InitLuaPath
        Write-Host "init.lua successfully installed!" -ForegroundColor Green
    } catch {
        Write-Host "Failed to download init.lua. Please check your internet connection or URL." -ForegroundColor Red
    }
} else {
    Write-Host "init.lua already exists. Skipping download." -ForegroundColor Yellow
}

Write-Host "✨ setting is over! please re-boot PowerShell" -ForegroundColor Green

