ARG DISTRO_BASE=debian
ARG DISTRO_TAG=bookworm-slim

FROM $DISTRO_BASE:$DISTRO_TAG

ENV LANG=C.UTF-8
ENV LC_ALL C.UTF-8

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        bc binutils-aarch64-linux-gnu bison \
        ca-certificates \
        cpio curl dwarves flex \
        gcc gcc-aarch64-linux-gnu \
        git gettext kmod \
        libelf-dev libncurses-dev libssl-dev \
        make pkg-config python3 qemu-system-x86 \
        rsync unzip xz-utils zstd && \
    rm -rf /var/lib/apt/lists/*
