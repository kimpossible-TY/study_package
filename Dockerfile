# .devcontainer/Dockerfile
FROM mcr.microsoft.com/devcontainers/base:debian

# 1. 필수 시스템 패키지 설치
RUN apt-get update && apt-get install -y \
    gawk make curl git lsof tmux fonts-noto-cjk \
    stow \
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