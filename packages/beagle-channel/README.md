# Beagle Chat OpenClaw Channel

This package provides a Beagle Chat channel plugin for OpenClaw. It connects to a local Beagle sidecar daemon over HTTP.

**Directory / WAN IP:** OpenClaw Directory entries include `hostIp` and `hostIpExternal` only when the **beagle-sidecar** sends its JSON profile to the directory peer — this plugin does not supply those fields. If `hostIpExternal` is empty in the directory UI, fix it on the machine running the sidecar: set `BEAGLE_EXTERNAL_IP`, or allow outbound HTTPS so the sidecar can resolve the public IP (see repo `INSTALL.md`).

## Config

Use the shape OpenClaw `doctor` expects (2026.x): each account under `channels.beagle.accounts`, `allowFrom` when `dmPolicy` is `open`, and `plugins.allow` so the locally installed extension is explicitly trusted.

```json
{
  "channels": {
    "beagle": {
      "dmPolicy": "open",
      "accounts": {
        "default": {
          "enabled": true,
          "sidecarBaseUrl": "http://127.0.0.1:39091",
          "authToken": "devtoken",
          "allowFrom": ["*"],
          "trustedGroupPeers": ["aHzsSg...6377"],
          "trustedGroupAddresses": ["E9kgtc...REP6"],
          "requireTrustedGroup": false
        }
      }
    }
  },
  "plugins": {
    "allow": ["beagle"],
    "entries": {
      "beagle": { "enabled": true }
    }
  }
}
```

Multi-agent note:

- Define one `accounts.<id>` entry per OpenClaw agent.
- The plugin sends `X-Beagle-Account: <accountId>` on every sidecar request automatically.
- Route each conversation to the correct `accountId` via your OpenClaw channel routing rules.

Example (`main` + `beagle-profile`):

```json
{
  "channels": {
    "beagle": {
      "accounts": {
        "main": {
          "enabled": true,
          "sidecarBaseUrl": "http://127.0.0.1:39091",
          "authToken": "devtoken",
          "allowFrom": ["*"]
        },
        "beagle-profile": {
          "enabled": true,
          "sidecarBaseUrl": "http://127.0.0.1:39091",
          "authToken": "devtoken",
          "allowFrom": ["*"]
        }
      }
    }
  }
}
```

Then keep OpenClaw bindings/routing rules aligned so `accountId=main` routes to agent `main`,
and `accountId=beagle-profile` routes to agent `beagle-profile`.

Add the plugin entry so OpenClaw loads the extension (include `plugins.allow` so `openclaw doctor` does not warn about unlisted local extensions):

```json
{
  "plugins": {
    "allow": ["beagle"],
    "entries": {
      "beagle": { "enabled": true }
    }
  }
}
```

## Install

Build and copy the extension files into `~/.openclaw/extensions/beagle`:

```bash
npm run build
mkdir -p ~/.openclaw/extensions/beagle
cp package.json index.js openclaw.plugin.json ~/.openclaw/extensions/beagle/
cp -r dist ~/.openclaw/extensions/beagle/
```

Restart OpenClaw after installing.

## Supported Features

- Direct chats
- CarrierGroup forwarded group chats (`CGP1` inbound parsing)
- Text messages
- Media (images/files) via `sendMedia`
- Group-context replies encoded as `CGR1` envelope back to CarrierGroup
- Agent status signaling via sidecar `sendStatus` (`CGS1` for group, `BGS1` for DM)

Status signaling is disabled by default to avoid raw `CGS1/BGS1` text in clients that have not implemented status-envelope parsing yet.
Enable it explicitly with:

```bash
export BEAGLE_STATUS_ENABLED=1
```

## Development

```bash
npm install
npm run build
```

## Troubleshooting

- **`openclaw doctor` and Beagle**
  - **`allowFrom`**: With `dmPolicy: "open"`, set `allowFrom: ["*"]` on each `channels.beagle.accounts.<id>` (or run `openclaw doctor --fix`).
  - **`plugins.allow`**: If the plugin lives under `~/.openclaw/extensions/beagle/`, add `"plugins": { "allow": ["beagle"], ... }` so OpenClaw treats it as an explicitly allowed id.
  - **Config shape**: Older single-account keys at `channels.beagle.*` are migrated by doctor into `channels.beagle.accounts.default.*`; prefer the nested form in new configs.
- **`unknown_account` from sidecar (`/events` 404)** — The plugin sends `X-Beagle-Account: <channels.beagle.accounts key>` (often `default` after doctor). The sidecar starts one Carrier stack per **agent id** in `openclaw.json` (`main`, `dirs`, …). If those names differ, you get `unknown_account`. **Fix:** use a current **beagle-sidecar** build (maps `default` → default agent runtime), or rename the Beagle account key to match your agent id (e.g. `main` instead of `default`). See **INSTALL.md** troubleshooting.
- If gateway logs show `dispatch queuedFinal=false` and no `deliver`/`sendText`, the agent route produced no final output for that inbound turn.
- The channel now sends a fallback text when no outbound message was delivered, so group/DM users do not see silent drops.
- Useful logs:
  - `journalctl --user -u openclaw-gateway.service -f`
  - `journalctl --user -u beagle-sidecar.service -f`

## Channel Subscription Commands (BIP-006 bootstrap)

This plugin now supports lightweight local subscription commands via Beagle DM/group chat:

- `/channels` (or `/discover`) — list available channels
- `/subscribe <channel-id-or-name>`
- `/unsubscribe <channel-id-or-name>`
- `/subscriptions` (or `/subs`)

When a subscribed Discord channel receives a new inbound message, the plugin forwards that post to all matching Beagle subscribers. Group subscriptions are delivered back through the CarrierGroup reply envelope path, and DM subscriptions are delivered as direct Beagle messages.

Discord attachment fanout to DM subscribers uses sidecar media `outFormat: "swift-json"` for iOS compatibility. In live testing on March 11, 2026, sidecar `auto` mode could fall back to `packed` media payloads after file-transfer timeout, and iOS subscribers did not render those images reliably.

Implementation note:
- The working relay uses a background Discord REST poller inside the Beagle plugin.
- This is intentional: OpenClaw's `message_received` hook only sees messages that already entered the agent dispatch path, which misses ordinary channel traffic.
- Bot-authored Discord replies are also relayed, so agent responses in subscribed channels reach Beagle subscribers.

### Configure channel discovery

Set `subscribableChannels` under your Beagle account config:

```json
{
  "channels": {
    "beagle": {
      "accounts": {
        "default": {
          "allowFrom": ["*"],
          "subscribableChannels": [
            { "id": "1476753578482995334", "name": "#beagle", "description": "BIP discussions" },
            { "id": "1475346350596947979", "name": "#verify", "description": "verification flow" }
          ]
        }
      }
    }
  }
}
```

### Local storage

Subscriptions are persisted to:
- `~/.openclaw/workspace/memory/beagle-channel-subscriptions.json`

Override path with:
- `BEAGLE_SUBSCRIPTION_STORE_PATH=/custom/path/subscriptions.json`

Store format notes:
- `version: 2` records include `deliveryPeerId` for actual Beagle delivery.
- Group subscriptions also persist `groupUserId` and `groupAddress` so Discord fanout can be sent back into the original Beagle group.

See:
- `docs/BEAGLE_DISCORD_SUBSCRIPTION_RELAY.md`
