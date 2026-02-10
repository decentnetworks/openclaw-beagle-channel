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

## HTTP API

- `GET /health` -> `{ "ok": true }`
- `POST /sendText` `{ "peer": "...", "text": "..." }`
- `POST /sendMedia` `{ "peer": "...", "caption": "...", "mediaPath": "..." }`
- `GET /events` -> `[{"peer":"...","text":"..."}]`
