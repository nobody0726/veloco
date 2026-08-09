# Veloco development image, pinned to Ubuntu 24.04.
#
# Supported platforms: linux/amd64 and linux/arm64
#   docker build --platform linux/amd64 -f docker/dev.Dockerfile -t veloco-dev .
#   docker run --rm -it --platform linux/amd64 -v "$PWD:/workspace" -w /workspace veloco-dev
#   docker build --platform linux/arm64 -f docker/dev.Dockerfile -t veloco-dev:arm64 .
#
# Use native containers for benchmark data; emulated containers are
# correctness-only and should not be mixed with native baselines.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang \
    lld \
    cmake \
    ninja-build \
    liburing-dev \
    gdb \
    linux-tools-common \
    linux-tools-generic \
    valgrind \
    pkg-config \
    git \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# perf inside a container depends on host-kernel tooling; the generic
# entry point is installed and host tools can be bind-mounted if needed.
WORKDIR /workspace

CMD ["/bin/bash"]
