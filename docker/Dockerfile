# ============================================================================
# SN110 Open Firmware — ARM Cross-Compilation & Testing Environment
#
# Provides:
#   - ARM cross-compiler (arm-linux-gnueabi-gcc) targeting ARMv4T
#   - QEMU user-mode emulation for running ARM binaries on x86
#   - Same test suite runs on real ARM code paths
#
# Usage:
#   docker build -t sn110-toolchain -f docker/Dockerfile .
#   docker run --rm -v $(pwd):/project sn110-toolchain make arm-test
#   docker run --rm -it -v $(pwd):/project sn110-toolchain bash
# ============================================================================

FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc-arm-linux-gnueabi \
    binutils-arm-linux-gnueabi \
    libc6-dev-armel-cross \
    qemu-user-static \
    python3 \
    file \
    make \
    && rm -rf /var/lib/apt/lists/*

# Verify toolchain works
RUN arm-linux-gnueabi-gcc --version | head -1 && \
    qemu-arm-static --version | head -1 && \
    echo "=== SN110 Toolchain Ready ==="

WORKDIR /project
CMD ["make", "arm-test"]

