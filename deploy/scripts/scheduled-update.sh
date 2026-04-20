#!/usr/bin/env bash
#
# Single entrypoint for the scheduled upgrade timer.
#
# Three steps, in order:
#   1. `openclaw update --yes`       — openclaw core self-upgrade (JS CLI).
#   2. `openclaw plugins update --all` — refresh tracked plugins, including
#       beagle-channel (assuming it was installed via `openclaw plugins
#       install --link` — see install.sh).
#   3. `update-sidecar.sh`            — git pull + cmake rebuild + restart
#       beagle-sidecar. `openclaw update` doesn't know about the C++ sidecar.
#
# Each step is skippable via env var so operators can degrade gracefully if
# one subsystem isn't installed on this host. Non-zero exit from any
# non-skipped step propagates (systemd will mark the unit failed and journal
# will have the details).

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

log() {
  printf '[scheduled-update] %s\n' "$*"
}

run_openclaw_core_update() {
  if [[ "${SKIP_OPENCLAW_UPDATE:-0}" == "1" ]]; then
    log "SKIP_OPENCLAW_UPDATE=1; skipping openclaw core update"
    return 0
  fi
  if ! command -v openclaw >/dev/null 2>&1; then
    log "openclaw CLI not found; skipping core update"
    return 0
  fi
  log "openclaw update --yes"
  openclaw update --yes
}

run_openclaw_plugins_update() {
  if [[ "${SKIP_PLUGINS_UPDATE:-0}" == "1" ]]; then
    log "SKIP_PLUGINS_UPDATE=1; skipping openclaw plugins update"
    return 0
  fi
  if ! command -v openclaw >/dev/null 2>&1; then
    log "openclaw CLI not found; skipping plugins update"
    return 0
  fi
  log "openclaw plugins update --all"
  openclaw plugins update --all
}

run_sidecar_update() {
  if [[ "${SKIP_SIDECAR_UPDATE:-0}" == "1" ]]; then
    log "SKIP_SIDECAR_UPDATE=1; skipping sidecar update"
    return 0
  fi
  log "invoking update-sidecar.sh"
  "$SCRIPT_DIR/update-sidecar.sh"
}

run_openclaw_core_update
run_openclaw_plugins_update
run_sidecar_update

log "all steps complete"
