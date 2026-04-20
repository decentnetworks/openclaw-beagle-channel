# Scheduled upgrades for openclaw + beagle-channel + beagle-sidecar

These are **opt-in** systemd `--user` units. Nothing in this repo installs
them by default; operators copy them into place when they want unattended
upgrades on a host.

## What it does

On a weekly timer (Mon 04:30 local + up to 30min jitter), run
[`deploy/scripts/scheduled-update.sh`](../scripts/scheduled-update.sh), which:

1. `openclaw update --yes` — upgrades the openclaw CLI itself (built-in
   self-upgrade path, JS).
2. `openclaw plugins update --all` — refreshes every tracked plugin,
   including `@beagle/openclaw-channel` if it was installed with
   `openclaw plugins install --link` (see the repo root `install.sh`).
3. [`deploy/scripts/update-sidecar.sh`](../scripts/update-sidecar.sh) —
   `git pull` + `cmake --build` + `systemctl --user restart
   beagle-sidecar.service`. The C++ sidecar is **not** covered by
   `openclaw update`; this is the piece that keeps it current.

Each step is independently skippable (see env vars in the scripts).

## Install

Assumes the repo is checked out at `~/devs/openclaw-beagle-channel`. Adjust
the paths in the unit file if yours lives elsewhere, or use a drop-in.

```bash
mkdir -p ~/.config/systemd/user

cp ~/devs/openclaw-beagle-channel/deploy/systemd/openclaw-beagle-channel-update.service \
   ~/.config/systemd/user/
cp ~/devs/openclaw-beagle-channel/deploy/systemd/openclaw-beagle-channel-update.timer \
   ~/.config/systemd/user/

systemctl --user daemon-reload
systemctl --user enable --now openclaw-beagle-channel-update.timer
```

Check status:

```bash
systemctl --user list-timers | grep openclaw-beagle-channel-update
systemctl --user status openclaw-beagle-channel-update.timer
journalctl --user -u openclaw-beagle-channel-update.service --since -7d
```

Force a run now (useful for the first deploy):

```bash
systemctl --user start openclaw-beagle-channel-update.service
journalctl --user -u openclaw-beagle-channel-update.service -f
```

## Customizing

Use a drop-in instead of editing the shipped unit — keeps `git pull` from
clobbering your local changes.

```bash
systemctl --user edit openclaw-beagle-channel-update.service
```

Common drop-ins:

```ini
# Build sidecar with real SDK instead of stub.
[Service]
Environment=BEAGLE_SDK_ROOT=%h/devs/beagle-sdk
Environment=BEAGLE_SDK_BUILD_DIR=%h/devs/beagle-sdk/build

# Different checkout path.
Environment=REPO_DIR=/opt/openclaw-beagle-channel
```

```ini
# Different cadence (hourly instead of weekly).
[Timer]
OnCalendar=
OnCalendar=hourly
```

(The empty `OnCalendar=` resets the list before appending — required when
overriding, otherwise both schedules apply.)

## Troubleshooting

**Unit failed after `openclaw update --yes`:**  
Usually a network/npm-registry hiccup. `systemctl --user start <unit>` again
in a bit. If it repeats, `openclaw update --dry-run` manually to see what
it's trying to do, or check `~/.openclaw/logs/update.log`.

**Unit failed inside `update-sidecar.sh`:**  
Likely a merge conflict on `git pull --ff-only` (you have local commits on
the repo's default branch). Run `git status` in the checkout; either rebase
or stash. The script won't `git reset --hard` — it refuses to clobber local
work.

**`beagle-sidecar.service` didn't restart:**  
The script only restarts units it can see. Run
`systemctl --user cat beagle-sidecar.service` to confirm the unit exists
under your user scope; if your sidecar is under `--system` instead, set
`Environment=SIDECAR_SYSTEMD_SCOPE=--system` in the drop-in.

## Not shipped here (yet)

- **Dependency between the update run and service restarts.** If you want
  a restart of `openclaw-gateway.service` after plugin updates, add it via
  drop-in `ExecStartPost=`.
- **A "canary" mechanism** (update one host, wait, then the rest). For now
  use a per-host drop-in to stagger `OnCalendar=` times across the fleet.
