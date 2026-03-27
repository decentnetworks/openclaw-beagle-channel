# Beagle Sidecar Push Notifications

This sidecar can notify the Beagle mobile app when a direct text message hits Carrier's offline-delivery path.

The feature is opt-in and configured only from a local runtime file:

- `beagle_push.json`

The sidecar creates this file under the sidecar `--data-dir` on first start. It is intended for local secrets and should not be committed.

## What It Does

When `sendText` hits Carrier's `friend offline` error:

- the sidecar keeps the existing Express fallback behavior
- the sidecar can also call the push API `notification` endpoint
- the sidecar auto-registers itself through `registerPush` at startup if enabled

Notification spam control is built in:

- `first_until_online`: notify only the first offline text for a peer, then suppress more until that peer becomes online again
- `interval`: notify at most once per peer per configured interval

Suppression resets automatically when the friend connection callback reports that peer as online again.

## Config File

Example `beagle_push.json`:

```json
{
  "enabled": true,
  "registerOnStart": true,
  "env": "prod",
  "appName": "openclaw-beagle-sidecar",
  "token": "",
  "tokenType": "sidecar",
  "brand": "OpenClaw",
  "model": "Beagle Sidecar",
  "os": "macOS",
  "osVersion": "",
  "notificationMode": "first_until_online",
  "notificationMinIntervalSeconds": 300,
  "pushapiServers": [
    {
      "url": "https://pushapi.beagle.chat",
      "appKey": "08726e938c624c4da9dcd9a85f2bbcd8",
      "registerPush": "/push-api/register",
      "notification": "/push-api/notification",
      "push": "/push-api/push-message"
    }
  ]
}
```

Fields:

- `enabled`: master switch for the whole feature
- `registerOnStart`: if `true`, call `registerPush` during sidecar startup
- `env`: forwarded to push API requests, usually `prod`
- `appName`: forwarded to push API requests
- `token`: registration token; if empty the sidecar uses `sidecar:<carrierUserId>`
- `tokenType`: registration token type, default `sidecar`
- `brand`, `model`, `os`, `osVersion`: registration metadata
- `notificationMode`: `first_until_online` or `interval`
- `notificationMinIntervalSeconds`: used only for `interval`
- `pushapiServers`: one or more push backends to try in order

## Runtime Behavior

Startup:

- sidecar loads `beagle_push.json`
- if enabled, it posts to `registerPush`

Offline direct text:

- sidecar attempts `carrier_send_friend_message`
- if Carrier returns `ERROR_FRIEND_OFFLINE`, sidecar may post to `notification`
- sidecar then continues with the existing Express fallback path

Online reset:

- when Carrier reports that friend as online again, sidecar clears that peer's suppression state

## Important Limits

- This only applies to normal direct text sends.
- It does not trigger for sidecar status envelopes.
- It does not trigger for media fallback text payloads.
- Group delivery is not part of this flow.

## Local Test Flow

1. Build and run the real sidecar with a writable `--data-dir`.
2. Start it once so `beagle_push.json` is created.
3. Edit `beagle_push.json` locally and set `enabled: true`.
4. Restart the sidecar so registration runs again.
5. Send a direct text to a friend who is offline.
6. Check sidecar logs for:
   - `push register ok`
   - `offline notification ok`

## File Locations

With `--data-dir ~/.carrier`, the relevant files are:

- `~/.carrier/beagle_push.json`
- `~/.carrier/friend_state.tsv`
- `~/.carrier/friend_events.log`

## Code References

- push config structures and loader: `packages/beagle-sidecar/src/beagle_sdk.cpp`
- offline notification decision and API POST: `packages/beagle-sidecar/src/beagle_sdk.cpp`
- startup registration: `packages/beagle-sidecar/src/beagle_sdk.cpp`
- suppression reset on friend online: `packages/beagle-sidecar/src/beagle_sdk.cpp`
