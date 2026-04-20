#!/usr/bin/env bash
#
# Rebuild and restart beagle-sidecar from a local git checkout.
#
# Why this exists: `openclaw update` (the built-in self-upgrade CLI) covers
# openclaw core and tracked JS plugins, but it does not know anything about
# the C++ sidecar. The sidecar is compiled from this repo, so its upgrade
# path has to be driven separately. This script is the "sidecar half" of the
# scheduled update; `openclaw update` is the "JS half".
#
# Idempotent and safe to re-run. Designed to be invoked from
# openclaw-beagle-channel-update.service on a systemd timer.
#
# Env:
#   REPO_DIR                 (default: the repo this script lives in)
#   BEAGLE_SDK_ROOT          (optional: build with real SDK; stub otherwise)
#   BEAGLE_SDK_BUILD_DIR     (optional, paired with BEAGLE_SDK_ROOT)
#   SIDECAR_SYSTEMD_UNIT     (default: beagle-sidecar.service)
#   SIDECAR_SYSTEMD_SCOPE    (default: --user; set to --system for root units)
#   SKIP_GIT_PULL            (set to 1 to skip `git pull` — useful in CI)
#   SKIP_RESTART             (set to 1 to rebuild without restarting)

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="${REPO_DIR:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}"
SIDECAR_DIR="$REPO_DIR/packages/beagle-sidecar"
SIDECAR_SYSTEMD_UNIT="${SIDECAR_SYSTEMD_UNIT:-beagle-sidecar.service}"
SIDECAR_SYSTEMD_SCOPE="${SIDECAR_SYSTEMD_SCOPE:---user}"

log() {
  printf '[update-sidecar] %s\n' "$*"
}

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf '[update-sidecar] missing required command: %s\n' "$1" >&2
    exit 1
  fi
}

need_cmd cmake
need_cmd git

if [[ ! -d "$SIDECAR_DIR" ]]; then
  log "sidecar source not found at $SIDECAR_DIR"
  exit 1
fi

HEAD_BEFORE="$(git -C "$REPO_DIR" rev-parse HEAD)"

if [[ "${SKIP_GIT_PULL:-0}" != "1" ]]; then
  log "git pull --ff-only in $REPO_DIR"
  git -C "$REPO_DIR" pull --ff-only
else
  log "SKIP_GIT_PULL=1; using working tree as-is"
fi

HEAD_AFTER="$(git -C "$REPO_DIR" rev-parse HEAD)"

if [[ "$HEAD_BEFORE" == "$HEAD_AFTER" && -x "$SIDECAR_DIR/build/beagle-sidecar" ]]; then
  log "already at $HEAD_AFTER and binary exists; nothing to do"
  exit 0
fi

stub_flag="ON"
extra=()
if [[ -n "${BEAGLE_SDK_ROOT:-}" ]]; then
  stub_flag="OFF"
  extra+=("-DBEAGLE_SDK_ROOT=${BEAGLE_SDK_ROOT}")
  if [[ -n "${BEAGLE_SDK_BUILD_DIR:-}" ]]; then
    extra+=("-DBEAGLE_SDK_BUILD_DIR=${BEAGLE_SDK_BUILD_DIR}")
  fi
  log "rebuilding sidecar (real SDK mode) at $HEAD_AFTER"
else
  log "rebuilding sidecar (stub mode) at $HEAD_AFTER"
fi

cmake -S "$SIDECAR_DIR" -B "$SIDECAR_DIR/build" -DBEAGLE_SDK_STUB="$stub_flag" "${extra[@]}"
cmake --build "$SIDECAR_DIR/build"

if [[ "${SKIP_RESTART:-0}" == "1" ]]; then
  log "SKIP_RESTART=1; built but not restarting"
  exit 0
fi

if ! command -v systemctl >/dev/null 2>&1; then
  log "systemctl not available; skipping restart"
  exit 0
fi

if systemctl "$SIDECAR_SYSTEMD_SCOPE" cat "$SIDECAR_SYSTEMD_UNIT" >/dev/null 2>&1; then
  log "systemctl $SIDECAR_SYSTEMD_SCOPE restart $SIDECAR_SYSTEMD_UNIT"
  systemctl "$SIDECAR_SYSTEMD_SCOPE" restart "$SIDECAR_SYSTEMD_UNIT"
else
  log "$SIDECAR_SYSTEMD_UNIT not installed under $SIDECAR_SYSTEMD_SCOPE; skipping restart"
fi

log "done at $HEAD_AFTER"
