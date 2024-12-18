ARG DISTRO_BASE=archlinux
ARG DISTRO_TAG=base-devel

FROM $DISTRO_BASE:$DISTRO_TAG

ENV LANG=C.UTF-8
ENV LC_ALL C.UTF-8

RUN useradd builder && \
    pacman -Syu --needed --noconfirm --noprogressbar \
        bc cpio git pahole python rsync && \
    git config --system --add safe.directory '*' && \
    rm /var/cache/pacman/pkg/*
