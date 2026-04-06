FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential gcc-i686-linux-gnu binutils-i686-linux-gnu \
    qemu-system-x86 perl python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
