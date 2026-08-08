FROM luckfoxtech/luckfox_pico:1.0

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        curl \
        git-lfs \
        libboost-all-dev \
        libcurl4-openssl-dev \
        libdrm-dev \
        libjson-c-dev \
        libjsoncpp-dev \
        libopenblas-dev \
        libopus-dev \
        libsdl2-dev \
        libsdl2-image-dev \
        libwebsocketpp-dev \
        ninja-build \
        portaudio19-dev \
        repo \
    && git lfs install --system \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# 容器内所有 wget 走宿主代理（host.docker.internal 由 Docker Desktop 提供）
# 代理端口不可用时保持直连，避免影响离线场景
RUN printf "use_proxy = yes\nhttp_proxy = http://host.docker.internal:7897\nhttps_proxy = http://host.docker.internal:7897\n" >> /etc/wgetrc

WORKDIR /workspace/Echo-Mate

CMD ["/bin/bash"]
