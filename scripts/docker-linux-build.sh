#!/bin/sh
# Build the Linux target in a persistent Docker container.
#
# The first run installs the Debian build dependencies once, commits them into
# taiji-build-env:bookworm, and keeps taiji-build-env around. Later runs reuse
# both the apt packages and the CMake object cache.
set -eu

NAME="${TAIJI_DOCKER_NAME:-taiji-build-env}"
IMAGE="${TAIJI_DOCKER_IMAGE:-taiji-build-env:bookworm}"
BASE="${TAIJI_DOCKER_BASE:-debian:bookworm-slim}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="/build"

if ! docker inspect "$NAME" >/dev/null 2>&1; then
    if docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo "creating container $NAME from $IMAGE"
        docker run -d --name "$NAME" --hostname "$NAME" \
            -v "$ROOT:/work" -w /work "$IMAGE" sleep infinity >/dev/null
    else
        echo "creating container $NAME and installing build dependencies (one time)"
        docker run -d --name "$NAME" --hostname "$NAME" \
            -v "$ROOT:/work" -w /work "$BASE" sleep infinity >/dev/null
        docker exec "$NAME" sh -c '
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq
            apt-get install -y -qq build-essential cmake pkg-config libsdl2-dev librsvg2-bin \
                libwayland-dev libx11-dev libxext-dev wayland-protocols libpng-dev libglib2.0-dev
        '
        docker commit "$NAME" "$IMAGE" >/dev/null
        echo "committed build image: $IMAGE"
    fi
else
    docker start "$NAME" >/dev/null 2>&1 || true
fi

# If the source was moved, refresh the bind mount path by recreating the
# container from the already-prepared image.
MOUNTED="$(docker inspect -f '{{range .Mounts}}{{.Destination}}={{.Source}}{{end}}' "$NAME" 2>/dev/null || true)"
case "$MOUNTED" in
    */work="$ROOT"|*work="$ROOT") ;;
    *work=*)
        echo "source path changed; recreating $NAME from $IMAGE"
        docker rm -f "$NAME" >/dev/null
        docker run -d --name "$NAME" --hostname "$NAME" \
            -v "$ROOT:/work" -w /work "$IMAGE" sleep infinity >/dev/null
        ;;
esac

docker exec "$NAME" sh -c '
  if ! command -v wayland-scanner >/dev/null 2>&1 ||
     [ ! -f /usr/include/X11/extensions/shape.h ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq libwayland-dev libx11-dev libxext-dev wayland-protocols libpng-dev libglib2.0-dev
  fi
' >/dev/null 2>&1 || true

docker exec -w /work "$NAME" sh -c "cmake -S /work -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR -j\$(nproc)"

mkdir -p "$ROOT/build"
docker cp "$NAME:$BUILD_DIR/taiji" "$ROOT/build/taiji-linux" >/dev/null
if docker cp "$NAME:$BUILD_DIR/taiji-hotkeyd" "$ROOT/build/taiji-hotkeyd" >/dev/null 2>&1; then
    :
fi
if docker cp "$NAME:$BUILD_DIR/taiji-fbcap" "$ROOT/build/taiji-fbcap" >/dev/null 2>&1; then
    :
fi
if docker cp "$NAME:$BUILD_DIR/taiji-osd" "$ROOT/build/taiji-osd" >/dev/null 2>&1; then
    :
fi
if docker cp "$NAME:$BUILD_DIR/taiji-files" "$ROOT/build/taiji-files" >/dev/null 2>&1; then
    :
fi
if docker cp "$NAME:$BUILD_DIR/taiji-files-portal" "$ROOT/build/taiji-files-portal" >/dev/null 2>&1; then
    :
fi
echo "Linux build: $ROOT/build/taiji-linux"
echo "Container:  $NAME (reuse with: docker exec -w /work $NAME sh)"
