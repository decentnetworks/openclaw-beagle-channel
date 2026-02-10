#include "beagle_sdk.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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
  std::string msg_id;
  long long ts = 0;
};

static std::mutex g_events_mu;
static std::vector<Event> g_events;

static bool extract_json_string(const std::string& body, const std::string& key, std::string& out) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return false;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos = body.find('"', pos);
  if (pos == std::string::npos) return false;
  size_t end = body.find('"', pos + 1);
  if (end == std::string::npos) return false;
  out = body.substr(pos + 1, end - pos - 1);
  return true;
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
      default: out += c; break;
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
  std::string needle = "Content-Length:";
  size_t pos = headers.find(needle);
  if (pos == std::string::npos) return 0;
  pos += needle.size();
  while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) pos++;
  size_t end = headers.find("\r\n", pos);
  if (end == std::string::npos) end = headers.size();
  return std::atoi(headers.substr(pos, end - pos).c_str());
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

static void push_event(const BeagleIncomingMessage& msg) {
  Event ev;
  ev.peer = msg.peer;
  ev.text = msg.text;
  ev.media_path = msg.media_path;
  ev.media_url = msg.media_url;
  ev.media_type = msg.media_type;
  ev.filename = msg.filename;
  ev.msg_id = msg.msg_id;
  ev.ts = msg.ts;

  std::lock_guard<std::mutex> lock(g_events_mu);
  g_events.push_back(std::move(ev));
}

struct ServerOptions {
  int port = 39091;
  std::string token;
  std::string data_dir = "./data";
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
    }
  }
  return opts;
}

int main(int argc, char** argv) {
  ServerOptions opts = parse_args(argc, argv);

  BeagleSdk sdk;
  if (!sdk.start({opts.data_dir}, push_event)) {
    std::cerr << "Failed to start Beagle SDK\n";
    return 1;
  }

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Failed to create socket\n";
    return 1;
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(opts.port));

  if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "Bind failed\n";
    return 1;
  }

  if (listen(server_fd, 16) < 0) {
    std::cerr << "Listen failed\n";
    return 1;
  }

  std::cerr << "Beagle sidecar listening on 0.0.0.0:" << opts.port << "\n";

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
        send_response(client_fd, 401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
        close(client_fd);
        continue;
      }
    }

    if (method == "GET" && path == "/health") {
      send_response(client_fd, 200, "application/json", "{\"ok\":true}");
    } else if (method == "GET" && path == "/events") {
      std::vector<Event> events;
      {
        std::lock_guard<std::mutex> lock(g_events_mu);
        events.swap(g_events);
      }
      send_response(client_fd, 200, "application/json", events_to_json(std::move(events)));
    } else if (method == "POST" && path == "/sendText") {
      std::string peer;
      std::string text;
      extract_json_string(body, "peer", peer);
      extract_json_string(body, "text", text);

      bool ok = sdk.send_text(peer, text);
      send_response(client_fd, ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (method == "POST" && path == "/sendMedia") {
      std::string peer;
      std::string caption;
      std::string media_path;
      std::string media_url;
      std::string media_type;
      std::string filename;
      extract_json_string(body, "peer", peer);
      extract_json_string(body, "caption", caption);
      extract_json_string(body, "mediaPath", media_path);
      extract_json_string(body, "mediaUrl", media_url);
      extract_json_string(body, "mediaType", media_type);
      extract_json_string(body, "filename", filename);

      bool ok = sdk.send_media(peer, caption, media_path, media_url, media_type, filename);
      send_response(client_fd, ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else {
      send_response(client_fd, 404, "application/json", "{\"ok\":false,\"error\":\"not_found\"}");
    }

    close(client_fd);
  }

  sdk.stop();
  return 0;
}
