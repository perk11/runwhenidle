FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG HOST_UID


RUN set -ex \
    && apt-get update \
    && apt-get install -y --no-install-recommends  \
    build-essential \
    fakeroot \
    dpkg-dev \
    libxss-dev \
    git \
    && apt-get clean \
    && rm -rf /tmp/* /var/tmp/*

RUN useradd build -u $HOST_UID \
    && mkdir -p /home/build \
    && chown build:build /home/build