# Pre-flight build image for scripts/linux-check.sh (see the header of that script).
# BASE is pinned by digest by the script; the default here is Ubuntu 22.04 (CI's linux-x64 row).
ARG BASE=ubuntu:22.04
FROM ${BASE}

ENV DEBIAN_FRONTEND=noninteractive
COPY packages.txt /tmp/packages.txt

# Toolchain + test harness (xvfb for host_smoke) + the build dependencies from packages.txt.
# clang is only used by the sanitizer mode (the default Ubuntu 24.04 clang matches CI's job).
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential ninja-build ccache pkg-config rsync curl ca-certificates \
      xvfb xauth $(grep -v '^#' /tmp/packages.txt | tr '\n' ' ') \
 && rm -rf /var/lib/apt/lists/* /tmp/packages.txt

# Clang WITH recommends: that pulls in the sanitizer runtime (libclang-rt-*-dev), which the
# sanitizer mode needs.
RUN apt-get update && apt-get install -y clang && rm -rf /var/lib/apt/lists/*

# CMake >= 3.28 is required by the project; Ubuntu 22.04 ships 3.22. Kitware's tarball, SHA-256 checked.
ARG CMAKE_VERSION=3.31.9
ARG CMAKE_SHA256_AARCH64=09b7127b09e3d3ed5fa894f4e59f6a01112f267da6acc4249e46dba97b14afcb
ARG CMAKE_SHA256_X86_64=312d78e0b7c5c9b4a97a87a46c3e525aa84895fe8135c238c4616bba73dc9518
RUN set -eu; arch=$(uname -m); \
    case "$arch" in aarch64) sha=$CMAKE_SHA256_AARCH64;; x86_64) sha=$CMAKE_SHA256_X86_64;; *) exit 1;; esac; \
    f="cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz"; \
    curl -fsSL -o "/tmp/$f" "https://cmake.org/files/v3.31/$f"; \
    echo "$sha  /tmp/$f" | sha256sum -c -; \
    tar -xzf "/tmp/$f" -C /opt && ln -s "/opt/cmake-${CMAKE_VERSION}-linux-${arch}/bin/"* /usr/local/bin/ && rm "/tmp/$f"

LABEL vs.linux-check=1
WORKDIR /work
