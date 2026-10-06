#!/bin/sh
# Fetch the pinned Pico SDK into third_party/pico-sdk (idempotent).
set -eu
TAG="${PICO_SDK_TAG:-2.3.1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/third_party/pico-sdk"

mkdir -p "$ROOT/third_party"
if [ ! -d "$DIR/.git" ]; then
    git clone --branch "$TAG" --depth 1 https://github.com/raspberrypi/pico-sdk "$DIR"
fi
cur="$(git -C "$DIR" describe --tags --exact-match 2>/dev/null || true)"
if [ "$cur" != "$TAG" ]; then
    git -C "$DIR" fetch --depth 1 origin "tag" "$TAG"
    git -C "$DIR" checkout "$TAG"
fi
git -C "$DIR" submodule update --init --depth 1
echo "pico-sdk $TAG ready at $DIR"
