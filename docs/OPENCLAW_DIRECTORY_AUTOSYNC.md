# OpenClaw Directory autosync (beagle-channel)

This plugin can persist **presence** and **friend_info** sidecar events to the [OpenClaw Directory](https://github.com/0xli/directory) service **without** relying on the `dirs` LLM to call `exec`/`curl`, while still routing other traffic to the agent.

## Behavior

On each inbound Beagle event, after dedupe and routing resolution:

1. **Normalize** the inbound text: strip control characters, extract embedded JSON when the payload is not a single JSON object, and inject **`peer`** / **`friendInfo.userId`** when missing (sidecar often puts the friend id on the transport envelope only).
2. If the body is a structured system event (`_event` is present), attempt **`maybeUpsertDirectorySystemEvent`**:
   - **Agent guard:** only runs when the routed OpenClaw **`agentId` is exactly `dirs`**. Other Beagle accounts/agents are unchanged.
   - **`presence`:** maps `status` to `connectionStatus` `1` / `0` for the directory HTTP API.
   - **`friend_info`:** maps Carrier connection fields with **`carrierFriendConnToDirectory`**: numeric **`0` / `"0"` = connected** → directory **`connectionStatus: 1`** (online); other numeric values → **`0`** (offline). Non-numeric `status` strings are ignored for connection mapping.
3. If the HTTP POST to **`DIRECTORY_UPSERT_URL`** succeeds, the inbound handler **returns without dispatching the LLM** for that event (avoids duplicate `directory_upsert` calls and unnecessary Carrier replies for system-only payloads).
4. If the POST fails or the event is not handled (e.g. unknown `_event`), normal **`dispatchReplyWithBufferedBlockDispatcher`** runs so the **`dirs`** agent can still process **`profile`** payloads and chat.

## Configuration

| Environment variable | Default | Purpose |
|----------------------|---------|---------|
| `DIRECTORY_UPSERT_URL` | `http://127.0.0.1:3000/tools/directory_upsert` | Directory HTTP tool base (POST JSON body) |

The directory web server must expose **`POST /tools/directory_upsert`** (no auth in stock directory).

## Implementation

All logic lives in **`packages/beagle-channel/src/index.ts`**:

- `extractEmbeddedSystemEventJson`
- `normalizeSystemEventPayload`
- `carrierFriendConnToDirectory`
- `maybeUpsertDirectorySystemEvent` (called from `handleInboundEvent`)

Rebuild and copy **`dist/index.js`** into your OpenClaw beagle extension path, then restart the gateway.

## See also

- `docs/BEAGLE_SIDECAR_PRESENCE_EVENTS.md` — sidecar emission format and C++ references
