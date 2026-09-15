# syntax=docker/dockerfile:1
#
# linux/arm64 benches for a machine that is not the build host. Context is
# the repository root:
#
#   docker build --platform linux/arm64 -f owl/bench/Dockerfile -o owl/bench/out .
#
# Same toolchain as examples/rest/Dockerfile: Ubuntu 24.04, GCC 14, CMake 4.3,
# libh2o-evloop from h2o master. sqlite is off; prometheus is on so the
# RED-instrumented binary is in the same drop as the plain one.

FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git pkg-config ninja-build \
        gcc-14 g++-14 \
        libssl-dev zlib1g-dev libwslay-dev \
    && rm -rf /var/lib/apt/lists/*

ARG CMAKE_VERSION=4.3.0
RUN arch="$(uname -m)" \
    && curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz" \
       | tar -xz -C /opt \
    && ln -s "/opt/cmake-${CMAKE_VERSION}-linux-${arch}/bin/"* /usr/local/bin/

ENV CC=gcc-14 CXX=g++-14

ARG H2O_REF=master
RUN git clone --depth 1 --branch "${H2O_REF}" --recurse-submodules --shallow-submodules https://github.com/h2o/h2o.git /tmp/h2o \
    && cmake -S /tmp/h2o -B /tmp/h2o/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DWITH_MRUBY=OFF \
    && cmake --build /tmp/h2o/build \
    && cmake --install /tmp/h2o/build \
    && echo /usr/local/lib > /etc/ld.so.conf.d/usr-local.conf \
    && ldconfig \
    && rm -rf /tmp/h2o

WORKDIR /src
COPY . /src
# Ubuntu's libwslay-dev ships a cmake config, not libwslay.pc. owl looks it
# up through pkg-config under Homebrew's name.
# No C++ modules here: scanning for them would need clang-scan-deps.
RUN mkdir -p /usr/share/pkgconfig \
    && printf '%s\n' \
        'Name: libwslay' \
        'Description: WebSocket library' \
        'Version: 1.1.1' \
        'Libs: -lwslay' \
        'Cflags:' \
        > /usr/share/pkgconfig/libwslay.pc \
    && cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DBUILD_TESTING=OFF \
        -DOWL_ENABLE_SQLITE=OFF \
        -DOWL_ENABLE_PROMETHEUS=ON \
    && cmake --build /build --target owl_bench raw_h2o_bench owl_bench_prometheus \
    && mkdir -p /opt/bench \
    && cp /build/owl/owl_bench /build/owl/raw_h2o_bench /build/owl/owl_bench_prometheus /opt/bench/ \
    && find /usr/local -name 'libh2o-evloop.so*' -exec cp -a {} /opt/bench/ \;

FROM scratch AS export
COPY --from=build /opt/bench/ /
