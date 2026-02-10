# openclaw-beagle-channel
Beagle Chat provider for OpenClaw, split into two subprojects:

- `packages/beagle-channel` – OpenClaw channel plugin (TypeScript)
- `packages/beagle-sidecar` – Beagle sidecar daemon (C++)

This repo is intentionally brand-named **Beagle / Beagle Chat** (no "Carrier" branding).

## Quick Start (Dev)

1. Build and run the sidecar (stub mode):

```bash
cd packages/beagle-sidecar
cmake -S . -B build -DBEAGLE_SDK_STUB=ON
cmake --build build
./build/beagle-sidecar --port 39091
```

2. Build the channel plugin:

```bash
cd packages/beagle-channel
npm install
npm run build
```

3. Configure OpenClaw to enable the Beagle channel:

```json
{
  "channels": {
    "beagle": {
      "accounts": {
        "default": {
          "enabled": true,
          "sidecarBaseUrl": "http://127.0.0.1:39091",
          "authToken": "devtoken"
        }
      }
    }
  }
}
```

## Project Layout

- `packages/beagle-channel` – OpenClaw plugin (TS). Handles outbound `sendText`/`sendMedia` and inbound polling.
- `packages/beagle-sidecar` – native daemon using the Beagle network SDK (stubbed until SDK wiring is added).

## Notes

- The sidecar provides a minimal localhost API: `/health`, `/sendText`, `/sendMedia`, `/events`.
- The SDK wiring is stubbed but structured so you can drop in the real SDK quickly.
