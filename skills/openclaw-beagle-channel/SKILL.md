---
name: openclaw-beagle-channel
description: >
  Build, install, configure, and troubleshoot the Beagle Chat channel plugin
  and Beagle sidecar for OpenClaw. Covers local development, sidecar startup,
  extension install to ~/.openclaw/extensions/beagle, OpenClaw config updates,
  CarrierGroup/group chat behavior, Discord subscription relay, and common
  Beagle media/routing issues. Use when: working on the
  openclaw-beagle-channel repository, setting up Beagle Chat for an OpenClaw
  agent, debugging beagle-sidecar or beagle-channel behavior, preparing this
  integration for ClawHub or OpenClaw distribution.
metadata:
  openclaw:
    requires:
      bins:
        - git
        - npm
        - cmake
---

# OpenClaw Beagle Channel

Use this skill when the task is specifically about the Beagle Chat integration
for OpenClaw. This repository has two moving parts:

- `packages/beagle-channel`: the TypeScript OpenClaw channel plugin
- `packages/beagle-sidecar`: the local Beagle/Carrier sidecar daemon

Treat the skill as an operator and maintainer guide. Prefer the existing
commands and config shapes in this skill instead of inventing new ones.

## Quick Start

1. Build and run the sidecar in stub mode unless the real Carrier SDK is
   explicitly available.
2. Build the TypeScript plugin.
3. Install the plugin into `~/.openclaw/extensions/beagle/`.
4. Add the `channels.beagle` account config and enable the `beagle` plugin
   entry in `~/.openclaw/openclaw.json`.
5. Restart OpenClaw and verify inbound polling plus outbound delivery.

Stub-mode sidecar:

```bash
cd packages/beagle-sidecar
cmake -S . -B build -DBEAGLE_SDK_STUB=ON
cmake --build build
./build/beagle-sidecar --port 39091
```

Plugin build and install:

```bash
cd packages/beagle-channel
npm install
npm run build
mkdir -p ~/.openclaw/extensions/beagle
cp package.json index.js openclaw.plugin.json ~/.openclaw/extensions/beagle/
cp -r dist ~/.openclaw/extensions/beagle/
```

Minimal OpenClaw config template:

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

## Prerequisites

| Requirement | Why it matters |
| --- | --- |
| Git | Required when cloning the integration repo and the Carrier SDK repo |
| Node.js + npm | Required for `packages/beagle-channel` build/install |
| CMake + C++ toolchain | Required for `packages/beagle-sidecar` |
| OpenClaw | Required to load the `beagle` extension |
| Elastos Carrier Native SDK | Required only for non-stub sidecar builds |

If the user does not already have the Carrier SDK checked out, keep work in
stub mode and say so explicitly.

## Safety Notes

- Do not silently modify `~/.openclaw/openclaw.json`, launchd units, or
  systemd units. Present config/service changes as templates for manual review.
- Do not assume the real Carrier SDK is installed. Stub mode is the safe
  default for development and CI-like validation.
- Do not publish this skill as the installable Beagle plugin itself. This
  skill is a companion guide; the actual chat provider still needs to be
  shipped as an OpenClaw package/plugin.

## How To Work In This Repo

### Architecture

- `beagle-sidecar` exposes local HTTP endpoints such as `/health`, `/status`,
  `/sendText`, `/sendMedia`, `/sendStatus`, and `/events`.
- `beagle-channel` talks to that sidecar over HTTP and maps Beagle DMs/groups
  into OpenClaw channel messages.
- Group chat support uses CarrierGroup envelopes:
  - inbound `CGP1`
  - outbound replies `CGR1`
  - status messages `CGS1` for groups and `BGS1` for DMs

### Preferred naming

Use these names consistently unless the user asks otherwise:

- User-facing name: `Beagle Chat`
- Technical plugin/channel id: `beagle`
- Docs / marketplace subtitle: `Beagle Chat (Elastos Carrier)`

### Real SDK build

Only use this path when the user has the Carrier SDK available:

```bash
cd packages/beagle-sidecar
export BEAGLE_SDK_ROOT=~/devs/Elastos.NET.Carrier.Native.SDK
cmake -S . -B build -DBEAGLE_SDK_STUB=OFF -DBEAGLE_SDK_ROOT="$BEAGLE_SDK_ROOT"
cmake --build build
./build/beagle-sidecar --port 39091
```

### Multi-agent routing

- One OpenClaw Beagle account maps to one sidecar account id.
- The plugin automatically sends `X-Beagle-Account: <accountId>` on sidecar
  requests.
- Keep OpenClaw routing/bindings aligned so each Beagle account reaches the
  intended OpenClaw agent.

Example account layout:

```json
{
  "channels": {
    "beagle": {
      "accounts": {
        "main": {
          "enabled": true,
          "sidecarBaseUrl": "http://127.0.0.1:39091",
          "authToken": "devtoken"
        },
        "beagle-profile": {
          "enabled": true,
          "sidecarBaseUrl": "http://127.0.0.1:39091",
          "authToken": "devtoken"
        }
      }
    }
  }
}
```

### Optional features

- Status signaling is off by default. Enable only when the client side can
  parse status envelopes:

```bash
export BEAGLE_STATUS_ENABLED=1
```

- Subscription relay supports:
  - `/channels`
  - `/discover`
  - `/subscribe <channel>`
  - `/unsubscribe <channel>`
  - `/subscriptions`
  - `/subs`

- Subscription persistence defaults to:
  `~/.openclaw/workspace/memory/beagle-channel-subscriptions.json`

## Troubleshooting

- If OpenClaw logs show inbound dispatch without outbound `deliver` or
  `sendText`, the agent route likely produced no final output.
- If the plugin does not load, confirm
  `~/.openclaw/extensions/beagle/` contains `index.js`,
  `openclaw.plugin.json`, `package.json`, and `dist/`.
- If group replies are missing context, inspect the `CGP1`/`CGR1` envelope
  path before changing routing logic.
- If iOS media delivery is unreliable, prefer sidecar media
  `outFormat: "swift-json"` over modes that can fall back to packed payloads.
- Useful logs:
  - `journalctl --user -u openclaw-gateway.service -f`
  - `journalctl --user -u beagle-sidecar.service -f`

## Publishing Guidance

- This folder can be published to ClawHub as a skill because it is a
  self-contained `SKILL.md` bundle with OpenClaw UI metadata.
- The Beagle integration itself is still an OpenClaw channel plugin. To make
  the chat provider installable from ClawHub/OpenClaw, publish the plugin or
  package separately from this companion skill.
- Keep marketplace copy aligned with the repo:
  - mention DMs, group chat, media, and sidecar-based architecture
  - keep the id stable as `beagle`
  - do not claim the skill itself installs background services automatically

## File Reference

| File | Purpose |
| --- | --- |
| `SKILL.md` | Core instructions for operating and maintaining the Beagle OpenClaw integration |
| `agents/openai.yaml` | UI metadata for OpenClaw and ClawHub listing surfaces |
| `assets/beagle-chat-logo.svg` | Beagle Chat icon used by the skill card |
