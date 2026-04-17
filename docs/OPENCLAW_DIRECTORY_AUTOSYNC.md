# OpenClaw Directory autosync (beagle-channel)

This plugin can persist **presence**, **friend_info**, and **`{"profile":{...}}`** Carrier payloads to the [OpenClaw Directory](https://github.com/0xli/directory) service **without** relying on the `dirs` LLM to call `exec`/`curl`, while still routing other traffic to the agent.

## Behavior

On each inbound Beagle event, after dedupe and routing resolution:

1. **Normalize** the inbound text: strip control characters, extract embedded JSON when the payload is not a single JSON object, and inject **`peer`** / **`friendInfo.userId`** when missing (sidecar often puts the friend id on the transport envelope only).
2. If the body is a structured system event (`_event` is present), attempt **`maybeUpsertDirectorySystemEvent`**:
   - **Agent guard:** only runs when the routed OpenClaw **`agentId` is exactly `dirs`**. Other Beagle accounts/agents are unchanged.
   - **`presence`:** maps `status` to `connectionStatus` `1` / `0` for the directory HTTP API.
   - **`friend_info`:** maps Carrier connection fields with **`carrierFriendConnToDirectory`**: Elastos Carrier enum order is **`0` = connected** → directory **`1`** (online); **`1` = disconnected** → directory **`0`**. Other numeric values are ignored unless they are `0` or `1`. Set **`CARRIER_FRIEND_INFO_ONE_CONNECTED=1`** if your SDK uses **`1` = connected** instead.
3. Else if the body is **`{"profile":{...}}`** (no `_event`), attempt **`maybeUpsertDirectoryProfileMessage`**: maps **`userId`** from the DM peer id and forwards **`publicProfile`**, versions, host fields, etc., so **`publicProfile`** (from IDENTITY.md / `setPublicProfile`) is stored in SQLite.
4. If the HTTP POST to **`DIRECTORY_UPSERT_URL`** succeeds for either handler, the inbound handler **returns without dispatching the LLM** for that event (avoids duplicate `directory_upsert` calls and unnecessary Carrier replies for system-only payloads).
5. If the POST fails or the event is not handled, normal **`dispatchReplyWithBufferedBlockDispatcher`** runs so the **`dirs`** agent can still process chat and non-JSON payloads.

## Configuration

| Environment variable | Default | Purpose |
|----------------------|---------|---------|
| `DIRECTORY_UPSERT_URL` | `http://127.0.0.1:3000/tools/directory_upsert` | Directory HTTP tool base (POST JSON body) |
| `CARRIER_FRIEND_INFO_ONE_CONNECTED` | unset | Set to **`1`** only if `friend_info` uses **`1` = connected** (non‑Elastos enum order) |

The directory web server must expose **`POST /tools/directory_upsert`** (no auth in stock directory).

### Do not double-consume `/events`

If the **directory** `web/server.js` **sidecar poller** is enabled (`DIRECTORY_SIDECAR_EVENTS_POLL=1`), it competes with this plugin for the **same drained** `GET /events` queue on the Beagle account. Leave that poller **off** on hosts where OpenClaw beagle-channel is running (default in the directory repo).

## Implementation

All logic lives in **`packages/beagle-channel/src/index.ts`**:

- `extractEmbeddedSystemEventJson`
- `normalizeSystemEventPayload`
- `carrierFriendConnToDirectory`
- `maybeUpsertDirectorySystemEvent`
- `maybeUpsertDirectoryProfileMessage` (called from `handleInboundEvent`)

Rebuild and copy **`dist/index.js`** into your OpenClaw beagle extension path, then restart the gateway.

## See also

- `docs/BEAGLE_SIDECAR_PRESENCE_EVENTS.md` — sidecar emission format and C++ references
