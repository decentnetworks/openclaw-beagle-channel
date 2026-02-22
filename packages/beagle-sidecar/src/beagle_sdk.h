#pragma once

#include <functional>
#include <string>

struct BeagleIncomingMessage {
  std::string peer;
  std::string text;
  std::string media_path;
  std::string media_url;
  std::string media_type;
  std::string filename;
  unsigned long long size = 0;
  std::string msg_id;
  long long ts = 0;
};

using BeagleIncomingCallback = std::function<void(const BeagleIncomingMessage&)>;

struct BeagleSdkOptions {
  std::string config_path;
  std::string data_dir;
};

struct BeagleStatus {
  bool ready = false;
  bool connected = false;
  std::string last_peer;
  long long last_online_ts = 0;
  long long last_offline_ts = 0;
  unsigned long long online_count = 0;
  unsigned long long offline_count = 0;
};

class BeagleSdk {
public:
  bool start(const BeagleSdkOptions& options, BeagleIncomingCallback on_incoming);
  void stop();

  bool send_text(const std::string& peer, const std::string& text);
  bool send_media(const std::string& peer,
                  const std::string& caption,
                  const std::string& media_path,
                  const std::string& media_url,
                  const std::string& media_type,
                  const std::string& filename,
                  const std::string& out_format = "");
  bool send_status(const std::string& peer,
                   const std::string& state,
                   const std::string& phase,
                   int ttl_ms,
                   const std::string& chat_type,
                   const std::string& group_user_id,
                   const std::string& group_address,
                   const std::string& group_name,
                   const std::string& seq);

  const std::string& userid() const { return user_id_; }
  const std::string& address() const { return address_; }
  BeagleStatus status() const;

private:
  std::string user_id_;
  std::string address_;
};
