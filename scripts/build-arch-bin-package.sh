#!/bin/sh
# Build packaging/arch-bin/taiji-bin-*.pkg.tar.zst in a cached Arch Linux ARM
# container image. The image is prepared once, so later runs do not download
# base-devel/fakeroot again.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PDIR="$ROOT/packaging/arch-bin"
IMAGE="${TAIJI_PKGBUILD_IMAGE:-taiji-pkgbuild-env:archlinuxarm}"
BASE="${TAIJI_PKGBUILD_BASE:-menci/archlinuxarm:latest}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Preparing $IMAGE once (base-devel + fakeroot)..."
    NAME="taiji-pkgbuild-setup-$$"
    docker run --name "$NAME" --privileged --security-opt seccomp=unconfined \
        "$BASE" bash -lc '
        set -e
        pacman -Syu --noconfirm --needed base-devel fakeroot
        useradd -m builder
    '
    docker commit "$NAME" "$IMAGE" >/dev/null
    docker rm -f "$NAME" >/dev/null
    echo "Committed packaging image: $IMAGE"
fi

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/taiji-pkgbuild.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT INT TERM
cp -a "$PDIR/." "$STAGE/"
rm -f "$STAGE"/*.pkg.tar.zst
rm -rf "$STAGE/pkg" "$STAGE/src"

docker run --rm --privileged \
    -v "$STAGE:/src" \
    -v "$PDIR:/out" \
    "$IMAGE" bash -lc '
        set -e
        chown -R builder:builder /src
        su builder -c "cd /src && PKGEXT=.pkg.tar.zst makepkg --nodeps -f"
        cp /src/*.pkg.tar.zst /out/
    '

PKG=$(ls -1t "$PDIR"/taiji-bin-*.pkg.tar.zst | head -n 1)
echo
echo "Created: $PKG"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$PKG"
else
    shasum -a 256 "$PKG"
fi
