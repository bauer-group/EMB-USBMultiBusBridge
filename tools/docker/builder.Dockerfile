# SPDX-License-Identifier: Apache-2.0
#
# Single-image build environment for the entire RPBridge tree:
#   - firmware  (Cortex-M33 / Pico-SDK 2.2.0 / ARM GNU Toolchain)
#   - driver    (Rust-for-Linux out-of-tree against kernel 6.18+)
#   - userspace (Rust workspace, x86_64 native + aarch64 cross)
#
# Distribution choice: Ubuntu 26.04 LTS.
#
#   - Ships kernel 6.18 with CONFIG_RUST=y in the default HWE flavour,
#     so the headers package alone is enough to build the Rust kernel
#     module. Debian Trixie ships 6.12 by default and only offers 6.18
#     via backports — more apt-pin gymnastics, more drift surface.
#
# Reproducibility model: floating apt versions, deliberately. Every
# build pulls "what's current" from the Ubuntu archive, so a local
# build and a CI build started in the same time window yield identical
# binaries. Cross-time byte-equality is not a goal; the trade-off
# was made consciously (see commit message + tools/docker/README.md).
#
# Build context is tools/docker/ — kept small via .dockerignore so the
# image build doesn't waste minutes uploading firmware/build/ etc.

FROM ubuntu:26.04

ARG DEBIAN_FRONTEND=noninteractive
ARG TZ=UTC

# ─── system layer ────────────────────────────────────────────────────────
# Single apt invocation — fewer layers, fewer caches on disk, cleaner
# image. Order: base utilities → firmware → driver → userspace cross.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git pkg-config build-essential file \
        sudo locales tzdata \
    && \
    # Firmware: Pico-SDK 2.2.0 needs ARM GNU Toolchain + cmake ≥ 3.28
    # + ninja. Ubuntu 26.04 ships gcc-arm-none-eabi 13.x which matches
    # what Pico-SDK 2.2.0 is tested against.
    apt-get install -y --no-install-recommends \
        gcc-arm-none-eabi libnewlib-arm-none-eabi \
        libstdc++-arm-none-eabi-newlib \
        cmake ninja-build python3 python3-pip libusb-1.0-0-dev \
    && \
    # Driver: Linux 6.18 headers + matching distro rustc (Canonical
    # ships rustc paired to the kernel's min-tool-version pin) + the
    # bindgen / clang stack needed for kernel Rust. rustfmt + clippy
    # come from rustup further down — Ubuntu 26.04 doesn't package
    # them as standalone apt names anymore.
    apt-get install -y --no-install-recommends \
        linux-headers-generic \
        rustc rust-src cargo bindgen \
        clang lld llvm libelf-dev libssl-dev bc kmod cpio flex bison \
    && \
    # Userspace: libudev/libusb (rusb / libudev-rs FFI) + aarch64 cross
    # linker for the cross-build matrix. The aarch64 std-lib comes from
    # rustup further down — apt doesn't ship rust-std cross targets.
    apt-get install -y --no-install-recommends \
        libudev-dev \
        gcc-aarch64-linux-gnu libc6-dev-arm64-cross \
    && \
    # QEMU + KUnit harness for the driver test gate (M12 in driver/TODO.md).
    apt-get install -y --no-install-recommends \
        qemu-system-x86 qemu-system-arm \
    && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

# ─── userspace rust toolchain (rustup) ───────────────────────────────────
# The distro's rustc handles the kernel module (it is the version the
# Ubuntu kernel team ships against). Userspace cross-compilation to
# aarch64 needs rust-std for that target — which apt doesn't package
# separately — so we install rustup with both targets pre-fetched.
#
# Lives under /opt so the container can run as any uid:gid and still
# read the toolchain. CARGO_HOME and RUSTUP_HOME are world-rwx so the
# bind-mounted source tree's cargo cache (under target/) writes cleanly.
ENV RUSTUP_HOME=/opt/rustup \
    CARGO_HOME=/opt/cargo \
    RUSTUP_TOOLCHAIN=stable
RUN mkdir -p "$RUSTUP_HOME" "$CARGO_HOME" \
    && curl --proto '=https' --tlsv1.2 -fsSL https://sh.rustup.rs \
       | bash -s -- -y --no-modify-path --profile minimal \
                    --default-toolchain stable \
                    --target x86_64-unknown-linux-gnu \
                    --target aarch64-unknown-linux-gnu \
    && "$CARGO_HOME/bin/rustup" component add rustfmt clippy \
    && chmod -R a+rwX "$RUSTUP_HOME" "$CARGO_HOME"

# Cargo + linker config so cross-builds Just Work without the workspace
# needing its own .cargo/config.toml. Userspace builds inherit this.
ENV CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc

# ─── PATH wiring ──────────────────────────────────────────────────────────
# /opt/cargo/bin must come BEFORE /usr/bin so userspace `cargo` resolves
# to rustup's cargo (which knows about installed targets). The kernel
# build invokes `rustc` directly via its own Makefile, which does not
# go through cargo and is unaffected by this ordering.
#
# Driver builds intentionally bypass /opt/cargo/bin via tools/build.sh
# so they pick up the distro's rustc that the kernel was packaged with.
ENV PATH="/opt/cargo/bin:${PATH}"

# pico-sdk path — overridden by tools/build.sh when the submodule is
# at a different location (it normally is, under firmware/third_party).
ENV PICO_SDK_PATH=/workspace/firmware/third_party/pico-sdk

WORKDIR /workspace

# Default to a shell so `docker run -it ... builder` is a usable dev
# environment when build.sh is bypassed. tools/build.sh always passes
# explicit commands.
CMD ["/bin/bash"]
