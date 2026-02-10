# Beagle Sidecar

Native sidecar daemon that bridges OpenClaw to the Beagle network SDK.

## Build (Stub)

```bash
cmake -S . -B build -DBEAGLE_SDK_STUB=ON
cmake --build build
./build/beagle-sidecar --port 39091 --token devtoken
```

## Build (Real SDK)

Set the SDK root and disable stub mode:

```bash
export BEAGLE_SDK_ROOT=/path/to/Elastos.NET.Carrier.Native.SDK
cmake -S . -B build -DBEAGLE_SDK_STUB=OFF -DBEAGLE_SDK_ROOT=$BEAGLE_SDK_ROOT
cmake --build build
```

If your SDK build directory is non-standard, also pass `-DBEAGLE_SDK_BUILD_DIR=/path/to/build`.

Run with a Carrier config:

```bash
./build/beagle-sidecar --config $BEAGLE_SDK_ROOT/config/carrier.conf --data-dir ~/.carrier
```

You can also use the helper script:

```bash
./start.sh
```

`--token` is optional. If you set it, OpenClaw must send the same bearer token.

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
  "database": "beagle"
}
```

When enabled, the sidecar creates/uses:

- `beagle_friend_info` (current info)
- `beagle_friend_info_history` (changes over time)
- `beagle_friend_events` (online/offline events)

## HTTP API

- `GET /health` -> `{ "ok": true }`
- `POST /sendText` `{ "peer": "...", "text": "..." }`
- `POST /sendMedia` `{ "peer": "...", "caption": "...", "mediaPath": "..." }`
- `GET /events` -> `[{"peer":"...","text":"..."}]`
