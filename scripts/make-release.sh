#!/bin/sh
# Copyright (c) 2026 https://asp-lang.org
# SPDX-License-Identifier: MIT

set -eu

usage() {
    echo "Usage: $0 <linux|macos|all>" >&2
    exit 2
}

fail() {
    echo "make-release: $*" >&2
    exit 1
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
VERSION_HEADER="$ROOT/src/cmd/version.h"
TARGET=${1:-}

[ "$#" -eq 1 ] || usage
case "$TARGET" in
    linux|macos|all) ;;
    *) usage ;;
esac

VERSION=$(sed -n 's/^#define[[:space:]]*OPENASP_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' "$VERSION_HEADER")
[ -n "$VERSION" ] || fail "cannot read OPENASP_VERSION from $VERSION_HEADER"
case "$VERSION" in
    *[!0-9A-Za-z._-]*) fail "OPENASP_VERSION contains unsupported archive-name characters: $VERSION" ;;
esac

cpu_name() {
    case "$(uname -m)" in
        arm64|aarch64) echo "arm64" ;;
        x86_64|amd64) echo "x86-64" ;;
        *) fail "unsupported CPU architecture: $(uname -m)" ;;
    esac
}

package_local() {
    platform=$1
    expected_os=$2
    extension=$3
    current_os=$(uname -s)
    [ "$current_os" = "$expected_os" ] ||
        fail "$platform releases require $expected_os, current host is $current_os"

    cpu=$(cpu_name)
    archive_base="openasp-$VERSION-$platform-$cpu"
    work="$ROOT/build/release-work/$platform"
    stage_parent="$ROOT/build/release-stage"
    stage="$stage_parent/$archive_base"
    output="$ROOT/release/$archive_base.$extension"

    rm -rf "$work" "$stage"
    mkdir -p "$ROOT/release" "$stage/assets" "$stage/bin" "$stage/docs" \
        "$stage/examples/extensions/acme_math" "$stage/extensions/openasp-ai" \
        "$stage/include/openasp" "$stage/lib/openasp"

    make -C "$ROOT" BUILD="$work" clean
    make -C "$ROOT" BUILD="$work" AI=1 all

    for binary in openasp openasp-fpm openasp-cli openasp-dis openasp-dbg openasp-component-host; do
        [ -x "$work/bin/$binary" ] || fail "missing release binary: $work/bin/$binary"
        cp "$work/bin/$binary" "$stage/bin/$binary"
    done
    cp "$ROOT/README.md" "$stage/README.md"
    cp "$ROOT/README.zh-CN.md" "$stage/README.zh-CN.md"
    cp -R "$ROOT/assets/." "$stage/assets/"
    cp "$ROOT/docs/extensions.md" "$ROOT/docs/extensions.zh-CN.md" \
        "$ROOT/docs/openasp.ai-design.md" "$stage/docs/"
    cp "$ROOT/examples/openasp-fpm.yaml" "$stage/examples/openasp-fpm.yaml"
    cp "$ROOT/examples/extensions/acme_math/Makefile" \
        "$ROOT/examples/extensions/acme_math/acme_math.c" \
        "$ROOT/examples/extensions/acme_math/openasp-fpm.yaml" \
        "$stage/examples/extensions/acme_math/"
    cp -R "$ROOT/include/." "$stage/include/"
    cp "$ROOT/extensions/openasp-ai/include/openasp/ai.h" \
        "$ROOT/extensions/openasp-ai/include/openasp/ai_backend.h" \
        "$stage/include/openasp/"
    cp "$ROOT/extensions/openasp-ai/README.md" \
        "$ROOT/extensions/openasp-ai/README.zh-CN.md" \
        "$ROOT/extensions/openasp-ai/openasp-ai.yaml.example" \
        "$stage/extensions/openasp-ai/"
    if [ "$platform" = "linux" ]; then
        cp "$ROOT/extensions/openasp-ai/libopenasp_ai.so" "$stage/lib/openasp/"
    else
        cp "$ROOT/extensions/openasp-ai/libopenasp_ai.dylib" "$stage/lib/openasp/"
    fi

    rm -f "$output"
    if [ "$platform" = "linux" ]; then
        tar -C "$stage_parent" -czf "$output" "$archive_base"
    else
        (
            cd "$stage_parent"
            zip -qry "$output" "$archive_base"
        )
    fi
    rm -rf "$stage"
    echo "Created $output"
}

package_linux() {
    if [ "$(uname -s)" = "Linux" ] && [ "$(cpu_name)" = "x86-64" ]; then
        package_local linux Linux tar.gz
        return
    fi
    command -v docker >/dev/null 2>&1 || fail "docker is required to build Linux releases on $(uname -s)"
    docker info >/dev/null 2>&1 || fail "Docker daemon is unavailable"

    image=${OPENASP_LINUX_IMAGE:-ubuntu:24.04}
    linux_platform=${OPENASP_LINUX_PLATFORM:-linux/amd64}
    apt_mirror=${OPENASP_APT_MIRROR:-}
    case "$linux_platform" in
        linux/amd64|linux/arm64) ;;
        *) fail "unsupported OPENASP_LINUX_PLATFORM: $linux_platform" ;;
    esac
    docker image inspect "$image" >/dev/null 2>&1 ||
        fail "Linux build image '$image' is not available locally"
    host_uid=$(id -u)
    host_gid=$(id -g)
    docker run --rm --pull=never --platform "$linux_platform" \
        -e "HOST_UID=$host_uid" \
        -e "HOST_GID=$host_gid" \
        -e "APT_MIRROR=$apt_mirror" \
        -v "$ROOT:/src" \
        -w /src \
        "$image" \
        sh -eu -c '
            export DEBIAN_FRONTEND=noninteractive
            if [ -n "$APT_MIRROR" ]; then
                sed -i "s|^URIs: .*|URIs: $APT_MIRROR|" /etc/apt/sources.list.d/ubuntu.sources
            fi
            apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 \
                -o Acquire::https::Timeout=30 update
            apt-get -o Acquire::Retries=3 -o Acquire::http::Timeout=30 \
                -o Acquire::https::Timeout=30 install -y -qq --no-install-recommends \
                build-essential pkg-config zip \
                libpcre2-dev libxml2-dev libcurl4-openssl-dev libsqlite3-dev \
                libjson-c-dev libssl-dev libmariadb-dev mdbtools-dev \
                libglib2.0-dev gettext >/dev/null
            groupadd -o -g "$HOST_GID" openasp-build
            useradd -o -m -u "$HOST_UID" -g "$HOST_GID" openasp-build
            su openasp-build -s /bin/sh -c "scripts/make-release.sh linux"
        '
}

package_macos() {
    package_local macos Darwin zip
}

case "$TARGET" in
    linux) package_linux ;;
    macos) package_macos ;;
    all)
        package_macos
        package_linux
        ;;
esac
