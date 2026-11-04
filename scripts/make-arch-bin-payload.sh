#!/bin/sh
# Build the prebuilt aarch64 /usr tree and source archive for packaging/arch-bin.
# The CMake build directory lives in a persistent container so only changed
# sources are recompiled on later runs.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sed -n 's/^project(taiji_desktop VERSION \([0-9][0-9.]*\).*/\1/p' \
  "$ROOT/CMakeLists.txt")
[ -n "$VERSION" ] || VERSION=1.0.0
NAME="${TAIJI_PAYLOAD_NAME:-taiji-payload-env}"
IMAGE="${TAIJI_DOCKER_IMAGE:-taiji-build-env:bookworm}"
OUT="$ROOT/packaging/arch-bin/taiji-$VERSION-aarch64.tar.gz"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "Missing Docker image: $IMAGE" >&2
  echo "Run ./scripts/docker-linux-build.sh first." >&2
  exit 1
fi

if ! docker inspect "$NAME" >/dev/null 2>&1; then
  echo "Creating persistent payload container: $NAME"
  docker run -d --name "$NAME" --hostname "$NAME" \
    -v "$ROOT:/work" -w /work "$IMAGE" sleep infinity >/dev/null
else
  docker start "$NAME" >/dev/null 2>&1 || true
fi

# Recreate the container if the repository moved.
MOUNTED="$(docker inspect -f '{{range .Mounts}}{{.Destination}}={{.Source}}{{end}}' "$NAME" 2>/dev/null || true)"
case "$MOUNTED" in
  */work="$ROOT"|*work="$ROOT") ;;
  *work=*)
    echo "Source path changed; recreating $NAME"
    docker rm -f "$NAME" >/dev/null
    docker run -d --name "$NAME" --hostname "$NAME" \
      -v "$ROOT:/work" -w /work "$IMAGE" sleep infinity >/dev/null
    ;;
esac

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/taiji-bin.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT INT TERM
PKGROOT="$STAGE/taiji-$VERSION-aarch64"
mkdir -p "$PKGROOT"

docker exec "$NAME" sh -c '
  if ! command -v wayland-scanner >/dev/null 2>&1 ||
     [ ! -f /usr/include/X11/extensions/shape.h ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq libwayland-dev libx11-dev libxext-dev wayland-protocols libpng-dev libglib2.0-dev
  fi
' >/dev/null 2>&1 || true

docker exec -w /work "$NAME" sh -c '
  set -eu
  cmake -S /work -B /payload-build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DTAIJI_INSTALL_SDDM_THEME=ON \
    -DTAIJI_INSTALL_WAYLAND_SESSION=ON \
    -DTAIJI_INSTALL_X11_SESSION=OFF \
    -DTAIJI_BUILD_HOTKEYD=ON
  cmake --build /payload-build -j"$(nproc)"
  rm -rf /payload-stage
  mkdir -p /payload-stage/out
  DESTDIR=/payload-stage/out cmake --install /payload-build
'

docker cp "$NAME:/payload-stage/out/." "$PKGROOT" >/dev/null

copy_file() {
  mkdir -p "$(dirname "$2")"
  cp "$1" "$2"
  chmod 644 "$2"
}
copy_file "$ROOT/packaging/sddm/sddm.conf.d/taiji.conf" \
  "$PKGROOT/etc/sddm.conf.d/taiji.conf"
copy_file "$ROOT/packaging/systemd/logind.conf.d/taiji-power.conf" \
  "$PKGROOT/etc/systemd/logind.conf.d/taiji-power.conf"
copy_file "$ROOT/packaging/polkit/50-taiji-system.rules" \
  "$PKGROOT/usr/share/polkit-1/rules.d/50-taiji-system.rules"
copy_file "$ROOT/packaging/polkit/50-taiji-store.rules" \
  "$PKGROOT/usr/share/polkit-1/rules.d/50-taiji-store.rules"
copy_file "$ROOT/packaging/store/featured.tsv" \
  "$PKGROOT/usr/share/taiji/store/featured.tsv"
copy_file "$ROOT/LICENSE" \
  "$PKGROOT/usr/share/licenses/taiji/LICENSE"
copy_file "$ROOT/THIRD_PARTY.md" \
  "$PKGROOT/usr/share/licenses/taiji/THIRD_PARTY.md"
copy_file "$ROOT/assets/fonts/OFL.txt" \
  "$PKGROOT/usr/share/licenses/taiji/OFL.txt"
copy_file "$ROOT/third_party/lvgl/LICENCE.txt" \
  "$PKGROOT/usr/share/licenses/taiji/LVGL-LICENCE.txt"
copy_file "$ROOT/third_party/umtp-responder/LICENSE" \
  "$PKGROOT/usr/share/licenses/taiji/UMTP-LICENSE.txt"
copy_file "$ROOT/README.md" \
  "$PKGROOT/usr/share/doc/taiji/README.md"

rm -f "$OUT"
find "$STAGE" -name '._*' -delete
COPYFILE_DISABLE=1 tar -C "$STAGE" -czf "$OUT" "taiji-$VERSION-aarch64"

echo
echo "Created: $OUT"
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$OUT"
else
  shasum -a 256 "$OUT"
fi
echo
echo "Put the checksum into packaging/arch-bin/PKGBUILD and regenerate .SRCINFO."
