FROM --platform=linux/amd64 luckfoxtech/luckfox_pico:1.0

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
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

WORKDIR /workspace/Echo-Mate

CMD ["/bin/bash"]
