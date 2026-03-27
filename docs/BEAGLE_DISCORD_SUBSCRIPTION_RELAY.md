## Beagle Discord Subscription Relay

This note documents the final working relay path for BIP-006 style Beagle subscriptions.

### Problem

The initial implementation persisted `/subscribe` records correctly, but subscribed Beagle users did not receive normal Discord channel traffic from `#beagle` or `#verify`.

Two separate issues caused that:

1. The first relay attempt used the OpenClaw plugin hook `api.on("message_received", ...)`.
   That hook only fires inside OpenClaw's inbound reply-dispatch path, after Discord mention gating and routing.
   Result: ordinary channel posts that never entered the agent pipeline were invisible to the relay.

2. The first raw Discord relay pass filtered out messages authored by the Discord bot user.
   Result: human Discord messages relayed, but agent replies from `@noodles` did not.

### Final Fix

The Beagle plugin now runs its own background relay service:

- polls Discord channel history directly through the Discord REST API
- watches only channels that currently have Beagle subscribers
- deduplicates relayed messages by Discord channel/message id
- forwards both human-authored and bot-authored channel messages
- delivers DM subscriptions as direct Beagle messages
- delivers group subscriptions as `CGR1` CarrierGroup reply envelopes

For Discord attachment fanout to Beagle DM subscribers, the relay now explicitly requests
sidecar media `outFormat: "swift-json"` instead of relying on sidecar `auto` mode.
Reason: in live testing on March 11, 2026, `auto` fell back from Carrier file-transfer
to `packed` payloads, and subscribed iOS Beagle clients received the surrounding text
messages but did not render the packed image payload. For subscription media, `swift-json`
restored successful image delivery on iOS.

Implementation lives in:

- `packages/beagle-channel/src/index.ts`

### Subscription Store

The store remains:

- `~/.openclaw/workspace/memory/beagle-channel-subscriptions.json`

The plugin now normalizes records toward `version: 2`, adding:

- `deliveryPeerId`
- `groupUserId`
- `groupAddress`
- `groupName`

This allows group subscriptions to relay back into the correct Beagle group peer/address instead of only supporting DM fanout.

### Verification

Working verification on this host:

- subscribed Beagle iOS client received Discord user messages from subscribed channels
- subscribed Beagle iOS client received Discord bot / agent replies from subscribed channels
- subscribed Beagle iOS client received Discord subscription images after switching relay media output to `swift-json`
- direct Beagle delivery path was confirmed independently with a relay-debug message

### Useful Logs

- active gateway log on macOS launchd installs:
  - `/tmp/openclaw/openclaw-YYYY-MM-DD.log`
- legacy gateway logs:
  - `~/.openclaw/logs/gateway.log`
  - `~/.openclaw/logs/gateway.err.log`

Look for:

- `subscription fanout delivered`
- `subscription fanout sendMedia`
- `subscription fanout failed`
- `discord subscription poll failed`
