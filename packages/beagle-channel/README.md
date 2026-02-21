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
