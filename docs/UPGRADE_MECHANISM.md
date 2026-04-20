# Upgrade mechanism — openclaw + beagle-channel + beagle-sidecar

**Status:** design doc; scaffolding shipped in `deploy/systemd/` and
`deploy/scripts/`. Policy (who upgrades when) lives outside this repo.

## The problem

This repo ships three things that each need their own upgrade story:

| Component       | Lang | Lives in                            | Upgraded by           |
|-----------------|------|-------------------------------------|-----------------------|
| `beagle-sidecar` | C++ | `packages/beagle-sidecar/`          | `cmake --build`        |
| `beagle-channel` | JS  | `packages/beagle-channel/`          | `npm run build`        |
| `openclaw` CLI + core | JS | installed globally (`~/.npm-global`) | built-in `openclaw update` |

`openclaw update` (first-party, ships with openclaw) already covers the third
row end-to-end, and `openclaw plugins update --all` covers the second —
**provided** beagle-channel was installed via `openclaw plugins install
--link`. The first row (C++ sidecar) has no comparable story; compiling is
local and specific to this repo.

Before this change, `install.sh` would raw-`cp` files into
`~/.openclaw/extensions/beagle`, which left the plugin untracked (openclaw
logged `loaded without install/load-path provenance` and skipped it from
`plugins update --all`). So even though the openclaw upgrade plumbing
existed, we weren't using it.

## The three layers

```
┌──────────────────────────────────────────────────────────────┐
│ Policy      directory DB / per-host drop-ins                 │  ← when
│             (cadence, canary, stagger, disable-on-host)      │
├──────────────────────────────────────────────────────────────┤
│ Mechanism   openclaw update / openclaw plugins update        │  ← how
│             update-sidecar.sh (git pull + cmake)             │
├──────────────────────────────────────────────────────────────┤
│ Signal      systemd timer + scheduled-update.sh entrypoint   │  ← when to
│                                                              │    run mechanism
└──────────────────────────────────────────────────────────────┘
```

- **Mechanism** already existed for 2 of 3 components. The only new code we
  write is `update-sidecar.sh` — it's a few dozen lines because all it does
  is re-run the same cmake steps `install.sh` runs.
- **Signal** is the systemd timer. Intentionally dumb: fire once a week,
  run `scheduled-update.sh`, done. No decision-making.
- **Policy** is where operators or the directory step in. This repo
  deliberately does **not** bake in a canary system, a blast-radius cap,
  or a "pause all updates" kill switch — those are deployment concerns.
  Per-host drop-ins handle stagger; a future directory endpoint could
  push policy centrally.

## Why not put the upgrader in beagle-sidecar itself?

Considered and rejected:

- **Sidecar self-upgrade** is a C++ process that would have to `exec(2)`
  into its replacement. Risk of wedging a host on a bad build is high, and
  it gives the sidecar a privilege (write access to its own binary) it
  doesn't need for its actual job (Carrier bridging).
- **beagle-channel self-upgrade** would run inside openclaw-gateway,
  which is the same process it's replacing. Upgrading the process hosting
  you is fiddly (you have to hand off state) and openclaw already does
  this well via `openclaw plugins update`.
- **Directory-driven push** means directory SSHes into each peer and runs
  commands. That's a security posture change we don't want to introduce
  for this use case. The directory can still *signal* "please upgrade" via
  a Carrier DM (analogous to the existing `_openclaw_directory_request`
  identity nudge), and the peer's local timer/script does the actual work.

So: **mechanism stays local to each host**, **signal is a boring cron-like
timer**, and **policy is delegated to operators or a future directory
feature**. This matches how openclaw itself was designed.

## Scheduled flow

`deploy/scripts/scheduled-update.sh` runs three ordered steps, each
independently skippable:

```
┌── openclaw update --yes ────────────────────┐   ← step 1 (CLI + core)
│                                             │
├── openclaw plugins update --all ────────────┤   ← step 2 (tracked plugins,
│                                             │               incl. beagle-channel)
├── deploy/scripts/update-sidecar.sh ─────────┤   ← step 3 (C++ sidecar)
│    ├── git pull --ff-only                   │
│    ├── cmake -S … -B …/build                │
│    ├── cmake --build …/build                │
│    └── systemctl --user restart beagle-...  │
└─────────────────────────────────────────────┘
```

- Ordering matters: core before plugins (plugin manifest may reference
  newer core APIs); plugins before sidecar (sidecar restart is the most
  disruptive — do it last).
- Each step fails loud (non-zero exit → unit fails → journal has the
  stack). Timer retries on next tick; no in-script retry loop. Easier to
  debug, and we'd rather miss one cycle than thrash.
- `update-sidecar.sh` bails early if `git HEAD` didn't move and the binary
  already exists. Avoids needless rebuild churn.

## What to install for a new host

```bash
# 1. Bootstrap: clone + build + register plugin.
git clone <repo> ~/devs/openclaw-beagle-channel
cd ~/devs/openclaw-beagle-channel
./install.sh                      # rebuilds sidecar, registers plugin via
                                  # `openclaw plugins install --link` if
                                  # the CLI is present

# 2. Opt into scheduled upgrades.
mkdir -p ~/.config/systemd/user
cp deploy/systemd/openclaw-beagle-channel-update.{service,timer} \
   ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now openclaw-beagle-channel-update.timer
```

See [`deploy/systemd/README.md`](../deploy/systemd/README.md) for the full
install + customization guide.

## Open questions / deferred

- **Signed updates.** `openclaw update` pulls from npm; npm is the trust
  root. For C++ sidecar, `git pull` from `origin` is the trust root.
  Stronger supply-chain signing (cosign-style) is out of scope here.
- **Cross-version compatibility.** If a new sidecar ABI breaks an old
  channel (or vice versa), the current flow cheerfully upgrades sidecar
  first, restarts it, then moves on. A proper "upgrade plan" that
  verifies compatibility up front isn't implemented. Fleet-wide
  compatibility is currently maintained by shipping them together from
  this repo.
- **Rollback.** No automatic rollback. If an upgrade breaks a host,
  operator reverts `HEAD` and runs `scheduled-update.sh` manually.
  For openclaw core, `openclaw update --tag <previous>` handles it.
- **Directory signaling.** Not implemented. The directory can't currently
  say "please upgrade now." A DM marker analogous to
  `_openclaw_directory_request` would be a natural fit — start
  `scheduled-update.sh` on receipt — but it's not shipped in this PR.
