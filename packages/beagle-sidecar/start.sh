#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$SCRIPT_DIR"
BIN="$REPO_DIR/build/beagle-sidecar"

usage() {
  cat <<'EOF'
Usage:
  ./start.sh run
  ./start.sh --start-service
  ./start.sh --stop-service
  ./start.sh --install-service
  ./start.sh --uninstall-service
  ./start.sh --restart-service
  ./start.sh --status-service
  ./start.sh --start-systemd-user
  ./start.sh --stop-systemd-user
  ./start.sh --install-systemd-user
  ./start.sh --uninstall-systemd-user
  ./start.sh --restart-systemd-user
  ./start.sh --status-systemd-user
  ./start.sh --start-launchd-user
  ./start.sh --stop-launchd-user
  ./start.sh --install-launchd-user
  ./start.sh --uninstall-launchd-user
  ./start.sh --restart-launchd-user
  ./start.sh --status-launchd-user

Environment:
  BEAGLE_SDK_ROOT            Required. SDK root containing config/carrier.conf
  BEAGLE_SIDECAR_DATA_DIR    Default: ~/.carrier
  BEAGLE_SIDECAR_PORT        Optional port override
  BEAGLE_SIDECAR_TOKEN       Optional bearer token
  BEAGLE_DIRECTORY_ADDRESS   Optional directory Carrier address override
                             (default: ZJUCSC...1jV2)
  BEAGLE_EMIT_PRESENCE       Set non-empty to emit friend online/offline events
  BEAGLE_SIDECAR_EXTRA_ARGS  Optional extra args (space-separated)
EOF
}

service_setup_script() {
  case "$(uname -s)" in
    Linux)
      printf '%s/scripts/setup-systemd-user.sh' "$REPO_DIR"
      ;;
    Darwin)
      printf '%s/scripts/setup-launchd-user.sh' "$REPO_DIR"
      ;;
    *)
      echo "Unsupported OS for background service setup: $(uname -s)." >&2
      exit 1
      ;;
  esac
}

resolve_sdk_root() {
  if [[ -n "${BEAGLE_SDK_ROOT:-}" ]]; then
    return 0
  fi
  local cache="$REPO_DIR/build/CMakeCache.txt"
  if [[ -f "$cache" ]]; then
    local line
    line="$(grep -m 1 "BEAGLE_SDK_ROOT:UNINITIALIZED=" "$cache" || true)"
    if [[ -n "$line" ]]; then
      BEAGLE_SDK_ROOT="${line#*=}"
      export BEAGLE_SDK_ROOT
    fi
  fi
}

ensure_sdk_root() {
  resolve_sdk_root
  if [[ -z "${BEAGLE_SDK_ROOT:-}" ]]; then
    echo "BEAGLE_SDK_ROOT is required (or build/CMakeCache.txt must include it)." >&2
    exit 1
  fi
}

ensure_real_build() {
  local cache="$REPO_DIR/build/CMakeCache.txt"
  if [[ ! -f "$cache" ]]; then
    return 0
  fi
  if grep -q '^BEAGLE_SDK_STUB:BOOL=ON$' "$cache"; then
    if [[ "${BEAGLE_ALLOW_STUB:-}" == "1" ]]; then
      echo "Warning: build/beagle-sidecar is configured with BEAGLE_SDK_STUB=ON; no real Carrier account will be created." >&2
      return 0
    fi
    cat >&2 <<'EOF'
build/beagle-sidecar is configured with BEAGLE_SDK_STUB=ON.
This mode never creates real Carrier accounts.

Reconfigure and rebuild with:
  cmake -S . -B build -DBEAGLE_SDK_STUB=OFF -DBEAGLE_SDK_ROOT=$BEAGLE_SDK_ROOT
  cmake --build build

If you intentionally want stub mode, rerun with BEAGLE_ALLOW_STUB=1.
EOF
    exit 1
  fi
}

build_args() {
  local data_dir="${BEAGLE_SIDECAR_DATA_DIR:-$HOME/.carrier}"
  ARGS=(--config "$BEAGLE_SDK_ROOT/config/carrier.conf" --data-dir "$data_dir")
  if [[ -n "${BEAGLE_SIDECAR_PORT:-}" ]]; then
    ARGS+=(--port "$BEAGLE_SIDECAR_PORT")
  fi
  if [[ -n "${BEAGLE_SIDECAR_TOKEN:-}" ]]; then
    ARGS+=(--token "$BEAGLE_SIDECAR_TOKEN")
  fi
  if [[ -n "${BEAGLE_DIRECTORY_ADDRESS:-}" ]]; then
    ARGS+=(--directory-address "$BEAGLE_DIRECTORY_ADDRESS")
  fi
  if [[ -n "${BEAGLE_EMIT_PRESENCE:-}" ]]; then
    ARGS+=(--emit-presence)
  fi
  if [[ -n "${BEAGLE_SIDECAR_EXTRA_ARGS:-}" ]]; then
    read -r -a EXTRA_ARGS <<<"$BEAGLE_SIDECAR_EXTRA_ARGS"
    ARGS+=("${EXTRA_ARGS[@]}")
  fi
}

cmd="${1:-run}"
case "$cmd" in
  run)
    ensure_sdk_root
    ensure_real_build
    build_args
    export LD_LIBRARY_PATH="${BEAGLE_SDK_ROOT}/build/linux/src/carrier:${BEAGLE_SDK_ROOT}/build/linux/src/session:${BEAGLE_SDK_ROOT}/build/linux/src/filetransfer:${BEAGLE_SDK_ROOT}/build/linux/intermediates/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    exec "$BIN" "${ARGS[@]}"
    ;;
  --start-service)
    "$(service_setup_script)" start
    ;;
  --stop-service)
    "$(service_setup_script)" stop
    ;;
  --install-service)
    "$(service_setup_script)" install
    ;;
  --uninstall-service)
    "$(service_setup_script)" uninstall
    ;;
  --restart-service)
    "$(service_setup_script)" restart
    ;;
  --status-service)
    "$(service_setup_script)" status
    ;;
  --start-systemd-user)
    "$(service_setup_script)" start
    ;;
  --stop-systemd-user)
    "$(service_setup_script)" stop
    ;;
  --install-systemd-user)
    "$(service_setup_script)" install
    ;;
  --uninstall-systemd-user)
    "$(service_setup_script)" uninstall
    ;;
  --restart-systemd-user)
    "$(service_setup_script)" restart
    ;;
  --status-systemd-user)
    "$(service_setup_script)" status
    ;;
  --start-launchd-user)
    "$REPO_DIR/scripts/setup-launchd-user.sh" start
    ;;
  --stop-launchd-user)
    "$REPO_DIR/scripts/setup-launchd-user.sh" stop
    ;;
  --install-launchd-user)
    "$REPO_DIR/scripts/setup-launchd-user.sh" install
    ;;
  --uninstall-launchd-user)
    "$REPO_DIR/scripts/setup-launchd-user.sh" uninstall
    ;;
  --restart-launchd-user)
    "$REPO_DIR/scripts/setup-launchd-user.sh" restart
    ;;
  --status-launchd-user)
    "$REPO_DIR/scripts/setup-launchd-user.sh" status
    ;;
  -h|--help)
    usage
    ;;
  *)
    echo "Unknown command: $cmd" >&2
    usage
    exit 1
    ;;
esac
