#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
START_SH="$REPO_DIR/start.sh"
LABEL="com.openclaw.beagle-sidecar"
PLIST_DIR="$HOME/Library/LaunchAgents"
PLIST_PATH="$PLIST_DIR/$LABEL.plist"
LOG_DIR="$HOME/Library/Logs/beagle-sidecar"
STDOUT_LOG="$LOG_DIR/stdout.log"
STDERR_LOG="$LOG_DIR/stderr.log"

usage() {
  cat <<'EOF'
Usage:
  scripts/setup-launchd-user.sh start
  scripts/setup-launchd-user.sh stop
  scripts/setup-launchd-user.sh install
  scripts/setup-launchd-user.sh uninstall
  scripts/setup-launchd-user.sh restart
  scripts/setup-launchd-user.sh status
  scripts/setup-launchd-user.sh logs
  scripts/setup-launchd-user.sh logs --follow

Environment:
  BEAGLE_SDK_ROOT            Required. SDK root containing config/carrier.conf
  BEAGLE_SIDECAR_DATA_DIR    Default: ~/.carrier
  BEAGLE_SIDECAR_PORT        Optional port override
  BEAGLE_SIDECAR_TOKEN       Optional bearer token
  BEAGLE_SIDECAR_EXTRA_ARGS  Optional extra args (space-separated)
EOF
}

require_launchd_user() {
  if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "launchd user services are only supported on macOS. Current OS: $(uname -s)." >&2
    exit 1
  fi
  if ! command -v launchctl >/dev/null 2>&1; then
    echo "launchctl is required but was not found in PATH." >&2
    exit 1
  fi
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

xml_escape() {
  local value="${1:-}"
  value="${value//&/&amp;}"
  value="${value//</&lt;}"
  value="${value//>/&gt;}"
  printf '%s' "$value"
}

domain() {
  printf 'gui/%s' "$(id -u)"
}

service_target() {
  printf '%s/%s' "$(domain)" "$LABEL"
}

write_plist() {
  local data_dir="${BEAGLE_SIDECAR_DATA_DIR:-$HOME/.carrier}"
  mkdir -p "$PLIST_DIR" "$LOG_DIR"

  cat > "$PLIST_PATH" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$START_SH</string>
    <string>run</string>
  </array>
  <key>WorkingDirectory</key>
  <string>$REPO_DIR</string>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <dict>
    <key>SuccessfulExit</key>
    <false/>
  </dict>
  <key>StandardOutPath</key>
  <string>$STDOUT_LOG</string>
  <key>StandardErrorPath</key>
  <string>$STDERR_LOG</string>
  <key>EnvironmentVariables</key>
  <dict>
    <key>BEAGLE_SDK_ROOT</key>
    <string>$(xml_escape "$BEAGLE_SDK_ROOT")</string>
    <key>BEAGLE_SIDECAR_DATA_DIR</key>
    <string>$(xml_escape "$data_dir")</string>
EOF

  if [[ -n "${BEAGLE_SIDECAR_PORT:-}" ]]; then
    cat >> "$PLIST_PATH" <<EOF
    <key>BEAGLE_SIDECAR_PORT</key>
    <string>$(xml_escape "$BEAGLE_SIDECAR_PORT")</string>
EOF
  fi

  if [[ -n "${BEAGLE_SIDECAR_TOKEN:-}" ]]; then
    cat >> "$PLIST_PATH" <<EOF
    <key>BEAGLE_SIDECAR_TOKEN</key>
    <string>$(xml_escape "$BEAGLE_SIDECAR_TOKEN")</string>
EOF
  fi

  if [[ -n "${BEAGLE_SIDECAR_EXTRA_ARGS:-}" ]]; then
    cat >> "$PLIST_PATH" <<EOF
    <key>BEAGLE_SIDECAR_EXTRA_ARGS</key>
    <string>$(xml_escape "$BEAGLE_SIDECAR_EXTRA_ARGS")</string>
EOF
  fi

  cat >> "$PLIST_PATH" <<EOF
  </dict>
</dict>
</plist>
EOF

  plutil -lint "$PLIST_PATH" >/dev/null
}

unload_if_loaded() {
  launchctl bootout "$(domain)" "$PLIST_PATH" >/dev/null 2>&1 || true
}

ensure_plist_exists() {
  if [[ ! -f "$PLIST_PATH" ]]; then
    echo "$LABEL plist is not installed at $PLIST_PATH." >&2
    exit 1
  fi
}

show_status() {
  local output
  if output="$(launchctl print "$(service_target)" 2>&1)"; then
    printf '%s\n' "$output"
  else
    echo "$LABEL is not loaded." >&2
    exit 1
  fi
}

show_logs() {
  case "${1:-}" in
    --follow|-f)
      touch "$STDOUT_LOG" "$STDERR_LOG"
      tail -n 200 -f "$STDOUT_LOG" "$STDERR_LOG"
      ;;
    "")
      touch "$STDOUT_LOG" "$STDERR_LOG"
      tail -n 200 "$STDOUT_LOG" "$STDERR_LOG"
      ;;
    *)
      echo "Unknown logs option: $1" >&2
      usage
      exit 1
      ;;
  esac
}

cmd="${1:-}"
subcmd="${2:-}"
case "$cmd" in
  start)
    require_launchd_user
    ensure_plist_exists
    launchctl bootstrap "$(domain)" "$PLIST_PATH"
    launchctl enable "$(service_target)"
    launchctl kickstart -k "$(service_target)"
    show_status
    ;;
  stop)
    require_launchd_user
    ensure_plist_exists
    unload_if_loaded
    ;;
  install)
    require_launchd_user
    ensure_sdk_root
    write_plist
    unload_if_loaded
    launchctl bootstrap "$(domain)" "$PLIST_PATH"
    launchctl enable "$(service_target)"
    launchctl kickstart -k "$(service_target)"
    show_status
    ;;
  uninstall)
    require_launchd_user
    unload_if_loaded
    rm -f "$PLIST_PATH"
    ;;
  restart)
    require_launchd_user
    ensure_plist_exists
    launchctl kickstart -k "$(service_target)"
    show_status
    ;;
  status)
    require_launchd_user
    show_status
    ;;
  logs)
    require_launchd_user
    show_logs "$subcmd"
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
