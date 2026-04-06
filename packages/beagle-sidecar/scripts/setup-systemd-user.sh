#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
START_SH="$REPO_DIR/start.sh"
UNIT_DIR="$HOME/.config/systemd/user"
UNIT_PATH="$UNIT_DIR/beagle-sidecar.service"

usage() {
  cat <<'EOF'
Usage:
  scripts/setup-systemd-user.sh start
  scripts/setup-systemd-user.sh stop
  scripts/setup-systemd-user.sh install
  scripts/setup-systemd-user.sh uninstall
  scripts/setup-systemd-user.sh restart
  scripts/setup-systemd-user.sh status
  scripts/setup-systemd-user.sh logs
  scripts/setup-systemd-user.sh logs --follow

Environment:
  BEAGLE_SDK_ROOT            Required. SDK root containing config/carrier.conf
  BEAGLE_SIDECAR_DATA_DIR    Default: ~/.carrier
  BEAGLE_SIDECAR_PORT        Optional port override
  BEAGLE_SIDECAR_TOKEN       Optional bearer token
  BEAGLE_SIDECAR_EXTRA_ARGS  Optional extra args (space-separated)
EOF
}

require_systemd_user() {
  if [[ "$(uname -s)" != "Linux" ]]; then
    echo "systemd user services are only supported on Linux. Current OS: $(uname -s)." >&2
    exit 1
  fi
  if ! command -v systemctl >/dev/null 2>&1; then
    echo "systemctl is required but was not found in PATH." >&2
    exit 1
  fi
}

maybe_delegate() {
  case "$(uname -s)" in
    Darwin)
      exec "$SCRIPT_DIR/setup-launchd-user.sh" "$@"
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
    line="$(grep -m1 '^BEAGLE_SDK_ROOT:UNINITIALIZED=' "$cache" 2>/dev/null || true)"
    if [[ -n "$line" ]]; then
      BEAGLE_SDK_ROOT="${line#*=}"
      export BEAGLE_SDK_ROOT
    fi
  fi
}

ensure_sdk_root() {
  resolve_sdk_root
  if [[ -z "${BEAGLE_SDK_ROOT:-}" ]]; then
    echo "BEAGLE_SDK_ROOT is required for install (or build/CMakeCache.txt must include it)." >&2
    exit 1
  fi
}

write_unit() {
  local data_dir="${BEAGLE_SIDECAR_DATA_DIR:-$HOME/.carrier}"
  mkdir -p "$UNIT_DIR"

  {
    echo "[Unit]"
    echo "Description=Beagle Sidecar"
    echo "After=network.target"
    echo
    echo "[Service]"
    echo "Type=simple"
    echo "WorkingDirectory=$REPO_DIR"
    echo "ExecStart=$START_SH run"
    echo "Restart=on-failure"
    echo "RestartSec=3"
    echo "Environment=BEAGLE_SDK_ROOT=$BEAGLE_SDK_ROOT"
    echo "Environment=BEAGLE_SIDECAR_DATA_DIR=$data_dir"
    if [[ -n "${BEAGLE_SIDECAR_PORT:-}" ]]; then
      echo "Environment=BEAGLE_SIDECAR_PORT=$BEAGLE_SIDECAR_PORT"
    fi
    if [[ -n "${BEAGLE_SIDECAR_TOKEN:-}" ]]; then
      echo "Environment=BEAGLE_SIDECAR_TOKEN=$BEAGLE_SIDECAR_TOKEN"
    fi
    if [[ -n "${BEAGLE_SIDECAR_EXTRA_ARGS:-}" ]]; then
      echo "Environment=BEAGLE_SIDECAR_EXTRA_ARGS=$BEAGLE_SIDECAR_EXTRA_ARGS"
    fi
    echo
    echo "[Install]"
    echo "WantedBy=default.target"
  } > "$UNIT_PATH"
}

cmd="${1:-}"
subcmd="${2:-}"
maybe_delegate "$@"
case "$cmd" in
  start)
    require_systemd_user
    systemctl --user start beagle-sidecar
    systemctl --user status beagle-sidecar --no-pager
    ;;
  stop)
    require_systemd_user
    systemctl --user stop beagle-sidecar
    ;;
  install)
    require_systemd_user
    ensure_sdk_root
    write_unit
    systemctl --user daemon-reload
    systemctl --user enable --now beagle-sidecar
    systemctl --user status beagle-sidecar --no-pager
    ;;
  uninstall)
    require_systemd_user
    systemctl --user disable --now beagle-sidecar || true
    rm -f "$UNIT_PATH"
    systemctl --user daemon-reload
    ;;
  restart)
    require_systemd_user
    systemctl --user restart beagle-sidecar
    systemctl --user status beagle-sidecar --no-pager
    ;;
  status)
    require_systemd_user
    systemctl --user status beagle-sidecar --no-pager
    ;;
  logs)
    require_systemd_user
    case "$subcmd" in
      --follow|-f)
        journalctl --user -u beagle-sidecar -n 200 -f
        ;;
      "")
        journalctl --user -u beagle-sidecar -n 200 --no-pager
        ;;
      *)
        echo "Unknown logs option: $subcmd" >&2
        usage
        exit 1
        ;;
    esac
    ;;
  -h|--help|"")
    usage
    ;;
  *)
    echo "Unknown command: $cmd" >&2
    usage
    exit 1
    ;;
esac
