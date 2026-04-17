# Beagle Sidecar Presence Events

## Overview

The sidecar emits presence events via the `/events` endpoint when friends' online/offline status changes. This is controlled by the `--emit-presence` flag or `BEAGLE_EMIT_PRESENCE=1` environment variable.

## How Presence Events Work

### Startup: `friend_list_callback`

When the Carrier SDK initializes, it calls `carrier_get_friends()` which invokes `friend_list_callback` for each known friend. **At this point the sidecar emits `presence` events for ALL friends** — not just new ones. This ensures:

- The directory gets initial presence status for all existing friends at startup
- Friends who were already online before the sidecar started are correctly marked as online

```cpp
// beagle_sdk.cpp:friend_list_callback
bool friend_list_callback(const CarrierFriendInfo* info, void* context) {
  // ...store friend info...
  emit_friend_info_event(state, fid, info);

  // Emit presence for ALL friends at startup (not just state transitions)
  if (state->emit_presence && state->on_incoming) {
    bool is_online = (info->status == CarrierConnectionStatus_Connected);
    BeagleIncomingMessage ev;
    ev.peer = fid;
    ev.text = std::string("{\"_event\":\"presence\",\"status\":\"")
              + (is_online ? "online" : "offline") + "\"}";
    state->on_incoming(ev);
  }
  return true;
}
```

### Runtime: `friend_connection_callback`

When a friend's connection state changes (goes online or offline), `friend_connection_callback` fires and emits a presence event:

```cpp
void friend_connection_callback(Carrier* carrier, const char* friendid,
                                CarrierConnectionStatus status, void* context) {
  bool is_online = (status == CarrierConnectionStatus_Connected);
  if (state->emit_presence && state->on_incoming) {
    BeagleIncomingMessage ev;
    ev.peer = friendid;
    ev.text = std::string("{\"_event\":\"presence\",\"status\":\"")
              + (is_online ? "online" : "offline") + "\"}";
    state->on_incoming(ev);
  }
}
```

## Event Format

Presence events are JSON messages delivered via `GET /events` or `GET /directory-events`:

```json
{"_event":"presence","status":"online","peer":"FhNk8xeE8M85t2bDFRWdJWEbxzKQaQRmB9NY7ghbcRGo"}
{"_event":"presence","status":"offline","peer":"FhNk8xeE8M85t2bDFRWdJWEbxzKQaQRmB9NY7ghbcRGo"}
```

Friend info events are also emitted:

```json
{"_event":"friend_info","peer":"...","userInfo":{"name":"...","gender":"...","phone":"...","email":"...","region":"..."},"friendInfo":{"label":"...","connectionStatus":0,"presenceStatus":0}}
```

## Directory Auto-Bootstrap

When the sidecar starts, it automatically adds the OpenClaw directory as a Carrier friend (unless already friends) and pushes the agent's profile. The `has_friend()` method checks if the directory is already a friend before attempting to add it, avoiding "friend already exist" warnings.

## Key Points

1. **Presence is emitted at startup for ALL friends** — `friend_list_callback` iterates all existing friends and emits presence for each one
2. **Presence is also emitted on state transitions** — `friend_connection_callback` fires when a friend goes online or offline
3. **The `/events` endpoint is drained by consumers** — both the directory poller and beagle-channel consume from the same queue
4. **Use `/directory-events` for exclusive access** — this mirrored queue ensures the directory never races with beagle-channel on event consumption (optional; some deployments poll only `/events` with a single consumer per account).

## OpenClaw directory integration

When hosting the [OpenClaw Directory](https://github.com/0xli/directory) alongside OpenClaw, use the beagle-channel **HTTP autosync** path for `presence` / `friend_info` (see **`docs/OPENCLAW_DIRECTORY_AUTOSYNC.md`**). Presence JSON from the C++ snippets above often **omits `peer` inside `ev.text`**; the plugin injects the transport peer id before persisting.
