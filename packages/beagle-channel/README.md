# Beagle Chat OpenClaw Channel

This package provides a Beagle Chat channel plugin for OpenClaw. It connects to a local Beagle sidecar daemon over HTTP.

## Config

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
          "trustedGroupPeers": ["aHzsSg...6377"],
          "trustedGroupAddresses": ["E9kgtc...REP6"],
          "requireTrustedGroup": false
        }
      }
    }
  }
}
```

Add the plugin entry so OpenClaw loads the extension:

```json
{
  "plugins": {
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
