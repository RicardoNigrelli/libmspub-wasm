# syntax=docker/dockerfile:1
#
# Diagnostic only: NATIVE (x86-64) libmspub 0.1.5 + pub2xhtml, to tell
# libmspub bugs apart from WebAssembly-port bugs.
#
#   docker build -t libmspub-native --build-arg APPLY_PATCHES=1 -f tools/native/Dockerfile .
#   docker run --rm -v <dir-with-pubs>:/in:ro libmspub-native pub2xhtml /in/file.pub
#
# With --build-arg DEBUG=1 libmspub prints its parse trace on stderr.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3

ARG LIBMSPUB_VERSION=0.1.5
ARG LIBMSPUB_SHA256=3671095f5a10bee8a755052a30576952c5b16d8b0f2ba9f2fb998338c18cb119
ARG APPLY_PATCHES=1
# DEBUG=1: --enable-debug (trace on stderr, -O0, much slower)
ARG DEBUG=0

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl g++ make pkg-config \
      libboost-dev libicu-dev zlib1g-dev librevenge-dev xz-utils patch \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN curl -fsSL -o libmspub.tar.xz \
      https://dev-www.libreoffice.org/src/libmspub/libmspub-${LIBMSPUB_VERSION}.tar.xz \
 && echo "${LIBMSPUB_SHA256}  libmspub.tar.xz" | sha256sum -c - \
 && tar xJf libmspub.tar.xz

COPY patches/ /patches/
WORKDIR /src/libmspub-${LIBMSPUB_VERSION}
RUN if [ "${APPLY_PATCHES}" = "1" ]; then for p in /patches/*.patch; do patch -p1 < "$p"; done; fi \
 && ./configure $([ "${DEBUG}" = "1" ] && echo --enable-debug) --disable-werror --without-docs \
 && make -j"$(nproc)" \
 && cp src/conv/svg/.libs/pub2xhtml src/conv/raw/.libs/pub2raw /usr/local/bin/ \
 && cp src/lib/.libs/libmspub-0.1.so* /usr/local/lib/ && ldconfig
