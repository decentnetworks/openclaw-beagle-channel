# Beagle Sidecar

Native sidecar daemon that bridges OpenClaw to the Beagle network SDK.

## Building

### Prerequisites

* A C++17 compliant compiler (e.g., GCC, Clang)
* CMake (version 3.16 or newer)
* For the "Real SDK" build, you need a pre-built [Elastos.NET.Carrier.Native.SDK](https://github.com/elastos/Elastos.NET.Carrier.Native.SDK).

### Recommended SDK Branch (offline replay fix)

Until upstream merges the fix for replayed offline messages, build sidecar against this fork branch:

```bash
cd ~/devs
git clone https://github.com/decentnetworks/Elastos.NET.Carrier.Native.SDK.git
cd Elastos.NET.Carrier.Native.SDK
git checkout fix/express-offline-watermark-replay
```

PR tracking this fix upstream:
`https://github.com/0xli/Elastos.NET.Carrier.Native.SDK/pull/3`

### Build (Stub)

This mode builds the sidecar without any Beagle network functionality. This is useful for development and testing of the sidecar itself without requiring the full SDK.

```bash
cmake -S . -B build -DBEAGLE_SDK_STUB=ON
cmake --build build
./build/beagle-sidecar --port 39091 --token devtoken
```

### Build (Real SDK)

This mode builds the sidecar with the full Beagle network SDK.

First, make sure you have built the `Elastos.NET.Carrier.Native.SDK`. Then, set the `BEAGLE_SDK_ROOT` environment variable to the path of the SDK and run cmake.

```bash
export BEAGLE_SDK_ROOT=/path/to/Elastos.NET.Carrier.Native.SDK
cmake -S . -B build -DBEAGLE_SDK_STUB=OFF -DBEAGLE_SDK_ROOT=$BEAGLE_SDK_ROOT
cmake --build build
```

If your SDK build directory is in a non-standard location, you can specify it with the `BEAGLE_SDK_BUILD_DIR` variable:

```bash
cmake -S . -B build -DBEAGLE_SDK_STUB=OFF -DBEAGLE_SDK_ROOT=$BEAGLE_SDK_ROOT -DBEAGLE_SDK_BUILD_DIR=/path/to/sdk/build/dir
```

Run with a Carrier config:

```bash
./build/beagle-sidecar --config $BEAGLE_SDK_ROOT/config/carrier.conf --data-dir ~/.carrier
```

You can also use the helper script:

```bash
./start.sh
```

`--token` is optional. If you set it, OpenClaw must send the same bearer token.

## Run In Background (systemd user service)

The scripts can auto-detect `BEAGLE_SDK_ROOT` from `build/CMakeCache.txt` if you already built.
Otherwise, export it first.

Install and start:

```bash
export BEAGLE_SDK_ROOT=/path/to/Elastos.NET.Carrier.Native.SDK
./start.sh --install-systemd-user
```

Status and logs:

```bash
./start.sh --status-systemd-user
scripts/setup-systemd-user.sh logs
scripts/setup-systemd-user.sh logs --follow
```

Uninstall:

```bash
./start.sh --uninstall-systemd-user
```

To keep it running after logout, you may need:

```bash
loginctl enable-linger "$USER"
```

## Crawler Service (userid -> ip/location)

Install and start crawler as a user service:

```bash
scripts/setup-crawler-systemd-user.sh install
```

Status and logs:

```bash
scripts/setup-crawler-systemd-user.sh status
scripts/setup-crawler-systemd-user.sh logs
scripts/setup-crawler-systemd-user.sh logs --follow
```

If you change sidecar bootstraps and want crawler to pick them up:

```bash
scripts/setup-crawler-systemd-user.sh sync-config
systemctl --user restart elacrawler.service
```

`sync-config` preserves existing crawler bootnodes and appends any missing bootnodes from `BEAGLE_SDK_ROOT/config/carrier.conf`.
Crawler does not support `express_nodes`; only `bootstraps` are synced.

## Profile + Welcome Config

When you pass `--data-dir`, the sidecar will create:

- `beagle_profile.json` (profile + welcome message)
- `welcomed_peers.txt` (persisted list of peers already welcomed)
- `beagle_db.json` (MySQL logging config)
- `friend_state.tsv` (last known friend info/status snapshot)
- `friend_events.log` (online/offline events)

Edit `beagle_profile.json` at any time to update the user profile and welcome text.
Default contents:

```json
{
  "welcomeMessage": "Hi! I'm the Beagle OpenClaw bot. Send a message to start.",
  "profile": {
    "name": "Snoopy",
    "gender": "2218",
    "phone": "Claw Bot to Help",
    "email": "SOL:,ETH:",
    "description": "Ask me anything about beagle chat, Tell me who your are",
    "region": "California"
  }
}
```

To enable MySQL logging, edit `beagle_db.json` and set `"enabled": true`.
Default contents:

```json
{
  "enabled": false,
  "host": "localhost",
  "port": 3306,
  "user": "beagle",
  "password": "A1anSn00py",
  "database": "beagle",
  "useCrawlerIndex": false,
  "crawlerDataDir": "~/.elacrawler",
  "crawlerRefreshSeconds": 60,
  "crawlerLookbackFiles": 20
}
```

Set `"useCrawlerIndex": true` to resolve `ip`/`location` from crawler output files.

When enabled, the sidecar creates/uses:

- `beagle_friend_info` (current info)
- `beagle_friend_info_history` (changes over time)
- `beagle_friend_events` (online/offline events, with crawler-derived `ip` and `location`)
- `beagle_crawler_node_cache` (persistent `userid -> ip/location` cache from crawler `.lst`)

## HTTP API

- `GET /health` -> `{ "ok": true }`
- `GET /status` -> sidecar runtime status snapshot
- `POST /sendText` `{ "peer": "...", "text": "..." }`
- `POST /sendMedia` `{ "peer": "...", "caption": "...", "mediaPath": "..." }`
- `POST /sendStatus` `{ "peer":"...", "state":"typing|thinking|tool|sending|idle|error", "ttlMs":12000, "chatType":"direct|group", "groupUserId":"...", "groupAddress":"...", "groupName":"...", "phase":"...", "seq":"..." }`
- `GET /events` -> `[{"peer":"...","text":"..."}]`

Status transport details:

- sidecar emits `BGS1 <json>` for DM status
- sidecar emits `CGS1 <json>` for group status
- these are transient control envelopes intended for client typing/thinking indicators
