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
  # Prefer the openclaw CLI: it records provenance so `openclaw plugins
  # update` and `openclaw plugins list` see the plugin as tracked (not
  # "loaded without install/load-path provenance"). We fall back to a raw
  # copy into ~/.openclaw/extensions/beagle for two reasons:
  #   1. openclaw CLI isn't installed (bootstrap host, CI).
  #   2. The CLI install fails. Known failure as of openclaw 2026.4.2:
  #      `plugins install` blocks the compiled bundle with "Environment
  #      variable access combined with network send" — a false positive
  #      from the HOME lookup in resolveLocalMediaPath + the HTTP calls
  #      to the local sidecar. The documented
  #      `--dangerously-force-unsafe-install` flag does not actually
  #      bypass the check in that version. Raw-cp still produces a
  #      working install; the only loss is provenance + plugins-update
  #      tracking, which is worth less than a working channel.
  if command -v openclaw >/dev/null 2>&1; then
    # `plugins inspect beagle` succeeds even for an auto-discovered copy
    # under ~/.openclaw/extensions/beagle (no install record). We only
    # want to skip CLI re-install if there's a real tracked install —
    # the presence of an `Install path:` line distinguishes the two.
    if openclaw plugins inspect beagle 2>/dev/null | grep -q '^Install path:'; then
      log "openclaw plugin 'beagle' already tracked; skipping re-install"
      log "  (to relink: openclaw plugins uninstall beagle && rerun this script)"
      return
    fi

    log "Trying: openclaw plugins install --link $CHANNEL_DIR"
    if openclaw plugins install --link --dangerously-force-unsafe-install "$CHANNEL_DIR"; then
      log "Registered via openclaw CLI"
      return
    fi

    log "openclaw CLI install failed (likely the dangerous-code false-"
    log "positive in openclaw ≤2026.4.2); falling back to raw copy"
  else
    log "openclaw CLI not found; falling back to raw copy into $EXT_DIR"
    log "  (install openclaw later and rerun this script to pick up"
    log "   provenance + enable 'openclaw plugins update')"
  fi

  mkdir -p "$EXT_DIR"

  cp "$CHANNEL_DIR/package.json" "$EXT_DIR/"
  cp "$CHANNEL_DIR/index.js" "$EXT_DIR/"
  cp "$CHANNEL_DIR/openclaw.plugin.json" "$EXT_DIR/"

  rm -rf "$EXT_DIR/dist"
  cp -R "$CHANNEL_DIR/dist" "$EXT_DIR/"
}

restart_services() {
  # Only restart if systemd user units exist AND the caller opted in. Keeps
  # this script safe to run from a dev checkout without disturbing prod.
  if [[ "${BEAGLE_INSTALL_RESTART:-0}" != "1" ]]; then
    return
  fi
  if ! command -v systemctl >/dev/null 2>&1; then
    log "systemctl not available; skipping service restart"
    return
  fi

  for unit in beagle-sidecar.service openclaw-gateway.service; do
    if systemctl --user cat "$unit" >/dev/null 2>&1; then
      log "systemctl --user restart $unit"
      systemctl --user restart "$unit"
    fi
  done
}

main() {
  need_cmd cp

  build_sidecar
  build_channel
  install_channel_plugin
  restart_services

  log "Done"
  log "Sidecar binary: $SIDECAR_DIR/build/beagle-sidecar"
  log "Plugin dir: $EXT_DIR"
  log ""
  log "Next steps:"
  log "  - If not already registered, run: openclaw plugins install --link $CHANNEL_DIR"
  log "  - To enable scheduled upgrades, see: deploy/systemd/README.md"
}

main "$@"
