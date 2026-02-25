# openclaw-beagle-channel
Beagle Chat provider for OpenClaw, split into two subprojects:

- `packages/beagle-channel` – OpenClaw channel plugin (TypeScript)
- `packages/beagle-sidecar` – Beagle sidecar daemon (C++)

Check [INSTALL.md](https://github.com/decentnetworks/openclaw-beagle-channel/blob/main/INSTALL.md) for details.

## Beagle Chat + OpenClaw

[Beagle Chat](https://beagle.chat) is a decentralized messaging app built on Carrier networking. This project is the bridge between Beagle and OpenClaw:

- `beagle-sidecar` connects to Carrier/Beagle and exposes a local HTTP API.
- `beagle-channel` plugs into OpenClaw and maps Beagle DMs/groups to OpenClaw channel messages.

In practice, this repo lets OpenClaw agents send/receive messages in Beagle Chat (including group chats).

## Using Beagle App to access OpenClaw agent

1. Join the Beagle community Discord server:
   - https://discord.gg/kYgJGrXewE
2. Install Beagle app:
   - Android: https://beagle.chat/androidinstall.html
   - iOS (App Store): https://apps.apple.com/us/app/beagle/id1597429120
   - iOS (TestFlight): https://testflight.apple.com/join/xjXaKCHW
3. (Optional) Learn more:
   - https://beagle.chat
4. Add OpenClaw demo agent `snoopy` as a friend in Beagle:
   - `E9kgtcGGAXyddTKwh1o44PZavkRfdYTZCikiHxnrWhhQgd4JREP6`
5. (Optional) Add demo groups in Beagle:
   - `beagles`: `aHzsSgcuKuWrLM2QLehrCrJCH5idWWhkvqKudVUzgpf4EHta6377`
   - `BTCD`: `CotCjqmBrQ3rVwMMgXvm2naRuH2HzqeDPwdqTN7CCdzzagkB8QmM`
   - `Zen7`: `ehmqadYM8wXhgk5aHHPQ3jTfiQLrxC3L1CRkMDJ2E4qzt4JrLCzL`
6. For your own setup, install OpenClaw on a spare machine (or separate macOS user):
   - https://openclaw.ai/
7. Build and run your own Beagle channel so your OpenClaw agent has a Beagle/Carrier address:
   - https://github.com/decentnetworks/openclaw-beagle-channel

## Source Repositories

Clone these two repositories to get started:

```bash
# 1. Elastos Carrier Native SDK (the P2P networking layer)
git clone https://github.com/decentnetworks/Elastos.NET.Carrier.Native.SDK.git

# 2. OpenClaw Beagle Channel (the OpenClaw integration)
git clone https://github.com/decentnetworks/openclaw-beagle-channel.git
```

## Install (Users)

Prereqs: CMake + C++ toolchain, Node.js + npm.

```bash
git clone https://github.com/decentnetworks/openclaw-beagle-channel.git
cd openclaw-beagle-channel
./install.sh
```

Then start the sidecar and enable the channel in OpenClaw:

```bash
./packages/beagle-sidecar/build/beagle-sidecar --port 39091
```

```json
{
  "channels": {
    "beagle": {
      "dmPolicy": "open",
      "accounts": {
        "default": {
          "enabled": true,
          "sidecarBaseUrl": "http://127.0.0.1:39091",
          "authToken": "devtoken"
        }
      }
    }
  },
  "plugins": {
    "entries": {
      "beagle": { "enabled": true }
    }
  }
}
```

## Quick Start (Dev)

1. Build and run the sidecar (stub mode):

```bash
cd packages/beagle-sidecar
cmake -S . -B build -DBEAGLE_SDK_STUB=ON
cmake --build build
./build/beagle-sidecar --port 39091
```

2. Build and install the channel plugin:

```bash
cd packages/beagle-channel
npm install
npm run build

# Install plugin to OpenClaw extensions
mkdir -p ~/.openclaw/extensions/beagle
cp package.json index.js openclaw.plugin.json ~/.openclaw/extensions/beagle/
cp -r dist ~/.openclaw/extensions/beagle/
```

See `packages/beagle-channel/README.md` for full plugin documentation.

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

- The sidecar provides a localhost API: `/health`, `/status`, `/sendText`, `/sendMedia`, `/sendStatus`, `/events`.
- The SDK wiring is stubbed but structured so you can drop in the real SDK quickly.
- `packages/beagle-channel` supports CarrierGroup envelopes:
  - inbound `CGP1` -> group context mapping
  - outbound group replies as `CGR1`
  - outbound transient status as `CGS1` (group) or `BGS1` (DM)
  - status emission is opt-in via `BEAGLE_STATUS_ENABLED=1` (default off)
