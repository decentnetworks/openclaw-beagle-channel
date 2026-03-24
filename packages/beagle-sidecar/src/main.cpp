#include "beagle_sdk.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct Event {
  std::string account_id;
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

struct AgentProfile {
  std::string account_id;
  std::string agent_id;
  std::string name;
  std::string gender;
  std::string phone;
  std::string email;
  std::string description;
  std::string region;
};

struct AccountRuntime {
  std::string account_id;
  std::string agent_id;
  std::string agent_name;
  std::unique_ptr<BeagleSdk> sdk;
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

static std::string trim_copy(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

static std::string lowercase_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static std::string get_env(const char* key) {
  const char* value = std::getenv(key);
  return value ? std::string(value) : std::string();
}

static bool file_exists(const std::string& path) {
  return access(path.c_str(), R_OK) == 0;
}

static std::string expand_home(const std::string& path) {
  if (path.empty() || path[0] != '~') return path;
  const std::string home = get_env("HOME");
  if (home.empty()) return path;
  if (path.size() == 1) return home;
  if (path[1] == '/') return home + path.substr(1);
  return path;
}

static std::string sanitize_account_id(const std::string& raw) {
  std::string in = trim_copy(raw);
  if (in.empty()) return "";
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
      out.push_back(c);
    } else if (std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '\\' || c == ':') {
      out.push_back('_');
    }
  }
  while (!out.empty() && out.front() == '_') out.erase(out.begin());
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

static bool read_file_to_string(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

static size_t skip_json_ws(const std::string& body, size_t pos) {
  while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
  return pos;
}

static bool find_matching_json_bracket(const std::string& body,
                                       size_t start,
                                       char open_char,
                                       char close_char,
                                       size_t& end) {
  if (start >= body.size() || body[start] != open_char) return false;
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (size_t i = start; i < body.size(); ++i) {
    char c = body[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
      continue;
    }
    if (c == open_char) {
      ++depth;
      continue;
    }
    if (c == close_char) {
      --depth;
      if (depth == 0) {
        end = i;
        return true;
      }
    }
  }
  return false;
}

static bool extract_json_container_by_key(const std::string& body,
                                          const std::string& key,
                                          char open_char,
                                          char close_char,
                                          size_t& start,
                                          size_t& end) {
  const std::string needle = "\"" + key + "\"";
  size_t key_pos = body.find(needle);
  if (key_pos == std::string::npos) return false;
  size_t colon = body.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return false;
  size_t pos = skip_json_ws(body, colon + 1);
  if (pos >= body.size() || body[pos] != open_char) return false;
  if (!find_matching_json_bracket(body, pos, open_char, close_char, end)) return false;
  start = pos;
  return true;
}

static std::string extract_first_json_string(const std::string& body,
                                             const std::initializer_list<const char*>& keys) {
  for (const char* key : keys) {
    std::string value;
    if (key && extract_json_string(body, key, value) && !trim_copy(value).empty()) {
      return trim_copy(value);
    }
  }
  return "";
}

static AgentProfile parse_agent_profile_from_object(const std::string& object_body,
                                                    const std::string& key_hint) {
  AgentProfile profile;
  profile.agent_id = extract_first_json_string(object_body, {"id", "agentId", "agent_id", "slug"});
  if (profile.agent_id.empty()) profile.agent_id = key_hint;
  profile.account_id = sanitize_account_id(profile.agent_id.empty() ? key_hint : profile.agent_id);
  profile.name = extract_first_json_string(object_body, {"name", "displayName", "title"});
  profile.gender = extract_first_json_string(object_body, {"gender"});
  profile.phone = extract_first_json_string(object_body, {"phone"});
  profile.email = extract_first_json_string(object_body, {"email"});
  profile.description = extract_first_json_string(object_body, {"description", "bio", "summary"});
  profile.region = extract_first_json_string(object_body, {"region", "location"});
  if (profile.name.empty()) profile.name = profile.agent_id;
  return profile;
}

static size_t skip_json_value(const std::string& body, size_t pos) {
  pos = skip_json_ws(body, pos);
  if (pos >= body.size()) return std::string::npos;
  char c = body[pos];
  if (c == '"') {
    std::string ignored;
    size_t end_pos = pos;
    if (!decode_json_string(body, pos, ignored, end_pos)) return std::string::npos;
    return end_pos + 1;
  }
  if (c == '{') {
    size_t end = pos;
    if (!find_matching_json_bracket(body, pos, '{', '}', end)) return std::string::npos;
    return end + 1;
  }
  if (c == '[') {
    size_t end = pos;
    if (!find_matching_json_bracket(body, pos, '[', ']', end)) return std::string::npos;
    return end + 1;
  }
  while (pos < body.size()) {
    char ch = body[pos];
    if (ch == ',' || ch == '}' || ch == ']') break;
    ++pos;
  }
  return pos;
}

static void parse_agent_object_map(const std::string& object_body, std::vector<AgentProfile>& out) {
  if (object_body.empty() || object_body.front() != '{') return;
  size_t pos = 1;
  while (pos < object_body.size()) {
    pos = skip_json_ws(object_body, pos);
    if (pos >= object_body.size() || object_body[pos] == '}') break;
    if (object_body[pos] != '"') break;

    std::string key;
    size_t key_end = pos;
    if (!decode_json_string(object_body, pos, key, key_end)) break;
    pos = skip_json_ws(object_body, key_end + 1);
    if (pos >= object_body.size() || object_body[pos] != ':') break;
    ++pos;
    pos = skip_json_ws(object_body, pos);
    if (pos >= object_body.size()) break;

    if (object_body[pos] == '{') {
      size_t obj_end = pos;
      if (!find_matching_json_bracket(object_body, pos, '{', '}', obj_end)) break;
      AgentProfile profile = parse_agent_profile_from_object(
          object_body.substr(pos, obj_end - pos + 1),
          sanitize_account_id(key));
      if (!profile.account_id.empty()) out.push_back(std::move(profile));
      pos = obj_end + 1;
    } else {
      size_t next = skip_json_value(object_body, pos);
      if (next == std::string::npos) break;
      pos = next;
    }

    pos = skip_json_ws(object_body, pos);
    if (pos < object_body.size() && object_body[pos] == ',') ++pos;
  }
}

static void parse_agent_array(const std::string& array_body, std::vector<AgentProfile>& out) {
  if (array_body.empty() || array_body.front() != '[') return;
  size_t pos = 1;
  while (pos < array_body.size()) {
    pos = skip_json_ws(array_body, pos);
    if (pos >= array_body.size() || array_body[pos] == ']') break;
    if (array_body[pos] == '{') {
      size_t obj_end = pos;
      if (!find_matching_json_bracket(array_body, pos, '{', '}', obj_end)) break;
      AgentProfile profile = parse_agent_profile_from_object(
          array_body.substr(pos, obj_end - pos + 1), "");
      if (!profile.account_id.empty()) out.push_back(std::move(profile));
      pos = obj_end + 1;
    } else {
      size_t next = skip_json_value(array_body, pos);
      if (next == std::string::npos) break;
      pos = next;
    }
    pos = skip_json_ws(array_body, pos);
    if (pos < array_body.size() && array_body[pos] == ',') ++pos;
  }
}

static std::vector<AgentProfile> load_agent_profiles_from_openclaw_config(const std::string& config_path) {
  std::vector<AgentProfile> profiles;
  std::string body;
  if (config_path.empty() || !read_file_to_string(config_path, body)) return profiles;

  const char* section_keys[] = {"agents", "characters"};
  for (const char* section_key : section_keys) {
    size_t start = 0;
    size_t end = 0;
    if (extract_json_container_by_key(body, section_key, '{', '}', start, end)) {
      std::string section_object = body.substr(start, end - start + 1);
      if (std::string(section_key) == "agents") {
        size_t list_start = 0;
        size_t list_end = 0;
        if (extract_json_container_by_key(section_object, "list", '[', ']', list_start, list_end)) {
          parse_agent_array(section_object.substr(list_start, list_end - list_start + 1), profiles);
        }
      }
      parse_agent_object_map(section_object, profiles);
      continue;
    }
    if (extract_json_container_by_key(body, section_key, '[', ']', start, end)) {
      parse_agent_array(body.substr(start, end - start + 1), profiles);
    }
  }

  std::unordered_map<std::string, AgentProfile> deduped;
  for (const auto& p : profiles) {
    if (p.account_id.empty()) continue;
    auto it = deduped.find(p.account_id);
    if (it == deduped.end()) {
      deduped.emplace(p.account_id, p);
      continue;
    }
    if (it->second.name.empty() && !p.name.empty()) it->second.name = p.name;
    if (it->second.description.empty() && !p.description.empty()) it->second.description = p.description;
    if (it->second.gender.empty() && !p.gender.empty()) it->second.gender = p.gender;
    if (it->second.phone.empty() && !p.phone.empty()) it->second.phone = p.phone;
    if (it->second.email.empty() && !p.email.empty()) it->second.email = p.email;
    if (it->second.region.empty() && !p.region.empty()) it->second.region = p.region;
    if (it->second.agent_id.empty() && !p.agent_id.empty()) it->second.agent_id = p.agent_id;
  }

  std::vector<AgentProfile> out;
  out.reserve(deduped.size());
  for (const auto& kv : deduped) out.push_back(kv.second);
  std::sort(out.begin(), out.end(), [](const AgentProfile& a, const AgentProfile& b) {
    return a.account_id < b.account_id;
  });
  return out;
}

static std::string events_to_json(std::vector<Event> events) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < events.size(); ++i) {
    const auto& ev = events[i];
    if (i) oss << ",";
    oss << "{"
        << "\"accountId\":\"" << json_escape(ev.account_id) << "\""
        << ",\"peer\":\"" << json_escape(ev.peer) << "\"";
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
  const std::string wanted = lowercase_copy(trim_copy(key));
  if (wanted.empty()) return "";
  size_t line_start = 0;
  while (line_start < headers.size()) {
    size_t line_end = headers.find("\r\n", line_start);
    if (line_end == std::string::npos) line_end = headers.size();
    if (line_end == line_start) break;
    size_t colon = headers.find(':', line_start);
    if (colon != std::string::npos && colon < line_end) {
      std::string k = lowercase_copy(trim_copy(headers.substr(line_start, colon - line_start)));
      if (k == wanted) {
        return trim_copy(headers.substr(colon + 1, line_end - colon - 1));
      }
    }
    if (line_end >= headers.size()) break;
    line_start = line_end + 2;
  }
  return "";
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

static void push_event(const std::string& account_id, const BeagleIncomingMessage& msg) {
  Event ev;
  ev.account_id = account_id;
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
  log_line(std::string("[sidecar] queued event account=") + account_id
           + " peer=" + msg.peer
           + " text_len=" + std::to_string(msg.text.size())
           + " ts=" + std::to_string(msg.ts));
}

struct ServerOptions {
  int port = 39091;
  std::string token;
  std::string data_dir = "./data";
  std::string config_path;
  std::string openclaw_config_path;
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
    } else if (arg == "--openclaw-config" && i + 1 < argc) {
      opts.openclaw_config_path = argv[++i];
    }
  }
  return opts;
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

static std::string resolve_openclaw_config_path(const ServerOptions& opts) {
  if (!opts.openclaw_config_path.empty()) return expand_home(opts.openclaw_config_path);
  std::string env_path = get_env("BEAGLE_OPENCLAW_CONFIG");
  if (env_path.empty()) env_path = get_env("OPENCLAW_CONFIG");
  if (!env_path.empty()) return expand_home(env_path);
  std::string home = get_env("HOME");
  if (!home.empty()) {
    std::string default_path = home + "/.openclaw/openclaw.json";
    if (file_exists(default_path)) return default_path;
  }
  return "";
}

static std::string account_data_dir(const std::string& base_data_dir,
                                    const std::string& account_id,
                                    bool multi_account) {
  if (!multi_account) return base_data_dir;
  std::string cleaned = sanitize_account_id(account_id);
  if (cleaned.empty()) cleaned = "default";
  return base_data_dir + "/accounts/" + cleaned;
}

static std::string requested_account_id(const std::string& headers, const std::string& body) {
  std::string account_id = trim_copy(header_value(headers, "X-Beagle-Account"));
  if (account_id.empty()) extract_json_string(body, "accountId", account_id);
  return sanitize_account_id(account_id);
}

int main(int argc, char** argv) {
  ServerOptions opts = parse_args(argc, argv);
  std::string config_path = resolve_config_path(opts);
  if (config_path.empty()) {
    log_line("Missing Carrier config. Provide --config or set BEAGLE_SDK_ROOT.");
    return 1;
  }

  std::string openclaw_config_path = resolve_openclaw_config_path(opts);
  std::vector<AgentProfile> agent_profiles;
  if (!openclaw_config_path.empty() && file_exists(openclaw_config_path)) {
    agent_profiles = load_agent_profiles_from_openclaw_config(openclaw_config_path);
    log_line(std::string("[sidecar] openclaw config=") + openclaw_config_path
             + " discovered_agents=" + std::to_string(agent_profiles.size()));
  }

  if (agent_profiles.empty()) {
    AgentProfile fallback;
    fallback.account_id = "default";
    fallback.agent_id = "default";
    fallback.name = "Snoopy";
    fallback.description = "OpenClaw Beagle agent";
    fallback.region = "California";
    agent_profiles.push_back(std::move(fallback));
  }

  const bool multi_account = agent_profiles.size() > 1;
  std::map<std::string, std::unique_ptr<AccountRuntime>> accounts;
  std::string default_account_id;

  for (const auto& profile : agent_profiles) {
    std::string account_id = sanitize_account_id(profile.account_id);
    if (account_id.empty()) account_id = sanitize_account_id(profile.agent_id);
    if (account_id.empty()) account_id = "default";
    if (accounts.find(account_id) != accounts.end()) continue;

    std::unique_ptr<AccountRuntime> runtime(new AccountRuntime());
    runtime->account_id = account_id;
    runtime->agent_id = profile.agent_id.empty() ? account_id : profile.agent_id;
    runtime->agent_name = profile.name.empty() ? runtime->agent_id : profile.name;
    runtime->sdk.reset(new BeagleSdk());

    BeagleSdkOptions sdk_opts;
    sdk_opts.config_path = config_path;
    sdk_opts.data_dir = account_data_dir(opts.data_dir, account_id, multi_account);
    sdk_opts.account_id = account_id;
    sdk_opts.profile_name = runtime->agent_name;
    sdk_opts.profile_gender = profile.gender;
    sdk_opts.profile_phone = profile.phone;
    sdk_opts.profile_email = profile.email;
    sdk_opts.profile_description = profile.description;
    sdk_opts.profile_region = profile.region;
    sdk_opts.openclaw_agent_id = runtime->agent_id;

    const std::string callback_account = account_id;
    if (!runtime->sdk->start(sdk_opts, [callback_account](const BeagleIncomingMessage& msg) {
          push_event(callback_account, msg);
        })) {
      log_line(std::string("Failed to start Beagle SDK account=") + account_id);
      for (auto& kv : accounts) {
        if (kv.second && kv.second->sdk) kv.second->sdk->stop();
      }
      return 1;
    }

    log_line(std::string("[sidecar] started account=") + account_id
             + " agent=" + runtime->agent_id
             + " user_id=" + runtime->sdk->userid()
             + " address=" + runtime->sdk->address());
    accounts.emplace(account_id, std::move(runtime));
  }

  if (accounts.empty()) {
    log_line("Failed to start any Beagle account");
    return 1;
  }
  if (accounts.find("main") != accounts.end()) {
    default_account_id = "main";
  } else if (accounts.find("default") != accounts.end()) {
    default_account_id = "default";
  } else if (!accounts.empty()) {
    default_account_id = accounts.begin()->first;
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

  auto resolve_account = [&](const std::string& wanted) -> AccountRuntime* {
    if (!wanted.empty()) {
      auto it = accounts.find(wanted);
      if (it != accounts.end()) return it->second.get();
      return nullptr;
    }
    auto it = accounts.find(default_account_id);
    if (it != accounts.end()) return it->second.get();
    if (!accounts.empty()) return accounts.begin()->second.get();
    return nullptr;
  };

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

    std::string wanted_account_id = requested_account_id(headers, body);
    AccountRuntime* account = resolve_account(wanted_account_id);

    if (method == "GET" && path == "/health") {
      if (!wanted_account_id.empty() && !account) {
        send_response(client_fd, 404, "application/json", "{\"ok\":false,\"error\":\"unknown_account\"}");
        close(client_fd);
        continue;
      }
      std::ostringstream oss;
      oss << "{"
          << "\"ok\":true"
          << ",\"requestedAccountId\":\"" << json_escape(account ? account->account_id : "") << "\""
          << ",\"userId\":\"" << json_escape(account ? account->sdk->userid() : "") << "\""
          << ",\"address\":\"" << json_escape(account ? account->sdk->address() : "") << "\""
          << ",\"accounts\":[";
      bool first = true;
      for (const auto& kv : accounts) {
        const AccountRuntime* runtime = kv.second.get();
        if (!runtime || !runtime->sdk) continue;
        if (!first) oss << ",";
        first = false;
        oss << "{"
            << "\"accountId\":\"" << json_escape(runtime->account_id) << "\""
            << ",\"agentId\":\"" << json_escape(runtime->agent_id) << "\""
            << ",\"agentName\":\"" << json_escape(runtime->agent_name) << "\""
            << ",\"userId\":\"" << json_escape(runtime->sdk->userid()) << "\""
            << ",\"address\":\"" << json_escape(runtime->sdk->address()) << "\""
            << "}";
      }
      oss << "]"
          << "}";
      send_response(client_fd, 200, "application/json", oss.str());
    } else if (method == "GET" && path == "/status") {
      if (!account) {
        send_response(client_fd, 404, "application/json", "{\"ok\":false,\"error\":\"unknown_account\"}");
        close(client_fd);
        continue;
      }
      BeagleStatus status = account->sdk->status();
      std::string last_online_human = to_iso8601(status.last_online_ts);
      std::string last_offline_human = to_iso8601(status.last_offline_ts);
      std::ostringstream oss;
      oss << "{"
          << "\"ok\":true"
          << ",\"accountId\":\"" << json_escape(account->account_id) << "\""
          << ",\"agentId\":\"" << json_escape(account->agent_id) << "\""
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
      if (!wanted_account_id.empty() && !account) {
        send_response(client_fd, 404, "application/json", "{\"ok\":false,\"error\":\"unknown_account\"}");
        close(client_fd);
        continue;
      }
      std::vector<Event> events;
      std::vector<Event> remaining;
      const std::string selected_account = account ? account->account_id : "";
      {
        std::lock_guard<std::mutex> lock(g_events_mu);
        remaining.reserve(g_events.size());
        for (auto& ev : g_events) {
          if (selected_account.empty() || ev.account_id == selected_account) {
            events.push_back(std::move(ev));
          } else {
            remaining.push_back(std::move(ev));
          }
        }
        g_events.swap(remaining);
      }
      if (!events.empty()) {
        std::string ua = header_value(headers, "User-Agent");
        std::ostringstream msg;
        msg << "[sidecar] /events account=" << (selected_account.empty() ? "(all)" : selected_account)
            << " -> " << events.size() << " event(s)"
            << " from " << client_ip(client_fd);
        if (!ua.empty()) msg << " ua=" << ua;
        log_line(msg.str());
      }
      send_response(client_fd, 200, "application/json", events_to_json(std::move(events)));
    } else if (method == "POST" && path == "/sendText") {
      if (!account) {
        send_response(client_fd, 404, "application/json", "{\"ok\":false,\"error\":\"unknown_account\"}");
        close(client_fd);
        continue;
      }
      std::string peer;
      std::string text;
      extract_json_string(body, "peer", peer);
      extract_json_string(body, "text", text);
      log_line(std::string("[sidecar] /sendText account=") + account->account_id
               + " peer=" + peer
               + " text_len=" + std::to_string(text.size()));

      bool ok = account->sdk->send_text(peer, text);
      send_response(client_fd, ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (method == "POST" && path == "/sendMedia") {
      if (!account) {
        send_response(client_fd, 404, "application/json", "{\"ok\":false,\"error\":\"unknown_account\"}");
        close(client_fd);
        continue;
      }
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
      log_line(std::string("[sidecar] /sendMedia account=") + account->account_id
               + " peer=" + peer
               + " caption_len=" + std::to_string(caption.size())
               + " media_url_len=" + std::to_string(media_url.size())
               + " media_path_len=" + std::to_string(media_path.size())
               + " out_format=" + (out_format.empty() ? "(default)" : out_format));

      bool ok = account->sdk->send_media(peer, caption, media_path, media_url, media_type, filename, out_format);
      send_response(client_fd, ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (method == "POST" && path == "/sendStatus") {
      if (!account) {
        send_response(client_fd, 404, "application/json", "{\"ok\":false,\"error\":\"unknown_account\"}");
        close(client_fd);
        continue;
      }
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
      log_line(std::string("[sidecar] /sendStatus account=") + account->account_id
               + " peer=" + peer
               + " state=" + state
               + " phase=" + phase
               + " chat_type=" + (chat_type.empty() ? "(auto)" : chat_type)
               + " ttl_ms=" + std::to_string(ttl_ms));
      bool ok = account->sdk->send_status(peer,
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

  for (auto& kv : accounts) {
    if (kv.second && kv.second->sdk) kv.second->sdk->stop();
  }
  return 0;
}
