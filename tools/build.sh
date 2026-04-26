#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Single entrypoint for building any (or every) component of RPBridge,
# used identically by humans on a workstation and by GitHub Actions.
#
# Outside the builder container, this script transparently builds the
# image (cached layers make repeat invocations near-instant) and
# re-execs itself inside it. Inside the container, the same script
# delegates to each component's native build system.
#
# Usage:
#   tools/build.sh                            # firmware + driver + userspace
#   tools/build.sh firmware                   # both boards, Release
#   tools/build.sh firmware pico2             # one board
#   tools/build.sh driver                     # kernel module
#   tools/build.sh userspace                  # native (x86_64) build
#   tools/build.sh userspace --aarch64        # cross-build to aarch64
#   tools/build.sh shell                      # interactive shell in builder
#   tools/build.sh image                      # (re)build the image only
#   tools/build.sh clean                      # rm -rf dist + native caches
#
# Environment:
#   RPBRIDGE_NO_DOCKER=1   skip the container hop (use host toolchain)
#   RPBRIDGE_IMAGE_TAG=…   override the image tag (default: rpbridge-builder:local)

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_root"

IMAGE_TAG="${RPBRIDGE_IMAGE_TAG:-rpbridge-builder:local}"
DOCKERFILE="$repo_root/tools/docker/builder.Dockerfile"
DIST="$repo_root/dist"

# ─── helpers ──────────────────────────────────────────────────────────────

log() { printf '\033[1;36m[build.sh]\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31m[build.sh:error]\033[0m %s\n' "$*" >&2; exit 1; }

# Git Bash / MSYS / Cygwin on Windows auto-mangle forward-slash arguments
# into Windows install-prefix paths. Two pieces of magic together defeat
# this without breaking source-path handling:
#
#   1. Convert HOST paths via `cygpath -w` so docker.exe (the native
#      Windows binary, not a WSL one) sees C:\Projects\… and not the
#      MSYS-internal /c/Projects/… that it doesn't understand.
#   2. Use a LEADING DOUBLE SLASH for CONTAINER paths ("//workspace").
#      MSYS leaves those alone; Linux normalises // to /.
#
# On Linux/macOS, `cygpath` doesn't exist — fall through to the identity
# mapping. `--user` is also Linux-only: on Windows Docker Desktop the
# bind-mount layer maps ownership for us, and a Git-Bash-mocked uid
# breaks tools that look up the real /etc/passwd entry.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        repo_root_host=$(cygpath -w "$repo_root")
        WS_IN=//workspace
        USER_ARG=()
        ;;
    *)
        repo_root_host="$repo_root"
        WS_IN=/workspace
        USER_ARG=(--user "$(id -u):$(id -g)")
        ;;
esac

# `tools/build.sh image` (or implicit, before any subcommand) — produces
# the builder image. Subsequent `docker build` calls hit the layer
# cache and finish in under a second when nothing changed.
build_image() {
    log "building image $IMAGE_TAG"
    local ctx_host dockerfile_host
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            ctx_host=$(cygpath -w "$repo_root/tools/docker")
            dockerfile_host=$(cygpath -w "$DOCKERFILE")
            ;;
        *)
            ctx_host="$repo_root/tools/docker"
            dockerfile_host="$DOCKERFILE"
            ;;
    esac
    docker build \
        --tag "$IMAGE_TAG" \
        --file "$dockerfile_host" \
        "$ctx_host"
}

# Run a command inside the builder. Bind-mounts the entire repo at
# /workspace so artefacts land back on the host filesystem.
in_container() {
    local extra_env=()
    # SOURCE_DATE_EPOCH propagates the latest commit timestamp into
    # the build, which gcc + cargo honour for deterministic outputs.
    if git -C "$repo_root" rev-parse HEAD >/dev/null 2>&1; then
        extra_env+=("-e" "SOURCE_DATE_EPOCH=$(git -C "$repo_root" log -1 --format=%ct)")
    fi
    docker run --rm -i \
        "${USER_ARG[@]}" \
        -v "$repo_root_host:$WS_IN" \
        -w "$WS_IN" \
        -e RPBRIDGE_IN_CONTAINER=1 \
        -e CCACHE_DIR="$WS_IN/.ccache" \
        "${extra_env[@]}" \
        "$IMAGE_TAG" \
        bash -c "$*"
}

# Detect early if we should hop into the container.
need_container() {
    [[ "${RPBRIDGE_IN_CONTAINER:-0}" == "1" ]] && return 1   # already inside
    [[ "${RPBRIDGE_NO_DOCKER:-0}" == "1" ]]    && return 1   # opt-out
    return 0
}

# ─── component builds (run inside the container or natively) ──────────────

build_firmware() {
    local boards=("$@")
    if [[ ${#boards[@]} -eq 0 ]]; then
        boards=(rpbridge_rp2354b rpbridge_pico2)
    fi
    log "building firmware: ${boards[*]}"
    mkdir -p "$DIST/firmware"
    # CMake bakes absolute paths into its cache. A previous host-native
    # build leaves CMAKE_HOME_DIRECTORY pointing at C:/Projects/… which
    # the container (mounted at /workspace) refuses to re-use, aborting
    # before reconfigure. Blow the cache away only when this mismatch
    # is detected — incremental container-only builds keep their cache.
    if [[ -d firmware/build ]] && \
       grep -rqE 'CMAKE_HOME_DIRECTORY:[A-Z]+=([A-Za-z]:|/[^w])' \
            firmware/build 2>/dev/null; then
        log "stale cmake cache from a non-container build — clearing firmware/build/"
        rm -rf firmware/build
    fi
    # Defer to firmware/build-all-variants.sh — it already knows how
    # to map short names ("pico2") to PICO_BOARD values and runs the
    # CMake/Ninja loop. We just stage outputs into dist/.
    bash "$repo_root/firmware/build-all-variants.sh" "${boards[@]}"
    for variant in "${boards[@]}"; do
        local short="${variant#rpbridge_}"
        local src="$repo_root/firmware/build/$short"
        local dst="$DIST/firmware/$short"
        bash "$repo_root/firmware/scripts/size-guard.sh" "$variant"
        if [[ -d "$src" ]]; then
            mkdir -p "$dst"
            cp -f "$src"/rpbridge-"$short".{uf2,elf,bin,hex} "$dst/" 2>/dev/null || true
            cp -f "$src"/rpbridge-"$short".elf.map           "$dst/" 2>/dev/null || true
            cp -f "$src/size.txt"                            "$dst/" 2>/dev/null || true
            ( cd "$dst" && sha256sum rpbridge-"$short".{uf2,elf,bin,hex} > CHECKSUMS.txt 2>/dev/null || true )
        fi
    done
    log "firmware artefacts: $DIST/firmware/"
}

build_driver() {
    log "building driver (out-of-tree Rust kernel module)"
    # Driver build MUST use the distro rustc that the Ubuntu kernel
    # was packaged with — the Rust ABI is unstable and rustup's
    # version will mismatch the kernel's .rmeta files. /opt/cargo/bin
    # is suppressed from PATH for this invocation only.
    local kver
    kver=$(ls -1 /usr/src/ 2>/dev/null | grep -E '^linux-headers-[0-9]+\.[0-9]+\.[0-9]+' | sort -V | tail -1)
    if [[ -z "$kver" ]]; then
        die "no linux-headers found under /usr/src — image build broken?"
    fi
    log "driver: KDIR=/usr/src/$kver"
    mkdir -p "$DIST/driver"
    PATH="/usr/bin:/bin:/usr/sbin:/sbin" \
        make -C "$repo_root/driver" \
            KDIR="/usr/src/$kver" \
            modules
    cp -f "$repo_root/driver"/*.ko "$DIST/driver/" 2>/dev/null || \
        die "driver build produced no .ko"
    log "driver artefact: $DIST/driver/"
}

build_userspace() {
    local cross=""
    for arg in "$@"; do
        case "$arg" in
            --aarch64) cross="aarch64-unknown-linux-gnu" ;;
            --x86_64)  cross="x86_64-unknown-linux-gnu"  ;;
            *) die "unknown userspace flag: $arg" ;;
        esac
    done
    log "building userspace ${cross:+(target=$cross)}"
    mkdir -p "$DIST/userspace"
    if [[ -n "$cross" ]]; then
        ( cd userspace && cargo build --release --target "$cross" --workspace )
        cp -f "userspace/target/$cross/release/rpbridge-ctl" \
              "$DIST/userspace/rpbridge-ctl-$cross"
    else
        ( cd userspace && cargo build --release --workspace )
        cp -f "userspace/target/release/rpbridge-ctl" \
              "$DIST/userspace/rpbridge-ctl"
    fi
    log "userspace artefacts: $DIST/userspace/"
}

# Test runs — run inside container the same way as builds. Each
# component delegates to its native test runner.
test_userspace() {
    log "testing userspace (cargo test --workspace)"
    ( cd userspace && cargo fmt --all -- --check )
    ( cd userspace && cargo clippy --all-targets --all-features -- -D warnings )
    ( cd userspace && cargo test --workspace )
}

# ─── command dispatch ─────────────────────────────────────────────────────

cmd="${1:-all}"
shift || true

# Anything that needs the container hops first; the second invocation
# (this script, inside the container, with RPBRIDGE_IN_CONTAINER=1) lands
# in the same case and runs the native logic.
if need_container; then
    case "$cmd" in
        image)
            build_image
            exit 0
            ;;
        shell)
            build_image
            log "entering builder shell — exit to return to host"
            docker run --rm -it \
                "${USER_ARG[@]}" \
                -v "$repo_root_host:$WS_IN" \
                -w "$WS_IN" \
                -e RPBRIDGE_IN_CONTAINER=1 \
                "$IMAGE_TAG"
            exit 0
            ;;
        clean)
            log "removing dist/ and native build dirs"
            rm -rf "$DIST" "$repo_root/firmware/build" \
                   "$repo_root/userspace/target" "$repo_root/.ccache"
            exit 0
            ;;
        firmware|driver|userspace|test|all)
            build_image
            in_container "tools/build.sh $cmd $*"
            exit 0
            ;;
        -h|--help|help)
            sed -n '4,28p' "$0"
            exit 0
            ;;
        *)
            die "unknown command: $cmd (try: tools/build.sh help)"
            ;;
    esac
fi

# ─── inside the container (or RPBRIDGE_NO_DOCKER=1) ───────────────────────
case "$cmd" in
    firmware)  build_firmware "$@"  ;;
    driver)    build_driver         ;;
    userspace) build_userspace "$@" ;;
    test)      test_userspace       ;;
    all)
        build_firmware
        build_driver || log "driver build failed — kernel headers missing on host?"
        build_userspace
        ;;
    *) die "unknown component: $cmd" ;;
esac
