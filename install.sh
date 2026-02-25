#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SIDECAR_DIR="$ROOT_DIR/packages/beagle-sidecar"
CHANNEL_DIR="$ROOT_DIR/packages/beagle-channel"
EXT_DIR="${OPENCLAW_EXT_DIR:-$HOME/.openclaw/extensions/beagle}"

log() {
  printf '[install] %s\n' "$*"
}

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'Missing required command: %s\n' "$1" >&2
    exit 1
  fi
}

build_sidecar() {
  need_cmd cmake

  local stub_flag="ON"
  local extra=()

  if [[ -n "${BEAGLE_SDK_ROOT:-}" ]]; then
    stub_flag="OFF"
    extra+=("-DBEAGLE_SDK_ROOT=${BEAGLE_SDK_ROOT}")
    if [[ -n "${BEAGLE_SDK_BUILD_DIR:-}" ]]; then
      extra+=("-DBEAGLE_SDK_BUILD_DIR=${BEAGLE_SDK_BUILD_DIR}")
    fi
    log "Building beagle-sidecar (real SDK mode)"
  else
    log "BEAGLE_SDK_ROOT not set; building beagle-sidecar in stub mode"
  fi

  cmake -S "$SIDECAR_DIR" -B "$SIDECAR_DIR/build" -DBEAGLE_SDK_STUB="$stub_flag" "${extra[@]}"
  cmake --build "$SIDECAR_DIR/build"
}

build_channel() {
  need_cmd npm

  log "Installing beagle-channel dependencies"
  (
    cd "$CHANNEL_DIR"
    npm install
    npm run build
  )
}

install_channel_plugin() {
  log "Installing beagle-channel plugin to $EXT_DIR"
  mkdir -p "$EXT_DIR"

  cp "$CHANNEL_DIR/package.json" "$EXT_DIR/"
  cp "$CHANNEL_DIR/index.js" "$EXT_DIR/"
  cp "$CHANNEL_DIR/openclaw.plugin.json" "$EXT_DIR/"

  rm -rf "$EXT_DIR/dist"
  cp -R "$CHANNEL_DIR/dist" "$EXT_DIR/"
}

main() {
  need_cmd cp

  build_sidecar
  build_channel
  install_channel_plugin

  log "Done"
  log "Sidecar binary: $SIDECAR_DIR/build/beagle-sidecar"
  log "Plugin dir: $EXT_DIR"
}

main "$@"
