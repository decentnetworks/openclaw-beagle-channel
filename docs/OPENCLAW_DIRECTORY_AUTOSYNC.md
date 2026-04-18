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
| `BEAGLE_DIRECTORY_AUTOSYNC_ACCOUNTS` | `dirs` | Comma-separated Beagle **`accountId`** values that are "directory accounts". All traffic on these accounts is HTTP-written to the directory regardless of which local OpenClaw agent per-peer bindings route it to. This is the primary filter and handles the common case automatically — peers that advertise an `agentName` (e.g. `beagle-profile`) which OpenClaw mirrors into the route but has no matching local agent still get their profile/presence autosynced. |
| `BEAGLE_DIRECTORY_AUTOSYNC_AGENTS` | `dirs` | Comma-separated OpenClaw **`agentId`** values that may HTTP-write the directory. Additive to the account filter — useful only if you multiplex a single Beagle account across directory + non-directory agents. In the common one-account-per-directory deployment, this variable can be ignored. |
| `CARRIER_FRIEND_INFO_ONE_CONNECTED` | unset | Set to **`1`** only if `friend_info` uses **`1` = connected** (non‑Elastos enum order) |

The directory web server must expose **`POST /tools/directory_upsert`** (no auth in stock directory).

### Do not double-consume `/events`

If the **directory** `web/server.js` **sidecar poller** is enabled (`DIRECTORY_SIDECAR_EVENTS_POLL=1`), it competes with this plugin for the **same drained** `GET /events` queue on the Beagle account. Leave that poller **off** on hosts where OpenClaw beagle-channel is running (default in the directory repo).

### Multi-agent: auto-grow `channels.beagle.accounts`

Beagle-sidecar creates one Carrier identity per entry in `agents.list`. Before auto-grow, the plugin only spawned inbound pollers / ran IDENTITY.md sync for accounts **explicitly** declared under `channels.beagle.accounts`, so extra agents got a Carrier address but **no public profile publish** — the directory stored metadata-only rows with empty `publicProfile`/`identity`.

As of **`895884d`**, the plugin takes the **union** of `channels.beagle.accounts` keys and `agents.list[].id` at startup:

1. **`listAccountIds`** returns every explicit account **plus** every OpenClaw agent id not already in the map. The gateway then spawns one inbound poll loop per id.
2. **`resolveAccount`** for a synthesized id reuses the **first explicit account** as a template (copies `sidecarBaseUrl` and `authToken`). So you only have to declare one beagle account with credentials; the rest are synthesized.
3. A one-shot log line fires at gateway start:
   ```
   [beagle] auto-grew channels.beagle.accounts from agents.list: synthesized=[beagle-profile] template=default — ensure openclaw.json has matching bindings {channel:"beagle",accountId:"<id>"} -> agentId:"<id>" for routing
   ```
4. **Guard:** auto-grow only runs when at least one explicit account exists (its config becomes the template). Zero explicit accounts → legacy behavior (`["default"]`), no surprise traffic.

#### What you still have to declare by hand

**`bindings`** — routing lives in the gateway core, not in this plugin. For each synthesized account you need:

```json
{
  "agentId": "<agentId>",
  "match": { "channel": "beagle", "accountId": "<agentId>" }
}
```

(The convention `accountId === agentId` keeps bindings trivial and lets the existing `resolveIdentityAgentIdFromOpenClawBindings` step pick the right workspace for IDENTITY.md.)

#### Minimal multi-agent config

With two agents (`main`, `beagle-profile`) the whole beagle section collapses to:

```json
"agents": {
  "list": [
    { "id": "main" },
    { "id": "beagle-profile" }
  ]
},
"channels": {
  "beagle": {
    "accounts": {
      "main": {
        "enabled": true,
        "sidecarBaseUrl": "http://127.0.0.1:39091",
        "authToken": "devtoken"
      }
    }
  }
},
"bindings": [
  { "agentId": "main",           "match": { "channel": "beagle", "accountId": "main" } },
  { "agentId": "beagle-profile", "match": { "channel": "beagle", "accountId": "beagle-profile" } }
]
```

The plugin synthesizes `channels.beagle.accounts["beagle-profile"]` at runtime from the `main` template. Workspace resolution still follows `agents.list[].workspace` / `~/.openclaw/workspace-<id>`, so `IDENTITY.md` is read per-agent.

#### Verifying on a host

1. Restart the gateway, grep the journal for `auto-grew channels.beagle.accounts`. If the line doesn't appear, either the agents are all already declared or `agents.list` is empty.
2. Look for `synced IDENTITY.md account=<id> identityAgent=<id> pushed=N` — one line per agent confirms each account ran setPublicProfile.
3. On the directory host, `GET /api/agents/<carrierUserId>` should show a non-empty `publicProfile`/`identity` for each agent.

### Multi-agent: which `IDENTITY.md` is published?

Carrier exposes **one** public profile per Beagle/sidecar account (one Carrier user id). Identity sync reads a single **`IDENTITY.md`** from one OpenClaw workspace.

**Default (no extra keys):** the plugin resolves the agent the same way as [multi-agent routing](https://docs.openclaw.ai/concepts/multi-agent) — it reads top-level **`bindings`** for `channel: "beagle"` and your Beagle **`accountId`** (e.g. `dirs`):

1. An **account-wide** binding (no `match.peer`) → first matching `agentId` wins.  
2. Else, if **every** peer-specific binding for that account uses the **same** `agentId` (typical: one directory friend → one routed agent such as `beagle-profile`) → that agent’s workspace is used.  
3. Else the plugin may call runtime **`resolveAgentRoute`** with a synthetic peer, then falls back to **`agents.defaults`** / **`~/.openclaw/workspace`** (same as before).

So if **`openclaw agents list --bindings`** shows your directory traffic going to `beagle-profile`, identity sync should pick **`~/.openclaw/workspace-beagle-profile/IDENTITY.md`** without adding anything under `channels.beagle.accounts`.

**Optional override:** set **`identityAgentId`** on the Beagle account only when bindings are ambiguous (multiple peer routes to different agents) or you need to force a specific workspace.

Workspace resolution for a chosen agent id follows OpenClaw: `agents.list[].workspace`, `agents.<id>.workspace`, or `~/.openclaw/workspace` / `~/.openclaw/workspace-<id>`.

Then rebuild the beagle-channel extension and restart the gateway.

## Implementation

All logic lives in **`packages/beagle-channel/src/index.ts`**:

- `extractEmbeddedSystemEventJson`
- `normalizeSystemEventPayload`
- `carrierFriendConnToDirectory`
- `maybeUpsertDirectorySystemEvent`
- `maybeUpsertDirectoryProfileMessage` (called from `handleInboundEvent`)
- `collectOpenclawAgentIds` — agents.list reader (auto-grow)
- `listBeagleAccountIdsWithAutoGrow` — union of explicit accounts + agent ids
- `logBeagleAutoGrowOnce` — one-shot startup log of synthesized accounts
- `resolveAccount` — synthesizes configs for agent ids not in `channels.beagle.accounts`

Rebuild and copy **`dist/index.js`** into your OpenClaw beagle extension path, then restart the gateway.

## See also

- `docs/BEAGLE_SIDECAR_PRESENCE_EVENTS.md` — sidecar emission format and C++ references
