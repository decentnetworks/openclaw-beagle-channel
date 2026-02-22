#include "beagle_sdk.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct Event {
  std::string peer;
  std::string text;
  std::string media_url;
  std::string media_path;
  std::string media_type;
  std::string filename;
  unsigned long long size = 0;
  std::string msg_id;
  long long ts = 0;
};

static std::string log_ts() {
  std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&now, &tm_buf);
  char out[32];
  if (std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &tm_buf) == 0) return "";
  return std::string(out);
}

static void log_line(const std::string& msg) {
  std::cerr << "[" << log_ts() << "] " << msg << "\n";
}

static std::mutex g_events_mu;
static std::vector<Event> g_events;

static bool decode_json_string(const std::string& body, size_t start, std::string& out, size_t& end_pos) {
  if (start >= body.size() || body[start] != '"') return false;
  out.clear();
  for (size_t i = start + 1; i < body.size(); ++i) {
    char c = body[i];
    if (c == '"') {
      end_pos = i;
      return true;
    }
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (i + 1 >= body.size()) return false;
    char esc = body[++i];
    switch (esc) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'u': {
        if (i + 4 >= body.size()) return false;
        int code = 0;
        for (int k = 0; k < 4; ++k) {
          char h = body[i + 1 + k];
          code <<= 4;
          if (h >= '0' && h <= '9') code |= (h - '0');
          else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
          else return false;
        }
        i += 4;
        if (code <= 0x7F) {
          out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
          out.push_back(static_cast<char>(0xC0 | ((code >> 6) & 0x1F)));
          out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xE0 | ((code >> 12) & 0x0F)));
          out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

static bool extract_json_string(const std::string& body, const std::string& key, std::string& out) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return false;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos = body.find('"', pos);
  if (pos == std::string::npos) return false;
  size_t end_pos = pos;
  return decode_json_string(body, pos, out, end_pos);
}

static bool extract_json_int(const std::string& body, const std::string& key, int& out) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return false;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  ++pos;
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\r' || body[pos] == '\n')) {
    ++pos;
  }
  if (pos >= body.size()) return false;

  if (body[pos] == '"') {
    std::string text;
    size_t end_pos = pos;
    if (!decode_json_string(body, pos, text, end_pos)) return false;
    try {
      out = std::stoi(text);
      return true;
    } catch (...) {
      return false;
    }
  }

  size_t end = pos;
  if (body[end] == '-') ++end;
  while (end < body.size() && std::isdigit(static_cast<unsigned char>(body[end]))) ++end;
  if (end == pos) return false;
  try {
    out = std::stoi(body.substr(pos, end - pos));
    return true;
  } catch (...) {
    return false;
  }
}

static std::string json_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

static std::string events_to_json(std::vector<Event> events) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < events.size(); ++i) {
    const auto& ev = events[i];
    if (i) oss << ",";
    oss << "{"
        << "\"peer\":\"" << json_escape(ev.peer) << "\"";
    if (!ev.text.empty()) oss << ",\"text\":\"" << json_escape(ev.text) << "\"";
    if (!ev.media_url.empty()) oss << ",\"mediaUrl\":\"" << json_escape(ev.media_url) << "\"";
    if (!ev.media_path.empty()) oss << ",\"mediaPath\":\"" << json_escape(ev.media_path) << "\"";
    if (!ev.media_type.empty()) oss << ",\"mediaType\":\"" << json_escape(ev.media_type) << "\"";
    if (!ev.filename.empty()) oss << ",\"filename\":\"" << json_escape(ev.filename) << "\"";
    if (ev.size > 0) oss << ",\"size\":" << ev.size;
    if (!ev.msg_id.empty()) oss << ",\"msgId\":\"" << json_escape(ev.msg_id) << "\"";
    if (ev.ts != 0) oss << ",\"ts\":" << ev.ts;
    oss << "}";
  }
  oss << "]";
  return oss.str();
}

static std::string read_until(int fd, const std::string& marker) {
  std::string data;
  char buf[4096];
  while (data.find(marker) == std::string::npos) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    data.append(buf, buf + n);
  }
  return data;
}

static int get_content_length(const std::string& headers) {
  std::string lower = headers;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  std::string needle = "content-length:";
  size_t pos = lower.find(needle);
  if (pos == std::string::npos) return 0;
  pos += needle.size();
  while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '\t')) pos++;
  size_t end = lower.find("\r\n", pos);
  if (end == std::string::npos) end = lower.size();
  return std::atoi(lower.substr(pos, end - pos).c_str());
}

static std::string header_value(const std::string& headers, const std::string& key) {
  std::string needle = key + ":";
  size_t pos = headers.find(needle);
  if (pos == std::string::npos) return "";
  pos += needle.size();
  while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) pos++;
  size_t end = headers.find("\r\n", pos);
  if (end == std::string::npos) end = headers.size();
  return headers.substr(pos, end - pos);
}

static void send_response(int fd, int code, const std::string& content_type, const std::string& body) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << code << " " << (code == 200 ? "OK" : "ERROR") << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << body;
  std::string out = oss.str();
  send(fd, out.c_str(), out.size(), 0);
}

static std::string client_ip(int fd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "";
  char buf[INET_ADDRSTRLEN] = {0};
  if (!inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf))) return "";
  return std::string(buf);
}

static std::string to_iso8601(long long ts) {
  if (ts <= 0) return "";
  time_t t = static_cast<time_t>(ts);
  struct tm tm_buf;
  if (!localtime_r(&t, &tm_buf)) return "";
  char out[32];
  if (strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &tm_buf) == 0) return "";
  return std::string(out);
}

static void push_event(const BeagleIncomingMessage& msg) {
  Event ev;
  ev.peer = msg.peer;
  ev.text = msg.text;
  ev.media_path = msg.media_path;
  ev.media_url = msg.media_url;
  ev.media_type = msg.media_type;
  ev.filename = msg.filename;
  ev.size = msg.size;
  ev.msg_id = msg.msg_id;
  ev.ts = msg.ts;

  std::lock_guard<std::mutex> lock(g_events_mu);
  g_events.push_back(std::move(ev));
  log_line(std::string("[sidecar] queued event peer=") + msg.peer
           + " text_len=" + std::to_string(msg.text.size())
           + " ts=" + std::to_string(msg.ts));
}

struct ServerOptions {
  int port = 39091;
  std::string token;
  std::string data_dir = "./data";
  std::string config_path;
};

static ServerOptions parse_args(int argc, char** argv) {
  ServerOptions opts;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      opts.port = std::atoi(argv[++i]);
    } else if (arg == "--token" && i + 1 < argc) {
      opts.token = argv[++i];
    } else if (arg == "--data-dir" && i + 1 < argc) {
      opts.data_dir = argv[++i];
    } else if (arg == "--config" && i + 1 < argc) {
      opts.config_path = argv[++i];
    }
  }
  return opts;
}

static std::string get_env(const char* key) {
  const char* value = std::getenv(key);
  return value ? std::string(value) : std::string();
}

static bool file_exists(const std::string& path) {
  return access(path.c_str(), R_OK) == 0;
}

static std::string resolve_config_path(const ServerOptions& opts) {
  if (!opts.config_path.empty()) return opts.config_path;

  std::string env_config = get_env("BEAGLE_CONFIG");
  if (!env_config.empty()) return env_config;

  std::string sdk_root = get_env("BEAGLE_SDK_ROOT");
  if (!sdk_root.empty()) {
    std::string candidate = sdk_root + "/config/carrier.conf";
    if (file_exists(candidate)) return candidate;
  }

  if (file_exists("./carrier.conf")) return "./carrier.conf";

  return std::string();
}

int main(int argc, char** argv) {
  ServerOptions opts = parse_args(argc, argv);
  std::string config_path = resolve_config_path(opts);
  if (config_path.empty()) {
    log_line("Missing Carrier config. Provide --config or set BEAGLE_SDK_ROOT.");
    return 1;
  }

  BeagleSdk sdk;
  if (!sdk.start({config_path, opts.data_dir}, push_event)) {
    log_line("Failed to start Beagle SDK");
    return 1;
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    log_line("Failed to create socket");
    return 1;
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(opts.port));

  if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    log_line("Bind failed");
    return 1;
  }

  if (listen(server_fd, 16) < 0) {
    log_line("Listen failed");
    return 1;
  }

  log_line(std::string("Beagle sidecar listening on 0.0.0.0:") + std::to_string(opts.port));

  while (true) {
    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) continue;

    std::string request = read_until(client_fd, "\r\n\r\n");
    size_t header_end = request.find("\r\n\r\n");
    std::string headers = header_end == std::string::npos ? request : request.substr(0, header_end + 2);

    int content_length = get_content_length(headers);
    std::string body;
    if (content_length > 0) {
      body = request.substr(header_end + 4);
      while (static_cast<int>(body.size()) < content_length) {
        char buf[4096];
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, buf + n);
      }
    }

    std::istringstream line_stream(request);
    std::string method;
    std::string path;
    line_stream >> method >> path;

    if (!opts.token.empty()) {
      std::string auth = header_value(headers, "Authorization");
      std::string expected = "Bearer " + opts.token;
      if (auth != expected) {
        log_line(std::string("[sidecar] unauthorized request for ") + path
                 + " from " + client_ip(client_fd));
        send_response(client_fd, 401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
        close(client_fd);
        continue;
      }
    }

    if (method == "GET" && path == "/health") {
      std::ostringstream oss;
      oss << "{"
          << "\"ok\":true"
          << ",\"userId\":\"" << json_escape(sdk.userid()) << "\""
          << ",\"address\":\"" << json_escape(sdk.address()) << "\""
          << "}";
      send_response(client_fd, 200, "application/json", oss.str());
    } else if (method == "GET" && path == "/status") {
      BeagleStatus status = sdk.status();
      std::string last_online_human = to_iso8601(status.last_online_ts);
      std::string last_offline_human = to_iso8601(status.last_offline_ts);
      std::ostringstream oss;
      oss << "{"
          << "\"ok\":true"
          << ",\"ready\":" << (status.ready ? "true" : "false")
          << ",\"connected\":" << (status.connected ? "true" : "false")
          << ",\"lastPeer\":\"" << json_escape(status.last_peer) << "\""
          << ",\"lastOnlineTs\":" << status.last_online_ts
          << ",\"lastOfflineTs\":" << status.last_offline_ts
          << ",\"lastOnline\":\"" << json_escape(last_online_human) << "\""
          << ",\"lastOffline\":\"" << json_escape(last_offline_human) << "\""
          << ",\"onlineCount\":" << status.online_count
          << ",\"offlineCount\":" << status.offline_count
          << "}";
      send_response(client_fd, 200, "application/json", oss.str());
    } else if (method == "GET" && path == "/events") {
      std::vector<Event> events;
      {
        std::lock_guard<std::mutex> lock(g_events_mu);
        events.swap(g_events);
      }
      if (!events.empty()) {
        std::string ua = header_value(headers, "User-Agent");
        std::ostringstream msg;
        msg << "[sidecar] /events -> " << events.size() << " event(s)"
            << " from " << client_ip(client_fd);
        if (!ua.empty()) msg << " ua=" << ua;
        log_line(msg.str());
      }
      send_response(client_fd, 200, "application/json", events_to_json(std::move(events)));
    } else if (method == "POST" && path == "/sendText") {
      std::string peer;
      std::string text;
      extract_json_string(body, "peer", peer);
      extract_json_string(body, "text", text);
      log_line(std::string("[sidecar] /sendText peer=") + peer
               + " text_len=" + std::to_string(text.size()));

      bool ok = sdk.send_text(peer, text);
      send_response(client_fd, ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (method == "POST" && path == "/sendMedia") {
      std::string peer;
      std::string caption;
      std::string media_path;
      std::string media_url;
      std::string media_type;
      std::string filename;
      std::string out_format;
      extract_json_string(body, "peer", peer);
      extract_json_string(body, "caption", caption);
      extract_json_string(body, "mediaPath", media_path);
      extract_json_string(body, "mediaUrl", media_url);
      extract_json_string(body, "mediaType", media_type);
      extract_json_string(body, "filename", filename);
      extract_json_string(body, "outFormat", out_format);
      log_line(std::string("[sidecar] /sendMedia peer=") + peer
               + " caption_len=" + std::to_string(caption.size())
               + " media_url_len=" + std::to_string(media_url.size())
               + " media_path_len=" + std::to_string(media_path.size())
               + " out_format=" + (out_format.empty() ? "(default)" : out_format));

      bool ok = sdk.send_media(peer, caption, media_path, media_url, media_type, filename, out_format);
      send_response(client_fd, ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (method == "POST" && path == "/sendStatus") {
      std::string peer;
      std::string state;
      std::string phase;
      std::string chat_type;
      std::string group_user_id;
      std::string group_address;
      std::string group_name;
      std::string seq;
      int ttl_ms = 12000;
      extract_json_string(body, "peer", peer);
      extract_json_string(body, "state", state);
      extract_json_string(body, "phase", phase);
      extract_json_string(body, "chatType", chat_type);
      extract_json_string(body, "groupUserId", group_user_id);
      extract_json_string(body, "groupAddress", group_address);
      extract_json_string(body, "groupName", group_name);
      extract_json_string(body, "seq", seq);
      int parsed_ttl = 0;
      if (extract_json_int(body, "ttlMs", parsed_ttl) && parsed_ttl > 0) ttl_ms = parsed_ttl;
      log_line(std::string("[sidecar] /sendStatus peer=") + peer
               + " state=" + state
               + " phase=" + phase
               + " chat_type=" + (chat_type.empty() ? "(auto)" : chat_type)
               + " ttl_ms=" + std::to_string(ttl_ms));
      bool ok = sdk.send_status(peer,
                                state,
                                phase,
                                ttl_ms,
                                chat_type,
                                group_user_id,
                                group_address,
                                group_name,
                                seq);
      send_response(client_fd, ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else {
      send_response(client_fd, 404, "application/json", "{\"ok\":false,\"error\":\"not_found\"}");
    }

    close(client_fd);
  }

  sdk.stop();
  return 0;
}
