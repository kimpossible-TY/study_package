# .devcontainer/Dockerfile
FROM mcr.microsoft.com/devcontainers/base:debian

# 1. 필수 시스템 패키지 설치
RUN apt-get update && apt-get install -y \
    gawk make curl git lsof tmux fonts-noto-cjk \
    stow cmake gnupg ripgrep tar unzip fd-find ca-certificates nodejs \
    && apt-get clean

# 2. Typst 설치
RUN curl -L https://github.com/typst/typst/releases/latest/download/typst-x86_64-unknown-linux-musl.tar.xz | tar -xJ -C /tmp --strip-components=1 \
    && mv /tmp/typst /usr/local/bin/

# 3. ble.sh 빌드 및 설치 (시스템 전역 경로에 설치 권장)
RUN git clone --recursive --depth 1 https://github.com/akinomyoga/ble.sh.git /tmp/ble.sh-src \
    && make -C /tmp/ble.sh-src install PREFIX=/usr/local \
    && rm -rf /tmp/ble.sh-src

RUN echo '[[ $- == *i* ]] && source /usr/local/share/blesh/ble.sh' >> /etc/bash.bashrc

# 4. uv 설치
ENV UV_INSTALL_DIR="/usr/local/bin"
RUN curl -LsSf https://astral.sh/uv/install.sh | sh

# 5. antigravity CLI 설치
RUN curl -fsSL https://antigravity.google/cli/install.sh | bash \
    && mv /root/.local/bin/agy /usr/local/bin/  # 시스템 전역에서 사용할 수 있도록 경로 이동

# 6. Neovim 설치
RUN curl -LO https://github.com/neovim/neovim/releases/latest/download/nvim-linux-x86_64.tar.gz \
    && rm -rf /opt/nvim-linux-x86_64 \
    && tar -C /usr/local --strip-components=1 -xzf nvim-linux-x86_64.tar.gz \
    && rm nvim-linux-x86_64.tar.gz

# 7. 구체적인 CLI 명령어 링크 처리 (fd-find 예외 처리)
RUN ln -sf $(which fdfind) /usr/local/bin/fd

# 8. Node.js 설치 (Mason.nvim 및 백엔드 런타임용 필수)
RUN curl -fsSL https://deb.nodesource.com/setup_lts.x | bash - \
    && apt-get install -y nodejs \
    && apt-get clean && rm -rf /var/lib/apt/lists/*
