# Beagle Chat OpenClaw Channel

This package provides a Beagle Chat channel plugin for OpenClaw. It connects to a local Beagle sidecar daemon over HTTP.

## Config

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

## Supported Features

- Direct chats
- Text messages
- Media (images/files) via `sendMedia`

## Development

```bash
npm install
npm run build
```
