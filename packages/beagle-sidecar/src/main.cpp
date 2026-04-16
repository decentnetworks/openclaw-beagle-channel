#include "beagle_sdk.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
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
  std::string public_display_name;
  std::string public_headline;
  std::string public_avatar;
  std::string public_homepage;
  std::vector<std::pair<std::string, std::string>> public_links;
};

struct AccountRuntime {
  std::string account_id;
  std::string agent_id;
  std::string default_agent_name;
  std::string agent_name;
  std::string public_profile_json;
  std::string public_display_name;
  std::string public_headline;
  std::string public_avatar;
  std::string public_homepage;
  std::vector<std::pair<std::string, std::string>> public_links;
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
/** Parallel copy of inbound events for GET /directory-events so the OpenClaw directory web
 *  poller does not lose profile JSON when beagle-channel consumes GET /events first. */
static std::vector<Event> g_directory_events;

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

static bool dir_exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
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

static bool is_reserved_agent_key(const std::string& key) {
  static const char* reserved[] = {
    "defaults", "default", "list", "models", "meta", "wizard",
    "plugins", "gateway", "auth", "channels", "session", nullptr
  };
  std::string lower = key;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const char** p = reserved; *p; ++p) {
    if (lower == *p) return true;
  }
  return false;
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
      if (!is_reserved_agent_key(key)) {
        AgentProfile profile = parse_agent_profile_from_object(
            object_body.substr(pos, obj_end - pos + 1),
            sanitize_account_id(key));
        if (!profile.account_id.empty()) out.push_back(std::move(profile));
      }
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

static bool profile_list_has_account_id(const std::vector<AgentProfile>& profiles,
                                       const std::string& id) {
  for (const auto& p : profiles) {
    if (sanitize_account_id(p.account_id) == id) return true;
    if (sanitize_account_id(p.agent_id) == id) return true;
  }
  return false;
}

static void merge_profiles_from_agents_dir(std::vector<AgentProfile>& profiles,
                                          const std::string& home) {
  if (home.empty()) return;
  std::string base = home + "/.openclaw/agents";
  DIR* d = opendir(base.c_str());
  if (!d) return;
  while (struct dirent* ent = readdir(d)) {
    std::string name = ent->d_name;
    if (name == "." || name == ".." || name.empty()) continue;
    if (name[0] == '.') continue;
    std::string sanitized = sanitize_account_id(name);
    if (sanitized.empty()) continue;
    if (profile_list_has_account_id(profiles, sanitized)) continue;
    std::string full = base + "/" + name;
    struct stat st;
    if (stat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

    AgentProfile ap;
    ap.agent_id = sanitized;
    ap.account_id = sanitized;
    ap.name = sanitized;
    std::string meta_path = full + "/agent/openclaw.json";
    if (!file_exists(meta_path)) meta_path = full + "/openclaw.json";
    std::string meta_body;
    if (read_file_to_string(meta_path, meta_body)) {
      std::string nm;
      if (extract_json_string(meta_body, "name", nm) && !trim_copy(nm).empty()) {
        ap.name = trim_copy(nm);
      } else if (extract_json_string(meta_body, "displayName", nm) && !trim_copy(nm).empty()) {
        ap.name = trim_copy(nm);
      }
    }
    profiles.push_back(std::move(ap));
  }
  closedir(d);
  std::sort(profiles.begin(), profiles.end(), [](const AgentProfile& a, const AgentProfile& b) {
    return a.account_id < b.account_id;
  });
}

static std::string exec_first_line(const char* cmd) {
  FILE* p = popen(cmd, "r");
  if (!p) return "";
  char buf[512];
  std::string out;
  if (fgets(buf, sizeof(buf), p)) out = trim_copy(std::string(buf));
  pclose(p);
  return out;
}

static std::string parse_last_touched_version_from_openclaw_json(const std::string& body) {
  const char* key = "\"lastTouchedVersion\"";
  size_t pos = body.find(key);
  if (pos == std::string::npos) return "";
  pos = body.find(':', pos);
  if (pos == std::string::npos) return "";
  pos = body.find('"', pos);
  if (pos == std::string::npos) return "";
  ++pos;
  size_t end = body.find('"', pos);
  if (end == std::string::npos) return "";
  return trim_copy(body.substr(pos, end - pos));
}

static std::string resolve_openclaw_version_from_path(const std::string& config_path) {
  std::string v = trim_copy(get_env("OPENCLAW_VERSION"));
  if (!v.empty()) return v;
  v = exec_first_line("openclaw --version 2>/dev/null");
  if (!v.empty()) return v;
  if (!config_path.empty()) {
    std::string body;
    if (read_file_to_string(config_path, body)) {
      v = parse_last_touched_version_from_openclaw_json(body);
      if (!v.empty()) return v;
    }
  }
  return "unknown";
}

static std::string resolve_beagle_channel_version_from_path() {
  std::string v = trim_copy(get_env("BEAGLE_CHANNEL_VERSION"));
  if (!v.empty()) return v;
  std::string home = get_env("HOME");
  if (home.empty()) return "unknown";
  std::string path = home + "/.openclaw/extensions/beagle/package.json";
  std::string body;
  if (!read_file_to_string(path, body)) return "unknown";
  if (extract_json_string(body, "version", v) && !trim_copy(v).empty()) return trim_copy(v);
  return "unknown";
}

/** One HTTPS GET; returns trimmed first line (IPv4/IPv6 text). Empty on failure. */
static std::string curl_fetch_text_line(const std::string& url) {
  FILE* p = popen(("curl -fsS --max-time 8 " + url + " 2>/dev/null").c_str(), "r");
  if (!p) return "";
  char buf[128];
  std::string out;
  if (fgets(buf, sizeof(buf), p)) out = trim_copy(std::string(buf));
  pclose(p);
  return out;
}

static std::string fetch_external_ip() {
  std::string pre = trim_copy(get_env("BEAGLE_EXTERNAL_IP"));
  if (!pre.empty()) return pre;
  static const char* urls[] = {
      "https://api.ipify.org",
      "https://icanhazip.com",
      "https://ifconfig.me/ip",
  };
  for (const char* url : urls) {
    std::string out = curl_fetch_text_line(url);
    if (!out.empty()) return out;
  }
  return "";
}

static std::string get_external_ip_cached() {
  static std::string cache;
  static std::chrono::steady_clock::time_point last{};
  static bool has_last = false;
  auto now = std::chrono::steady_clock::now();
  if (has_last && now - last < std::chrono::seconds(3600)) {
    return cache;
  }
  cache = fetch_external_ip();
  last = now;
  has_last = true;
  if (cache.empty()) {
    log_line("[sidecar] hostIpExternal empty: set BEAGLE_EXTERNAL_IP or allow outbound HTTPS "
             "(tried api.ipify.org, icanhazip.com, ifconfig.me/ip).");
  }
  return cache;
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
  g_events.push_back(ev);
  g_directory_events.push_back(std::move(ev));
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
  std::string directory_address;
  std::string directory_hello = "openclaw-beagle-channel";
  bool emit_presence = false;
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
    } else if (arg == "--directory-address" && i + 1 < argc) {
      opts.directory_address = argv[++i];
    } else if (arg == "--directory-hello" && i + 1 < argc) {
      opts.directory_hello = argv[++i];
    } else if (arg == "--emit-presence") {
      opts.emit_presence = true;
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

static std::string resolve_directory_address(const ServerOptions& opts) {
  if (!trim_copy(opts.directory_address).empty()) return trim_copy(opts.directory_address);
  std::string env_addr = get_env("BEAGLE_DIRECTORY_ADDRESS");
  if (env_addr.empty()) env_addr = get_env("OPENCLAW_DIRECTORY_ADDRESS");
  return trim_copy(env_addr);
}

static std::string resolve_directory_hello(const ServerOptions& opts) {
  if (!trim_copy(opts.directory_hello).empty()) return trim_copy(opts.directory_hello);
  std::string env_hello = get_env("BEAGLE_DIRECTORY_HELLO");
  if (env_hello.empty()) env_hello = get_env("OPENCLAW_DIRECTORY_HELLO");
  env_hello = trim_copy(env_hello);
  if (env_hello.empty()) env_hello = "openclaw-beagle-channel";
  return env_hello;
}

static std::string account_data_dir(const std::string& base_data_dir,
                                    const std::string& account_id,
                                    bool multi_account) {
  if (!multi_account) return base_data_dir;
  std::string cleaned = sanitize_account_id(account_id);
  if (cleaned.empty()) cleaned = "default";
  std::string nested = base_data_dir + "/accounts/" + cleaned;

  if ((cleaned == "main" || cleaned == "default") && dir_exists(base_data_dir) && !dir_exists(nested)) {
    DIR* dir = opendir(base_data_dir.c_str());
    if (dir) {
      bool has_legacy_entries = false;
      for (dirent* ent = readdir(dir); ent; ent = readdir(dir)) {
        std::string name = ent->d_name;
        if (name.empty() || name == "." || name == ".." || name == "accounts") continue;
        has_legacy_entries = true;
        break;
      }
      closedir(dir);
      if (has_legacy_entries) {
        log_line(std::string("[sidecar] legacy Carrier root detected; reusing ")
                 + base_data_dir + " for account=" + cleaned);
        return base_data_dir;
      }
    }
  }

  return nested;
}

static std::string requested_account_id(const std::string& headers, const std::string& body) {
  std::string account_id = trim_copy(header_value(headers, "X-Beagle-Account"));
  if (account_id.empty()) extract_json_string(body, "accountId", account_id);
  return sanitize_account_id(account_id);
}

static std::string get_hostname() {
  char buf[256] = {};
  if (gethostname(buf, sizeof(buf) - 1) == 0) return std::string(buf);
  return "";
}

static std::string get_local_ip_address() {
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) return "";
  std::string result;
  for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    char host[NI_MAXHOST];
    if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                    host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) != 0) continue;
    std::string addr(host);
    if (addr != "127.0.0.1") { result = addr; break; }
  }
  freeifaddrs(ifaddr);
  return result;
}

static std::string build_directory_profile_payload(const AccountRuntime* runtime,
                                                   const std::string& openclaw_version,
                                                   const std::string& beagle_channel_version) {
  if (!runtime || !runtime->sdk) return "";
  std::string host_name = get_hostname();
  std::string host_ip = get_local_ip_address();
  std::string host_ip_ext = get_external_ip_cached();

  std::string public_profile = trim_copy(runtime->public_profile_json);
  if (public_profile.empty()) {
    bool has_public_profile = false;
    auto append_public_field = [&](const std::string& key, const std::string& value) {
      if (value.empty()) return;
      public_profile += has_public_profile ? "," : "";
      public_profile += "\"" + json_escape(key) + "\":\"" + json_escape(value) + "\"";
      has_public_profile = true;
    };

    append_public_field("displayName", runtime->public_display_name);
    append_public_field("headline", runtime->public_headline);
    append_public_field("avatarUrl", runtime->public_avatar);
    append_public_field("homepageUrl", runtime->public_homepage);

    if (!runtime->public_links.empty()) {
      std::string links_json;
      bool has_links = false;
      for (const auto& kv : runtime->public_links) {
        if (kv.first.empty() || kv.second.empty()) continue;
        links_json += has_links ? "," : "";
        links_json += "\"" + json_escape(kv.first) + "\":\"" + json_escape(kv.second) + "\"";
        has_links = true;
      }
      if (has_links) {
        public_profile += has_public_profile ? "," : "";
        public_profile += "\"socials\":{" + links_json + "}";
      }
    }
  }

  std::string payload = std::string("{\"profile\":{\"address\":\"")
      + json_escape(runtime->sdk->address())
      + "\",\"agentName\":\"" + json_escape(runtime->agent_name)
      + "\",\"openclawVersion\":\"" + json_escape(openclaw_version)
      + "\",\"beagleChannelVersion\":\"" + json_escape(beagle_channel_version)
      + "\",\"hostName\":\"" + json_escape(host_name)
      + "\",\"hostIp\":\"" + json_escape(host_ip)
      + "\",\"hostIpExternal\":\"" + json_escape(host_ip_ext) + "\"";
  if (!public_profile.empty()) {
    payload += ",\"publicProfile\":" + public_profile;
  }
  payload += "}}";
  return payload;
}

int main(int argc, char** argv) {
  ServerOptions opts = parse_args(argc, argv);
  std::string config_path = resolve_config_path(opts);
  if (config_path.empty()) {
    log_line("Missing Carrier config. Provide --config or set BEAGLE_SDK_ROOT.");
    return 1;
  }

  std::string openclaw_config_path = resolve_openclaw_config_path(opts);
  std::string directory_address = resolve_directory_address(opts);
  std::string directory_hello = resolve_directory_hello(opts);
  std::string openclaw_version = resolve_openclaw_version_from_path(openclaw_config_path);
  std::string beagle_channel_version = resolve_beagle_channel_version_from_path();
  std::vector<AgentProfile> agent_profiles;
  if (!openclaw_config_path.empty() && file_exists(openclaw_config_path)) {
    agent_profiles = load_agent_profiles_from_openclaw_config(openclaw_config_path);
    log_line(std::string("[sidecar] openclaw config=") + openclaw_config_path
             + " discovered_agents=" + std::to_string(agent_profiles.size()));
  }
  std::string home = get_env("HOME");
  if (agent_profiles.empty() && !home.empty()) {
    size_t n_before = agent_profiles.size();
    merge_profiles_from_agents_dir(agent_profiles, home);
    if (agent_profiles.size() > n_before) {
      log_line(std::string("[sidecar] discovered agent dirs from ~/.openclaw/agents/ total=") +
               std::to_string(agent_profiles.size()));
    }
  }

  if (agent_profiles.empty()) {
    AgentProfile fallback;
    // OpenClaw's default agent slug is "main" (see `openclaw agents list`), not "default".
    fallback.account_id = "main";
    fallback.agent_id = "main";
    std::string dn = trim_copy(get_env("BEAGLE_DEFAULT_AGENT_NAME"));
    fallback.name = dn.empty() ? std::string("main") : dn;
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
    runtime->default_agent_name = profile.name.empty() ? runtime->agent_id : profile.name;
    runtime->agent_name = runtime->default_agent_name;
    runtime->public_display_name = profile.public_display_name;
    runtime->public_headline = profile.public_headline;
    runtime->public_avatar = profile.public_avatar;
    runtime->public_homepage = profile.public_homepage;
    runtime->public_links = profile.public_links;
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
    sdk_opts.emit_presence = opts.emit_presence || !get_env("BEAGLE_EMIT_PRESENCE").empty();

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

  // ── Directory auto-bootstrap ─────────────────────────────────────
  //
  // Use the OpenClaw directory as the built-in default so new installs
  // work without extra BEAGLE_DIRECTORY_ADDRESS setup, while keeping the
  // previous directory as a backup bootstrap target.
  const std::string DEFAULT_DIR = "ZJUCSC38KFw7DSwpfLp1HCem3dJEA5NG2ZvahbEjAUFZ4WUb1jV2";
  const std::string DEFAULT_DIR_1 = "bKaapxLjDGwuCZ7oohVFMVbcHWFYy7yNVGXkVSVL8AkAxbokCGi2";

  std::vector<std::string> dir_addresses;
  if (!trim_copy(directory_address).empty()) {
    dir_addresses.push_back(trim_copy(directory_address));
  } else {
    dir_addresses.push_back(DEFAULT_DIR);
    dir_addresses.push_back(DEFAULT_DIR_1);
  }

  for (auto& kv : accounts) {
    const std::string account_id = kv.first;
    AccountRuntime* started = kv.second.get();
    if (!started || !started->sdk) continue;

    for (const std::string& dir_addr : dir_addresses) {
      if (started->sdk->address() == dir_addr) continue;

      std::thread([started,
                   account_id,
                   dir_addr,
                   directory_hello,
                   openclaw_version,
                   beagle_channel_version]() {
        bool already_friend = started->sdk->has_friend(dir_addr);

        if (!already_friend) {
          // Wait for SDK to be ready
          for (int attempt = 1; attempt <= 12; ++attempt) {
            if (!started || !started->sdk) return;
            BeagleStatus st = started->sdk->status();
            if (!st.ready) {
              std::this_thread::sleep_for(std::chrono::seconds(2));
              continue;
            }

            bool added = started->sdk->add_friend(dir_addr, directory_hello);
            log_line(std::string("[sidecar] directory friend add account=") + account_id
                     + " address=" + dir_addr
                     + " attempt=" + std::to_string(attempt)
                     + " result=" + (added ? "ok" : "failed"));

            if (added) {
              already_friend = true;
              break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
          }
        } else {
          log_line(std::string("[sidecar] directory friend already exists, skipping add account=")
                   + account_id + " address=" + dir_addr);
        }

        if (!already_friend) {
          log_line(std::string("[sidecar] directory friend add failed after retries account=")
                   + account_id + " address=" + dir_addr);
          return;
        }

        // Directory is a friend — wait for it to come online and push profile
        std::string dir_userid = started->sdk->id_from_address(dir_addr);
        if (dir_userid.empty()) {
          log_line(std::string("[sidecar] directory friend but cannot derive userid account=")
                   + account_id + " address=" + dir_addr);
          return;
        }

        std::string profile_payload = build_directory_profile_payload(
            started, openclaw_version, beagle_channel_version);

        for (int wait_attempt = 1; wait_attempt <= 120; ++wait_attempt) {
          if (!started || !started->sdk) return;
          if (!started->sdk->friend_is_online(dir_userid)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
          }

          bool profile_ok = false;
          for (int push_attempt = 1; push_attempt <= 6; ++push_attempt) {
            profile_ok = started->sdk->send_text(dir_userid, profile_payload);
            log_line(std::string("[sidecar] directory auto profile push account=") + account_id
                     + " address=" + dir_addr
                     + " peer=" + dir_userid
                     + " wait_attempt=" + std::to_string(wait_attempt)
                     + " push_attempt=" + std::to_string(push_attempt)
                     + " result=" + (profile_ok ? "ok" : "failed"));
            if (profile_ok) return;
            std::this_thread::sleep_for(std::chrono::seconds(2));
          }

          std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        log_line(std::string("[sidecar] directory auto profile push timeout account=")
                 + account_id + " address=" + dir_addr + " peer=" + dir_userid);
      }).detach();
    }
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
    log_line(std::string("Bind failed on 0.0.0.0:") + std::to_string(opts.port) + " — "
             + std::strerror(errno)
             + " (another process may already use this port; stop the other beagle-sidecar or pick --port)");
    close(server_fd);
    return 1;
  }

  if (listen(server_fd, 16) < 0) {
    log_line(std::string("Listen failed on port ") + std::to_string(opts.port) + ": " + std::strerror(errno));
    close(server_fd);
    return 1;
  }

  log_line(std::string("Beagle sidecar listening on 0.0.0.0:") + std::to_string(opts.port));

  auto resolve_account = [&](const std::string& wanted) -> AccountRuntime* {
    if (!wanted.empty()) {
      auto it = accounts.find(wanted);
      if (it != accounts.end()) return it->second.get();
      // OpenClaw doctor often nests Beagle settings under channels.beagle.accounts.default while
      // Carrier runtimes are keyed from agents.* (e.g. main, dirs). Map "default" to the same
      // runtime we use when no X-Beagle-Account header is sent.
      if (wanted == "default" && !default_account_id.empty()) {
        auto fb = accounts.find(default_account_id);
        if (fb != accounts.end()) return fb->second.get();
      }
      // Single-account sidecars are often queried through a deployment-specific
      // account name (for example "dirs" or "default"). Treat that as the only
      // runtime instead of reporting unknown_account forever.
      if (accounts.size() == 1) return accounts.begin()->second.get();
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
    } else if (method == "GET" && path == "/directory-events") {
      // Same shape as /events but drains g_directory_events (mirrored at enqueue). Use this for
      // the OpenClaw directory SQLite poller so it never races beagle-channel on /events.
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
        remaining.reserve(g_directory_events.size());
        for (auto& ev : g_directory_events) {
          if (selected_account.empty() || ev.account_id == selected_account) {
            events.push_back(std::move(ev));
          } else {
            remaining.push_back(std::move(ev));
          }
        }
        g_directory_events.swap(remaining);
      }
      if (!events.empty()) {
        std::string ua = header_value(headers, "User-Agent");
        std::ostringstream msg;
        msg << "[sidecar] /directory-events account=" << (selected_account.empty() ? "(all)" : selected_account)
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
    } else if (method == "POST" && path == "/setPublicProfile") {
      if (!account) {
        send_response(client_fd, 404, "application/json", "{\"ok\":false,\"error\":\"unknown_account\"}");
        close(client_fd);
        continue;
      }

      std::string agent_name;
      bool has_agent_name = extract_json_string(body, "agentName", agent_name);
      agent_name = trim_copy(agent_name);
      size_t public_profile_start = 0;
      size_t public_profile_end = 0;
      bool has_public_profile = extract_json_container_by_key(
          body, "publicProfile", '{', '}', public_profile_start, public_profile_end);
      std::string public_profile_json = has_public_profile
          ? trim_copy(body.substr(public_profile_start, public_profile_end - public_profile_start + 1))
          : std::string();

      if (!has_agent_name && !has_public_profile) {
        send_response(client_fd, 400, "application/json", "{\"ok\":false,\"error\":\"missing_profile\"}");
        close(client_fd);
        continue;
      }

      if (has_agent_name) {
        account->agent_name = agent_name.empty() ? account->default_agent_name : agent_name;
      }
      if (has_public_profile) {
        account->public_profile_json = public_profile_json;
      }

      std::string profile_payload = build_directory_profile_payload(
          account, openclaw_version, beagle_channel_version);
      int pushed = 0;
      for (const std::string& dir_addr : dir_addresses) {
        if (!account->sdk || account->sdk->address() == dir_addr) continue;
        std::string peer_userid = account->sdk->id_from_address(dir_addr);
        if (peer_userid.empty()) continue;
        if (!account->sdk->friend_is_online(peer_userid)) continue;
        bool ok = account->sdk->send_text(peer_userid, profile_payload);
        log_line(std::string("[sidecar] /setPublicProfile push account=") + account->account_id
                 + " address=" + dir_addr
                 + " peer=" + peer_userid
                 + " result=" + (ok ? "ok" : "failed"));
        if (ok) pushed += 1;
      }

      std::ostringstream oss;
      oss << "{"
          << "\"ok\":true"
          << ",\"accountId\":\"" << json_escape(account->account_id) << "\""
          << ",\"agentName\":\"" << json_escape(account->agent_name) << "\""
          << ",\"pushed\":" << pushed
          << "}";
      send_response(client_fd, 200, "application/json", oss.str());
    } else if (method == "POST" && path == "/addFriend") {
      if (!account) {
        send_response(client_fd, 404, "application/json", "{\"ok\":false,\"error\":\"unknown_account\"}");
        close(client_fd);
        continue;
      }
      std::string address;
      std::string hello;
      extract_json_string(body, "address", address);
      if (trim_copy(address).empty()) extract_json_string(body, "peer", address);
      extract_json_string(body, "hello", hello);
      if (trim_copy(hello).empty()) hello = "openclaw-beagle-channel";

      if (trim_copy(address).empty()) {
        send_response(client_fd, 400, "application/json", "{\"ok\":false,\"error\":\"missing_address\"}");
        close(client_fd);
        continue;
      }

      log_line(std::string("[sidecar] /addFriend account=") + account->account_id
               + " address=" + address);
      bool ok = account->sdk->add_friend(address, hello);
      if (ok) {
        std::string peer_userid = account->sdk->id_from_address(address);
        if (peer_userid.empty()) {
          log_line(std::string("[sidecar] /addFriend cannot derive userid from address=") + address);
          send_response(client_fd, 500, "application/json",
                        "{\"ok\":false,\"error\":\"cannot_derive_userid\"}");
          close(client_fd);
          continue;
        }
        std::string profile_payload = build_directory_profile_payload(
            account, openclaw_version, beagle_channel_version);
        bool profile_ok = false;
        for (int push_attempt = 1; push_attempt <= 6; ++push_attempt) {
          profile_ok = account->sdk->send_text(peer_userid, profile_payload);
          log_line(std::string("[sidecar] /addFriend profile push account=") + account->account_id
                   + " address=" + address
                   + " peer=" + peer_userid
                   + " attempt=" + std::to_string(push_attempt)
                   + " result=" + (profile_ok ? "ok" : "failed"));
          if (profile_ok) break;
          std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        send_response(client_fd, 200, "application/json", "{\"ok\":true}");
      } else {
        send_response(client_fd, 500, "application/json", "{\"ok\":false,\"error\":\"add_friend_failed\"}");
      }
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
