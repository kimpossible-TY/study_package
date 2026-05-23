# 스크립트 실행 정책 완화
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# Scoop 설치
irm get.scoop.sh | iex

# install gh
scoop install gh git

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
"@
Set-Content -Path $PROFILE -Value $ProfileContent -Encoding UTF8

Write-Host "✨ 설정이 완전히 끝났습니다! 파워쉘을 껐다가 다시 켜보세요." -ForegroundColor Green

