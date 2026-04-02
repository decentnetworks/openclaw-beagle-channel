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

Multi-agent mode (auto-discover OpenClaw agents and allocate one Carrier identity per agent):

```bash
./build/beagle-sidecar \
  --config $BEAGLE_SDK_ROOT/config/carrier.conf \
  --data-dir ~/.carrier \
  --openclaw-config ~/.openclaw/openclaw.json \
  --directory-address bKaapxLjDGwuCZ7oohVFMVbcHWFYy7yNVGXkVSVL8AkAxbokCGi2
```

If `--openclaw-config` is omitted, sidecar checks:

1. `BEAGLE_OPENCLAW_CONFIG`
2. `OPENCLAW_CONFIG`
3. `~/.openclaw/openclaw.json` (if present)

When agents are discovered, sidecar starts one account per agent and uses agent metadata
(`name`, `description`, `gender`, `phone`, `email`, `region`) to populate Carrier self userinfo.

Optional OpenClaw Directory friend bootstrap:

- `--directory-address <carrier_address>`: auto-send add-friend request at startup for each account.
- `--directory-hello <text>`: custom hello text (default: `openclaw-beagle-channel`).
- Env fallback: `BEAGLE_DIRECTORY_ADDRESS` / `OPENCLAW_DIRECTORY_ADDRESS`, and
  `BEAGLE_DIRECTORY_HELLO` / `OPENCLAW_DIRECTORY_HELLO`.

If you omit `--directory-address` (and the env fallbacks are empty), the sidecar adds both built-in default directories as friends:
- `bKaapxLjDGwuCZ7oohVFMVbcHWFYy7yNVGXkVSVL8AkAxbokCGi2`
- `C5WWd6BpDvZmqVysfKZWFitK2B7XJJy9dwXVH12KGK62dk2RdKUt`

If you provide `--directory-address`, only that single address is used instead of the defaults.
The sidecar skips adding itself when its own Carrier address matches a directory address.

After the directory is added as a friend, the sidecar waits until that friend is **online**, then sends a one-time JSON profile message containing the Carrier address, agent name, OpenClaw version, host name, and host IP.

## Multi-Agent Routing (What Was Asked vs Implemented)

Requested:

- One Carrier address per OpenClaw agent (not one address for the whole OpenClaw instance)
- Use OpenClaw agent info to fill Carrier userinfo/friendinfo-visible profile fields
- Ensure inbound Carrier messages are delivered to the right OpenClaw agent

Implemented:

- Sidecar discovers agents from local OpenClaw config file (`openclaw.json`) and starts one sidecar account per agent
- Each account has its own Carrier identity/address and isolated data directory
- Sidecar writes metadata (`carrierUserId`, `carrierAddress`, `openclawAgentId`) to profile and applies agent profile overrides
- API is account-aware via `X-Beagle-Account` (or body `accountId`)
- Default account prefers `main`, then `default`, then first available account

How a Carrier message reaches the right agent:

1. `beagle-channel` polls `/events` with `X-Beagle-Account:<accountId>`
2. Sidecar returns only events for that account's Carrier identity
3. OpenClaw runtime resolves final `agentId` using channel routing rules with `(channel=beagle, accountId, peer)`
4. Outbound reply uses the same account header, so it sends from the matching Carrier address

How to use with your `main` + `beagle-profile` agents:

1. Verify OpenClaw agent bindings:
   - `openclaw agents list --bindings`
2. Start sidecar with OpenClaw config:
   - `./build/beagle-sidecar --config $BEAGLE_SDK_ROOT/config/carrier.conf --data-dir ~/.carrier --openclaw-config ~/.openclaw/openclaw.json`
3. Check generated accounts and addresses:
   - `curl -s http://127.0.0.1:39091/health`
4. Configure `channels.beagle.accounts` in OpenClaw so account IDs match agent accounts
5. Ensure routing bindings map beagle account/peer to the intended agent (verify again with `--bindings`)

Manual add-friend API (runtime, no restart):

```bash
curl -s -X POST http://127.0.0.1:39091/addFriend \
  -H "Content-Type: application/json" \
  -H "X-Beagle-Account: default" \
  -d '{"address":"bKaapxLjDGwuCZ7oohVFMVbcHWFYy7yNVGXkVSVL8AkAxbokCGi2","hello":"openclaw-beagle-channel"}'
```

`address` is required (alias: `peer`); `hello` is optional.

You can also use the helper script:

```bash
./start.sh
```

`--token` is optional. If you set it, OpenClaw must send the same bearer token.

## Run In Background (systemd user service, Linux only)

The scripts can auto-detect `BEAGLE_SDK_ROOT` from `build/CMakeCache.txt` if you already built.
Otherwise, export it first.

Install and start:

```bash
export BEAGLE_SDK_ROOT=/path/to/Elastos.NET.Carrier.Native.SDK
./start.sh --install-service
```

Status and logs:

```bash
./start.sh --status-service
./start.sh --restart-service
scripts/setup-systemd-user.sh logs
scripts/setup-systemd-user.sh logs --follow
```

Uninstall:

```bash
./start.sh --uninstall-service
```

To keep it running after logout, you may need:

```bash
loginctl enable-linger "$USER"
```

## Run In Background (launchd user agent, macOS only)

The macOS helper also auto-detects `BEAGLE_SDK_ROOT` from `build/CMakeCache.txt`
if you already built. Otherwise, export it first.

Install and start:

```bash
export BEAGLE_SDK_ROOT=/path/to/Elastos.NET.Carrier.Native.SDK
./start.sh --install-service
```

Status and logs:

```bash
./start.sh --status-service
./start.sh --restart-service
scripts/setup-launchd-user.sh logs
scripts/setup-launchd-user.sh logs --follow
```

Uninstall:

```bash
./start.sh --uninstall-service
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

In multi-agent mode, files are isolated per account under:

- `--data-dir/accounts/<accountId>/...`

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

- `GET /health` -> selected account identity + all account list
- `GET /status` -> selected account runtime status snapshot
- `POST /sendText` `{ "peer": "...", "text": "...", "accountId":"optional" }`
- `POST /sendMedia` `{ "peer": "...", "caption": "...", "mediaPath": "...", "accountId":"optional" }`
- `POST /sendStatus` `{ "peer":"...", "state":"typing|thinking|tool|sending|idle|error", "ttlMs":12000, "chatType":"direct|group", "groupUserId":"...", "groupAddress":"...", "groupName":"...", "phase":"...", "seq":"...", "accountId":"optional" }`
- `GET /events` -> `[{"accountId":"...","peer":"...","text":"..."}]`

Account selection:

- Recommended: request header `X-Beagle-Account: <accountId>`
- Fallback: JSON body field `accountId`
- If omitted, sidecar uses the default account

Status transport details:

- sidecar emits `BGS1 <json>` for DM status
- sidecar emits `CGS1 <json>` for group status
- these are transient control envelopes intended for client typing/thinking indicators
