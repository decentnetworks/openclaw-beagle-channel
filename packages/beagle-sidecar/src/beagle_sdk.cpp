#include "beagle_sdk.h"

#include <chrono>
#include <ctime>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_set>

#include <string.h>

#if BEAGLE_SDK_STUB

bool BeagleSdk::start(const BeagleSdkOptions& options, BeagleIncomingCallback on_incoming) {
  (void)on_incoming;
  std::cerr << "[beagle-sdk] start stub. data_dir=" << options.data_dir << "\n";
  return true;
}

void BeagleSdk::stop() {
  std::cerr << "[beagle-sdk] stop stub.\n";
}

bool BeagleSdk::send_text(const std::string& peer, const std::string& text) {
  std::cerr << "[beagle-sdk] send_text stub. peer=" << peer << " text=" << text << "\n";
  return true;
}

bool BeagleSdk::send_media(const std::string& peer,
                           const std::string& caption,
                           const std::string& media_path,
                           const std::string& media_url,
                           const std::string& media_type,
                           const std::string& filename) {
  std::cerr << "[beagle-sdk] send_media stub. peer=" << peer
            << " caption=" << caption
            << " media_path=" << media_path
            << " media_url=" << media_url
            << " media_type=" << media_type
            << " filename=" << filename << "\n";
  return true;
}

#else

extern "C" {
#include <carrier.h>
#include <carrier_config.h>
}

namespace {
struct RuntimeState {
  Carrier* carrier = nullptr;
  BeagleIncomingCallback on_incoming;
  std::thread loop_thread;
  std::mutex state_mu;
  std::string persistent_location;
  std::string user_id;
  std::string address;
  BeagleStatus status;
  std::unordered_set<std::string> welcomed_peers;
};

static void send_welcome_once(RuntimeState* state, const std::string& peer, const char* reason) {
  if (!state || peer.empty() || !state->carrier) return;
  {
    std::lock_guard<std::mutex> lock(state->state_mu);
    if (state->welcomed_peers.find(peer) != state->welcomed_peers.end()) return;
    state->welcomed_peers.insert(peer);
  }

  const std::string msg =
      "Hi! I'm the Beagle OpenClaw bot. Send a message to start.";
  uint32_t msgid = 0;
  int rc = carrier_send_friend_message(state->carrier,
                                       peer.c_str(),
                                       msg.data(),
                                       msg.size(),
                                       &msgid,
                                       nullptr,
                                       nullptr);
  if (rc < 0) {
    std::cerr << "[beagle-sdk] welcome message failed (" << reason
              << "): 0x" << std::hex << carrier_get_error() << std::dec << "\n";
  } else {
    std::cerr << "[beagle-sdk] welcome message sent (" << reason
              << ") to " << peer << "\n";
  }
}

void friend_message_callback(Carrier* carrier,
                             const char* from,
                             const void* msg,
                             size_t len,
                             int64_t timestamp,
                             bool offline,
                             void* context) {
  (void)carrier;
  auto* state = static_cast<RuntimeState*>(context);
  if (!state || !state->on_incoming) return;

  BeagleIncomingMessage incoming;
  incoming.peer = from ? from : "";
  incoming.text.assign(static_cast<const char*>(msg), len);
  incoming.ts = timestamp;
  state->on_incoming(incoming);

  {
    std::lock_guard<std::mutex> lock(state->state_mu);
    state->status.last_peer = incoming.peer;
    if (offline) {
      state->status.offline_count++;
      state->status.last_offline_ts = timestamp;
    } else {
      state->status.online_count++;
      state->status.last_online_ts = timestamp;
    }
  }

  std::cerr << "[beagle-sdk] message (" << (offline ? "offline" : "online")
            << ") from " << incoming.peer << ": " << incoming.text << "\n";
}

void friend_request_callback(Carrier* carrier,
                             const char* userid,
                             const CarrierUserInfo* info,
                             const char* hello,
                             void* context) {
  (void)info;
  (void)hello;
  auto* state = static_cast<RuntimeState*>(context);
  if (!carrier || !userid) return;
  int rc = carrier_accept_friend(carrier, userid);
  if (rc < 0) {
    std::cerr << "[beagle-sdk] accept friend failed: 0x" << std::hex << carrier_get_error() << std::dec << "\n";
  } else {
    std::cerr << "[beagle-sdk] accepted friend: " << userid << "\n";
    send_welcome_once(state, userid, "accepted");
  }
}

void connection_status_callback(Carrier* carrier,
                                CarrierConnectionStatus status,
                                void* context) {
  (void)carrier;
  auto* state = static_cast<RuntimeState*>(context);
  if (state) {
    std::lock_guard<std::mutex> lock(state->state_mu);
    state->status.connected = (status == CarrierConnectionStatus_Connected);
  }
  std::cerr << "[beagle-sdk] connection status: "
            << (status == CarrierConnectionStatus_Connected ? "connected" : "disconnected")
            << "\n";
}

void ready_callback(Carrier* carrier, void* context) {
  (void)carrier;
  auto* state = static_cast<RuntimeState*>(context);
  if (state) {
    std::lock_guard<std::mutex> lock(state->state_mu);
    state->status.ready = true;
  }
  std::cerr << "[beagle-sdk] ready\n";
}

void friend_connection_callback(Carrier* carrier,
                                const char* friendid,
                                CarrierConnectionStatus status,
                                void* context) {
  (void)carrier;
  auto* state = static_cast<RuntimeState*>(context);
  std::cerr << "[beagle-sdk] friend " << (friendid ? friendid : "")
            << " is " << (status == CarrierConnectionStatus_Connected ? "online" : "offline")
            << "\n";
  if (status == CarrierConnectionStatus_Connected && friendid) {
    send_welcome_once(state, friendid, "online");
  }
}

void friend_invite_callback(Carrier* carrier,
                            const char* from,
                            const char* bundle,
                            const void* data,
                            size_t len,
                            void* context) {
  (void)carrier;
  (void)bundle;
  auto* state = static_cast<RuntimeState*>(context);
  std::string payload;
  if (data && len) payload.assign(static_cast<const char*>(data), len);
  std::cerr << "[beagle-sdk] invite from " << (from ? from : "")
            << " data=" << payload << "\n";

  if (!state || !state->on_incoming || !from) return;
  BeagleIncomingMessage incoming;
  incoming.peer = from;
  incoming.text = payload.empty() ? "[invite]" : payload;
  incoming.ts = static_cast<long long>(std::time(nullptr));
  state->on_incoming(incoming);

  {
    std::lock_guard<std::mutex> lock(state->state_mu);
    state->status.last_peer = incoming.peer;
    state->status.online_count++;
    state->status.last_online_ts = incoming.ts;
  }
}
} // namespace

static RuntimeState g_state;

bool BeagleSdk::start(const BeagleSdkOptions& options, BeagleIncomingCallback on_incoming) {
  if (options.config_path.empty()) {
    std::cerr << "[beagle-sdk] missing config file path\n";
    return false;
  }

  CarrierOptions opts;
  if (!carrier_config_load(options.config_path.c_str(), nullptr, &opts)) {
    std::cerr << "[beagle-sdk] failed to load config: " << options.config_path << "\n";
    return false;
  }

  if (!options.data_dir.empty()) {
    g_state.persistent_location = options.data_dir;
    // carrier_config_free() will free this field, so allocate with strdup.
    opts.persistent_location = strdup(g_state.persistent_location.c_str());
  }

  CarrierCallbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.connection_status = connection_status_callback;
  callbacks.ready = ready_callback;
  callbacks.friend_connection = friend_connection_callback;
  callbacks.friend_message = friend_message_callback;
  callbacks.friend_request = friend_request_callback;
  callbacks.friend_invite = friend_invite_callback;

  g_state.on_incoming = std::move(on_incoming);

  Carrier* carrier = carrier_new(&opts, &callbacks, &g_state);
  carrier_config_free(&opts);
  if (!carrier) {
    std::cerr << "[beagle-sdk] carrier_new failed: 0x" << std::hex << carrier_get_error() << std::dec << "\n";
    return false;
  }

  g_state.carrier = carrier;

  char buf[CARRIER_MAX_ADDRESS_LEN + 1] = {0};
  char idbuf[CARRIER_MAX_ID_LEN + 1] = {0};
  carrier_get_userid(carrier, idbuf, sizeof(idbuf));
  carrier_get_address(carrier, buf, sizeof(buf));
  g_state.user_id = idbuf;
  g_state.address = buf;
  user_id_ = g_state.user_id;
  address_ = g_state.address;

  std::cerr << "[beagle-sdk] User ID: " << user_id_ << "\n";
  std::cerr << "[beagle-sdk] Address: " << address_ << "\n";

  g_state.loop_thread = std::thread([]() {
    int rc = carrier_run(g_state.carrier, 10);
    if (rc != 0) {
      std::cerr << "[beagle-sdk] carrier_run failed: 0x" << std::hex << carrier_get_error() << std::dec << "\n";
    }
  });

  return true;
}

void BeagleSdk::stop() {
  if (!g_state.carrier) return;
  carrier_kill(g_state.carrier);
  if (g_state.loop_thread.joinable()) g_state.loop_thread.join();
  g_state.carrier = nullptr;
}

bool BeagleSdk::send_text(const std::string& peer, const std::string& text) {
  if (!g_state.carrier) return false;
  uint32_t msgid = 0;
  int rc = carrier_send_friend_message(g_state.carrier,
                                       peer.c_str(),
                                       text.data(),
                                       text.size(),
                                       &msgid,
                                       nullptr,
                                       nullptr);
  if (rc < 0) {
    std::cerr << "[beagle-sdk] send_text failed: 0x" << std::hex << carrier_get_error() << std::dec << "\n";
    return false;
  }
  std::cerr << "[beagle-sdk] send_text ok msgid=" << msgid << " peer=" << peer << "\n";
  return true;
}

bool BeagleSdk::send_media(const std::string& peer,
                           const std::string& caption,
                           const std::string& media_path,
                           const std::string& media_url,
                           const std::string& media_type,
                           const std::string& filename) {
  std::string payload;
  if (!caption.empty()) payload += caption;
  if (!media_url.empty()) {
    if (!payload.empty()) payload += "\n";
    payload += media_url;
  } else if (!media_path.empty()) {
    if (!payload.empty()) payload += "\n";
    payload += media_path;
  }
  if (!filename.empty()) {
    if (!payload.empty()) payload += "\n";
    payload += "filename: " + filename;
  }
  if (!media_type.empty()) {
    if (!payload.empty()) payload += "\n";
    payload += "mediaType: " + media_type;
  }

  return send_text(peer, payload);
}

BeagleStatus BeagleSdk::status() const {
  std::lock_guard<std::mutex> lock(g_state.state_mu);
  return g_state.status;
}

#endif
