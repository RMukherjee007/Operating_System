FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    clang \
    lld \
    llvm \
    make \
    python3 \
    qemu-system-arm \
    qemu-efi-aarch64 \
    grub-efi-arm64-bin \
    grub-common \
    xorriso \
    mtools \
    novnc \
    websockify \
    procps \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["/bin/bash"]
