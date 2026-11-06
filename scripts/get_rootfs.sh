#!/usr/bin/env bash
# Downloads and unpacks the Alpine mini root filesystem into ./rootfs/alpine
# (no root needed). Override: ALPINE_VERSION=3.20.3 BRANCH=v3.20 DEST=/some/dir
set -euo pipefail

ALPINE_VERSION="${ALPINE_VERSION:-3.20.3}"
BRANCH="${BRANCH:-v${ALPINE_VERSION%.*}}"
DEST="${DEST:-$(cd "$(dirname "$0")/.." && pwd)/rootfs/alpine}"

case "$(uname -m)" in
  x86_64)        ARCH=x86_64 ;;
  aarch64|arm64) ARCH=aarch64 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

FILE="alpine-minirootfs-${ALPINE_VERSION}-${ARCH}.tar.gz"
URL="https://dl-cdn.alpinelinux.org/alpine/${BRANCH}/releases/${ARCH}/${FILE}"

if [ -x "$DEST/bin/busybox" ]; then
  echo "rootfs already present: $DEST"
  exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "downloading $URL"
curl -fsSL "$URL" -o "$TMP/$FILE"

# verify the checksum when Alpine publishes one
if curl -fsSL "$URL.sha256" -o "$TMP/$FILE.sha256" 2>/dev/null; then
  (cd "$TMP" && sha256sum -c "$FILE.sha256")
fi

mkdir -p "$DEST"
tar -xzf "$TMP/$FILE" -C "$DEST" --no-same-owner
echo "rootfs ready: $DEST"
