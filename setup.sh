#!/usr/bin/env bash
set -Eeuo pipefail

# CS422 TCP Congestion Control — One-command setup and run
# Target: Ubuntu 22.04+ on x86-64 Linux / WSL2

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

SRC_DIR="$ROOT_DIR/src"
BUILD_DIR="$ROOT_DIR/build"
DEPS_DIR="$ROOT_DIR/deps"
LIBBPF_SRC="$DEPS_DIR/libbpf"
LIBBPF_INSTALL="$DEPS_DIR/install"
BPFTOOL_SRC="$DEPS_DIR/bpftool"
BPFTOOL_BIN="$BPFTOOL_SRC/src/bpftool"

LOADER_PID=""
ORIGINAL_ALLOWED=""
CHANGED_ALLOWED=0

log() {
    printf '\n[CS422] %s\n' "$*"
}

fail() {
    printf '\n[ERROR] %s\n' "$*" >&2
    exit 1
}

cleanup() {
    trap - EXIT INT TERM

    if [[ "$CHANGED_ALLOWED" -eq 1 ]]; then
        log "Restoring allowed congestion-control algorithms..."
        # Restore only if our cs422 entry is still present.
        CURRENT=$(sysctl -n net.ipv4.tcp_allowed_congestion_control 2>/dev/null || true)
        if [[ " $CURRENT " == *" cs422 "* ]]; then
            sudo sysctl -q -w \
                "net.ipv4.tcp_allowed_congestion_control=$ORIGINAL_ALLOWED" \
                2>/dev/null || true
        fi
    fi

    if [[ -n "$LOADER_PID" ]]; then
        log "Unloading cs422..."
        sudo kill -INT "$LOADER_PID" 2>/dev/null || true
        wait "$LOADER_PID" 2>/dev/null || true
    fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# --------------------------------------------------
# 1. Environment checks
# --------------------------------------------------

log "Checking environment..."

[[ "$(uname -s)" == "Linux" ]] ||
    fail "Linux is required."

[[ -f /etc/os-release ]] ||
    fail "Cannot identify distribution."

source /etc/os-release

[[ "${ID:-}" == "ubuntu" ]] ||
    fail "Ubuntu is required for this script."

[[ "$(uname -m)" == "x86_64" ]] ||
    fail "Only x86-64 is currently supported."

[[ -r /sys/kernel/btf/vmlinux ]] ||
    fail "Kernel BTF unavailable. This kernel may not support the project."

[[ -f "$SRC_DIR/cs422.bpf.c" ]] ||
    fail "Missing src/cs422.bpf.c"

[[ -f "$SRC_DIR/loader.c" ]] ||
    fail "Missing src/loader.c"

log "OS: ${PRETTY_NAME:-Ubuntu}"
log "Kernel: $(uname -r)"

mkdir -p "$BUILD_DIR" "$DEPS_DIR" "$LIBBPF_INSTALL"

# --------------------------------------------------
# 2. Install dependencies
# --------------------------------------------------

log "Installing dependencies..."

sudo apt-get update

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential \
    clang \
    llvm \
    libelf-dev \
    zlib1g-dev \
    libssl-dev \
    libzstd-dev \
    flex \
    bison \
    pkg-config \
    git \
    make \
    iperf3 \
    iproute2 \
    libcap-dev

# --------------------------------------------------
# 3. Build libbpf
# --------------------------------------------------

log "Preparing libbpf..."

if [[ ! -d "$LIBBPF_SRC/.git" ]]; then
    git clone --depth 1 \
        https://github.com/libbpf/libbpf.git \
        "$LIBBPF_SRC"
fi

make -C "$LIBBPF_SRC/src" \
    BUILD_STATIC_ONLY=1 \
    OBJDIR="$DEPS_DIR/libbpf-obj" \
    DESTDIR="$LIBBPF_INSTALL" \
    PREFIX=/ \
    install

LIBBPF_INCLUDE="$LIBBPF_INSTALL/include"
LIBBPF_ARCHIVE="$LIBBPF_INSTALL/lib64/libbpf.a"

if [[ ! -f "$LIBBPF_ARCHIVE" ]]; then
    LIBBPF_ARCHIVE="$LIBBPF_INSTALL/lib/libbpf.a"
fi

[[ -f "$LIBBPF_ARCHIVE" ]] ||
    fail "libbpf build failed."

# --------------------------------------------------
# 4. Build bpftool
# --------------------------------------------------

log "Preparing bpftool..."

if [[ ! -d "$BPFTOOL_SRC/.git" ]]; then
    git clone --depth 1 --recurse-submodules \
        --shallow-submodules \
        https://github.com/libbpf/bpftool.git \
        "$BPFTOOL_SRC"
else
    git -C "$BPFTOOL_SRC" submodule update \
        --init --recursive
fi

if [[ ! -x "$BPFTOOL_BIN" ]]; then
    make -C "$BPFTOOL_SRC/src" -j"$(nproc)"
fi

[[ -x "$BPFTOOL_BIN" ]] ||
    fail "bpftool build failed."

"$BPFTOOL_BIN" version >/dev/null ||
    fail "bpftool could not execute."

# --------------------------------------------------
# 5. Generate kernel headers
# --------------------------------------------------

log "Generating vmlinux.h..."

"$BPFTOOL_BIN" btf dump \
    file /sys/kernel/btf/vmlinux \
    format c > "$SRC_DIR/vmlinux.h"

# --------------------------------------------------
# 6. Compile eBPF program
# --------------------------------------------------

log "Compiling eBPF program..."

CLANG_BIN="$(command -v clang)"

"$CLANG_BIN" \
    -O2 -g \
    -target bpf \
    -D__TARGET_ARCH_x86 \
    -I"$SRC_DIR" \
    -I"$LIBBPF_INCLUDE" \
    -c "$SRC_DIR/cs422.bpf.c" \
    -o "$BUILD_DIR/cs422.bpf.o"

# --------------------------------------------------
# 7. Compile loader
# --------------------------------------------------

log "Compiling loader..."

cc \
    -O2 -Wall -Wextra \
    -I"$LIBBPF_INCLUDE" \
    "$SRC_DIR/loader.c" \
    "$LIBBPF_ARCHIVE" \
    -lelf -lz \
    -o "$BUILD_DIR/loader"

# --------------------------------------------------
# 8. Register cs422
# --------------------------------------------------

log "Registering cs422..."

# Authenticate before backgrounding the loader.
sudo -v

sudo ./build/loader &
LOADER_PID=$!

REGISTERED=0

for i in {1..50}; do
    if ! kill -0 "$LOADER_PID" 2>/dev/null; then
        break
    fi

    if sysctl -n net.ipv4.tcp_available_congestion_control |
       grep -qw cs422; then
        REGISTERED=1
        break
    fi

    sleep 0.2
done

if [[ "$REGISTERED" -ne 1 ]]; then
    fail "cs422 did not register. Check the loader errors above."
fi

# --------------------------------------------------
# 9. Allow unprivileged applications to use cs422
# --------------------------------------------------

log "Configuring allowed congestion-control algorithms..."

ORIGINAL_ALLOWED="$(
    sysctl -n net.ipv4.tcp_allowed_congestion_control
)"

if ! grep -qw cs422 <<< "$ORIGINAL_ALLOWED"; then
    sudo sysctl -q -w \
        "net.ipv4.tcp_allowed_congestion_control=$ORIGINAL_ALLOWED cs422"

    CHANGED_ALLOWED=1
fi

# --------------------------------------------------
# 10. Ready
# --------------------------------------------------

printf '\n\r'
printf '========================================\n\r'
printf ' CS422 READY\n\r'
printf '========================================\n\r'
printf '\n\r'

printf '\rAvailable algorithms:\n'
sysctl -n net.ipv4.tcp_available_congestion_control

printf '\r\n\rAllowed algorithms:\n'
sysctl -n net.ipv4.tcp_allowed_congestion_control
printf '\r\n'

# Keep the loader alive.
wait "$LOADER_PID"
