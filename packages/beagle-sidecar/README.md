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

Then wire the SDK headers/libraries in `CMakeLists.txt` and implement the real logic in `src/beagle_sdk.cpp`.

## HTTP API

- `GET /health` -> `{ "ok": true }`
- `POST /sendText` `{ "peer": "...", "text": "..." }`
- `POST /sendMedia` `{ "peer": "...", "caption": "...", "mediaPath": "..." }`
- `GET /events` -> `[{"peer":"...","text":"..."}]`
