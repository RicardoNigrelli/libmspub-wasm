# syntax=docker/dockerfile:1
#
# Spike: libmspub -> WebAssembly.
# Reproducible build: emsdk pinned by version AND digest, sources pinned by
# version AND sha256 (downloaded from the official LibreOffice mirror).
#
#   docker build --output type=local,dest=dist .
#
# (or ./build.sh, which does the same)

ARG EMSDK_IMAGE=emscripten/emsdk:6.0.10@sha256:e077d54e2b8970575ebc4f185ac1de0b95c05f2b266134d4ba27449af7aebf65

############################################################################
# Stage 1: dependencies (emscripten ports + librevenge + libmspub)
############################################################################
FROM ${EMSDK_IMAGE} AS deps

ARG LIBREVENGE_VERSION=0.0.5
ARG LIBREVENGE_SHA256=5892ca6796f7a2a93d580832e907e849b19d980b40d326a283b18877ab6de0c5
ARG LIBMSPUB_VERSION=0.1.5
ARG LIBMSPUB_SHA256=3671095f5a10bee8a755052a30576952c5b16d8b0f2ba9f2fb998338c18cb119

# Every C++ object must use the same exception model: libmspub uses C++
# exceptions for normal control flow (EndOfStreamException), so exceptions
# cannot be disabled. We use native WebAssembly exceptions.
ENV EH_FLAGS="-fwasm-exceptions"
ENV PREFIX=/opt/pub

# 1) Emscripten ports: ICU 68.2 (with *stubdata*), boost headers, zlib.
RUN embuilder build icu boost_headers zlib

# 2) Sources, verified.
WORKDIR /src
RUN curl -fsSL -o librevenge.tar.bz2 \
      https://dev-www.libreoffice.org/src/librevenge-${LIBREVENGE_VERSION}.tar.bz2 \
 && echo "${LIBREVENGE_SHA256}  librevenge.tar.bz2" | sha256sum -c - \
 && curl -fsSL -o libmspub.tar.xz \
      https://dev-www.libreoffice.org/src/libmspub/libmspub-${LIBMSPUB_VERSION}.tar.xz \
 && echo "${LIBMSPUB_SHA256}  libmspub.tar.xz" | sha256sum -c - \
 && tar xjf librevenge.tar.bz2 && tar xJf libmspub.tar.xz

# Common flags. Ports are passed explicitly so configure tests link too.
ENV PORT_FLAGS="-sUSE_ZLIB=1 -sUSE_ICU=1 -sUSE_BOOST_HEADERS=1"

# 3) librevenge (core + stream + generators), static.
#    pkg-config is not in the image: PKG_CHECK_MODULES is satisfied with the
#    *_CFLAGS/*_LIBS variables.
WORKDIR /src/librevenge-${LIBREVENGE_VERSION}
RUN emconfigure ./configure --prefix=${PREFIX} \
      --disable-shared --enable-static \
      --disable-tests --without-docs --disable-werror --disable-weffc \
      --enable-streams --enable-generators \
      CFLAGS="-O3 ${EH_FLAGS}" CXXFLAGS="-O3 ${EH_FLAGS} ${PORT_FLAGS}" \
      LDFLAGS="${PORT_FLAGS} ${EH_FLAGS}" \
      ZLIB_CFLAGS=" " ZLIB_LIBS="-sUSE_ZLIB=1" \
 && emmake make -j"$(nproc)" \
 && emmake make install

# 4) libmspub, static, without the pub2* tools and fuzzers.
#    Our patches (patches/*.patch) are applied unless APPLY_PATCHES=0.
ARG APPLY_PATCHES=1
COPY patches/ /patches/
WORKDIR /src/libmspub-${LIBMSPUB_VERSION}
RUN if [ "${APPLY_PATCHES}" = "1" ]; then \
      for p in /patches/*.patch; do echo "applying $p"; git apply --verbose "$p"; done; \
    fi
RUN emconfigure ./configure --prefix=${PREFIX} \
      --disable-shared --enable-static \
      --disable-tools --disable-fuzzers --without-docs --disable-werror --disable-weffc \
      CFLAGS="-O3 ${EH_FLAGS}" CXXFLAGS="-O3 ${EH_FLAGS} ${PORT_FLAGS}" \
      LDFLAGS="${PORT_FLAGS} ${EH_FLAGS}" \
      REVENGE_CFLAGS="-I${PREFIX}/include/librevenge-0.0" \
      REVENGE_LIBS="-L${PREFIX}/lib -lrevenge-0.0" \
      ZLIB_CFLAGS=" " ZLIB_LIBS="-sUSE_ZLIB=1" \
      ICU_CFLAGS=" " ICU_LIBS="-sUSE_ICU=1" \
 && emmake make -j"$(nproc)" \
 && emmake make install

############################################################################
# Stage 1b: filtered ICU data (codepage converters only), see icu/
############################################################################
FROM deps AS icudata
# icupkg from Ubuntu 24.04 (ICU 74) only repackages the ICU 68 items, it
# does not regenerate them.
RUN apt-get update \
 && apt-get install -y --no-install-recommends icu-devtools=74.2-1ubuntu3.1 \
 && rm -rf /var/lib/apt/lists/*
COPY icu/make-icudata.sh /icu/make-icudata.sh
RUN bash /icu/make-icudata.sh /opt/icudata

############################################################################
# Stage 2: our binding (embind) + JSON generator, two optimization levels
############################################################################
FROM deps AS wasm
WORKDIR /binding
COPY --from=icudata /opt/icudata/icudata_subset.c ./
COPY src/ ./
RUN bash ./compile.sh /out

############################################################################
# Stage 3: export only the artifacts (docker build --output ...)
############################################################################
FROM scratch AS export
COPY --from=wasm /out/ /
