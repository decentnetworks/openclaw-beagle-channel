# openclaw-beagle-channel
Beagle Chat provider for OpenClaw, split into two subprojects:

- `packages/beagle-channel` – OpenClaw channel plugin (TypeScript)
- `packages/beagle-sidecar` – Beagle sidecar daemon (C++)
  
Check [INSTALL.md](https://github.com/decentnetworks/openclaw-beagle-channel/blob/main/INSTALL.md) for details.
  
## Source Repositories

Clone these two repositories to get started:

```bash
# 1. Elastos Carrier Native SDK (the P2P networking layer)
git clone https://github.com/0xli/Elastos.NET.Carrier.Native.SDK.git

# 2. OpenClaw Beagle Channel (the OpenClaw integration)
git clone https://github.com/decentnetworks/openclaw-beagle-channel.git
```

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
