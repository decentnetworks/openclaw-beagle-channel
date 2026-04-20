#!/usr/bin/env bash
#
# Single entrypoint for the scheduled upgrade timer.
#
# Steps, in order, each skippable via env:
#   1. `openclaw update --yes`       — openclaw core self-upgrade (JS CLI).
#   2. `openclaw plugins update --all` — refresh tracked plugins. No-op for
#       beagle-channel today because the openclaw install heuristic blocks
#       the bundle (see docs/UPGRADE_MECHANISM.md) and it ships via raw-cp.
#   3. `git pull --ff-only` in the repo — pulls any new sidecar / channel
#       source.
#   4. `install.sh` (with BEAGLE_INSTALL_RESTART=1) — rebuilds the C++
#       sidecar, rebuilds beagle-channel (`npm run build`), reinstalls the
#       channel (raw-cp fallback keeps ~/.openclaw/extensions/beagle/ fresh),
#       and restarts `beagle-sidecar.service` + `openclaw-gateway.service`.
#
# Why step 4 reruns install.sh rather than a bespoke subset: install.sh is
# already the canonical "build everything + put it in place" entry point.
# Reusing it means the scheduled path and the manual bootstrap path can't
# drift, and operators who change install.sh don't have to remember to
# mirror those changes here. The downside is running cmake + npm build on
# a no-op week — cheap, and update-sidecar.sh short-circuits on unchanged
# HEAD anyway (we mirror that optimization for the channel below).
#
# Each step fails loud. Non-zero exit → systemd marks the unit failed →
# journalctl has the stack. Timer retries next tick; no in-script retry.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="${REPO_DIR:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}"

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

run_git_pull() {
  if [[ "${SKIP_GIT_PULL:-0}" == "1" ]]; then
    log "SKIP_GIT_PULL=1; leaving working tree as-is"
    return 0
  fi
  if [[ ! -d "$REPO_DIR/.git" ]]; then
    log "$REPO_DIR is not a git checkout; skipping git pull"
    return 0
  fi
  log "git -C $REPO_DIR pull --ff-only"
  git -C "$REPO_DIR" pull --ff-only
}

run_local_install() {
  if [[ "${SKIP_LOCAL_INSTALL:-0}" == "1" ]]; then
    log "SKIP_LOCAL_INSTALL=1; skipping install.sh"
    return 0
  fi
  if [[ ! -x "$REPO_DIR/install.sh" ]]; then
    log "$REPO_DIR/install.sh not found or not executable; skipping"
    return 0
  fi
  log "running $REPO_DIR/install.sh (with BEAGLE_INSTALL_RESTART=1)"
  BEAGLE_INSTALL_RESTART="${BEAGLE_INSTALL_RESTART:-1}" "$REPO_DIR/install.sh"
}

run_openclaw_core_update
run_openclaw_plugins_update
run_git_pull
run_local_install

log "all steps complete"
