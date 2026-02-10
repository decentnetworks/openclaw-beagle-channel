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
  std::string msg_id;
  long long ts = 0;
};

using BeagleIncomingCallback = std::function<void(const BeagleIncomingMessage&)>;

struct BeagleSdkOptions {
  std::string data_dir;
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
                  const std::string& filename);
};
