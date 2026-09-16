# syntax=docker/dockerfile:1
#
# The environment owl builds and tests in. Every CI build job runs in this
# image, and a contributor gets the same environment with:
#
#   docker run --rm -it -v "$PWD:/src" -w /src ghcr.io/mradkhambek/owl-dev
#
# Two things owl needs are absent from Ubuntu's archive at the required
# versions and are fetched here: CMake 4.3 (apt has 3.28) and libh2o-evloop
# from h2o source (apt has 2.2; owl targets 2.3).
#
# The compiler is GCC 14. Ubuntu's clang 18 cannot use libstdc++'s <expected>
# -- it reports __cpp_concepts below what the header demands -- and libc++ 18
# has no floating-point from_chars. That is why CI has no compiler matrix.

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

# postgresql-client and redis-tools are here for waiting on service
# containers; python3 runs examples/rest/monkey.py.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git pkg-config ninja-build python3 \
        gcc-14 g++-14 \
        libssl-dev zlib1g-dev libuv1-dev libpq-dev libhiredis-dev libwslay-dev \
        libsqlite3-dev \
        postgresql-client redis-tools \
    && rm -rf /var/lib/apt/lists/*

ARG CMAKE_VERSION=4.3.0
RUN arch="$(uname -m)" \
    && curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz" \
       | tar -xz -C /opt \
    && ln -s "/opt/cmake-${CMAKE_VERSION}-linux-${arch}/bin/"* /usr/local/bin/

ENV CC=gcc-14 CXX=g++-14

# Pinned to a commit, not master. Tracking master means an upstream commit can
# break contributor pull requests with no change in this repository. The
# dev-image workflow's weekly run reports when master has moved past this pin;
# bumping it stays a deliberate act.
ARG H2O_REF=cac7e6568ad98a848f099ecd0a18b881f632479a
RUN git init /tmp/h2o \
    && git -C /tmp/h2o remote add origin https://github.com/h2o/h2o.git \
    && git -C /tmp/h2o fetch --depth 1 origin "${H2O_REF}" \
    && git -C /tmp/h2o checkout FETCH_HEAD \
    && git -C /tmp/h2o submodule update --init --recursive --depth 1 \
    && cmake -S /tmp/h2o -B /tmp/h2o/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DWITH_MRUBY=OFF \
    && cmake --build /tmp/h2o/build \
    && cmake --install /tmp/h2o/build \
    && rm -rf /tmp/h2o

# Ubuntu's libwslay-dev ships a cmake config, not libwslay.pc; owl looks it up
# through pkg-config under Homebrew's name.
RUN mkdir -p /usr/share/pkgconfig \
    && printf '%s\n' \
        'Name: libwslay' \
        'Description: WebSocket library' \
        'Version: 1.1.1' \
        'Libs: -lwslay' \
        'Cflags:' \
        > /usr/share/pkgconfig/libwslay.pc

WORKDIR /src
