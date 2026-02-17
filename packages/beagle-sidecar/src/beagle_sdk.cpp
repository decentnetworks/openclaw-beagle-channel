#include "beagle_sdk.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

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
                           const std::string& filename,
                           const std::string& out_format) {
  std::cerr << "[beagle-sdk] send_media stub. peer=" << peer
            << " caption=" << caption
            << " media_path=" << media_path
            << " media_url=" << media_url
            << " media_type=" << media_type
            << " filename=" << filename << "\n";
  return true;
}

BeagleStatus BeagleSdk::status() const {
  BeagleStatus s;
  s.ready = true;
  s.connected = true;
  return s;
}

#else

extern "C" {
#include <carrier.h>
#include <carrier_config.h>
#include <carrier_session.h>
#include <carrier_filetransfer.h>
}

namespace {
constexpr size_t kMaxBeaglechatFileBytes = 5 * 1024 * 1024;

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

struct FriendState {
  std::string friendid;
  std::string name;
  std::string gender;
  std::string phone;
  std::string email;
  std::string description;
  std::string region;
  std::string label;
  int status = 0;
  int presence = 0;
};

struct DbConfig {
  bool enabled = false;
  std::string host = "localhost";
  int port = 3306;
  std::string user = "beagle";
  std::string password = "A1anSn00py";
  std::string database = "beagle";
  bool use_crawler_index = false;
  std::string crawler_data_dir = "~/.elacrawler";
  int crawler_refresh_seconds = 60;
  int crawler_lookback_files = 20;
};

struct ProfileInfo {
  std::string name;
  std::string gender;
  std::string phone;
  std::string email;
  std::string description;
  std::string region;
};

struct RuntimeState {
  Carrier* carrier = nullptr;
  BeagleIncomingCallback on_incoming;
  std::thread loop_thread;
  std::mutex state_mu;
  std::string persistent_location;
  std::string welcome_message;
  std::string profile_path;
  std::string welcome_state_path;
  std::string db_config_path;
  std::string friend_state_path;
  std::string friend_event_log_path;
  std::string incoming_event_log_path;
  std::string media_dir;
  std::string user_id;
  std::string address;
  int64_t startup_ts_us = 0;
  BeagleStatus status;
  std::unordered_set<std::string> welcomed_peers;
  std::unordered_map<std::string, bool> peer_prefers_inline_media;
  std::unordered_map<std::string, std::string> peer_media_payload_hint;
  std::unordered_set<std::string> seen_incoming_signatures;
  std::deque<std::string> seen_incoming_order;
  std::map<std::string, FriendState> friend_state;
  DbConfig db;
  std::mutex crawler_mu;
  std::map<std::string, std::pair<std::string, std::string>> crawler_index;
  std::string crawler_index_signature;
  std::time_t crawler_index_last_refresh = 0;
};

struct TransferContext {
  RuntimeState* state = nullptr;
  CarrierFileTransfer* ft = nullptr;
  bool is_sender = false;
  bool connected = false;
  bool completed = false;
  std::string peer;
  std::string fileid;
  std::string filename;
  std::string media_type;
  std::string caption;
  std::string source_path;
  std::string target_path;
  uint64_t expected_size = 0;
  uint64_t transferred = 0;
  std::ifstream source;
  std::ofstream target;
  std::mutex connect_mu;
  std::condition_variable connect_cv;
  bool connect_done = false;
  bool connect_ok = false;
  std::mutex transfer_mu;
  std::condition_variable transfer_cv;
  bool transfer_done = false;
  bool transfer_ok = false;
  std::string transfer_detail;
};

static std::mutex g_ft_mu;
static std::map<CarrierFileTransfer*, std::shared_ptr<TransferContext>> g_transfers;

static void mark_sender_transfer_result(const std::shared_ptr<TransferContext>& ctx,
                                        bool ok,
                                        const std::string& detail) {
  if (!ctx || !ctx->is_sender) return;
  std::lock_guard<std::mutex> lock(ctx->transfer_mu);
  if (ctx->transfer_done) return;
  ctx->transfer_done = true;
  ctx->transfer_ok = ok;
  ctx->transfer_detail = detail;
  ctx->transfer_cv.notify_all();
}
static void persist_crawler_index_to_db(
    RuntimeState* state,
    const std::map<std::string, std::pair<std::string, std::string>>& rows,
    const std::string& source_file,
    std::time_t seen_at);
static std::pair<std::string, std::string> lookup_ip_location_from_crawler_cache_db(
    RuntimeState* state,
    const std::string& friendid);

static bool file_exists(const std::string& path) {
  std::ifstream f(path);
  return f.good();
}

static void ensure_dir(const std::string& path) {
  if (path.empty()) return;
  struct stat st;
  if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return;
  mkdir(path.c_str(), 0755);
}

static std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string token;
  while (iss >> token) out.push_back(token);
  return out;
}

static std::string trim_copy(const std::string& s);

static bool csv_has_token(const std::string& csv, const std::string& token) {
  if (csv.empty() || token.empty()) return false;
  std::string t = trim_copy(token);
  if (t.empty()) return false;
  size_t start = 0;
  while (start < csv.size()) {
    size_t comma = csv.find(',', start);
    if (comma == std::string::npos) comma = csv.size();
    std::string part = trim_copy(csv.substr(start, comma - start));
    if (!part.empty() && part == t) return true;
    start = comma + 1;
  }
  return false;
}

static std::string trim_copy(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  return s.substr(b, e - b);
}

static std::string expand_home(const std::string& path) {
  if (path.empty()) return path;
  if (path[0] != '~') return path;
  const char* home = std::getenv("HOME");
  if (!home) return path;
  if (path.size() == 1) return std::string(home);
  if (path[1] == '/') return std::string(home) + path.substr(1);
  return path;
}

static bool is_valid_ip_token(const std::string& ip) {
  if (ip.empty()) return false;
  for (char c : ip) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == ':' || c == '-') continue;
    return false;
  }
  return true;
}

static std::string endpoint_to_ip(const std::string& endpoint) {
  if (endpoint.empty() || endpoint == "*") return "";
  if (endpoint.front() == '[') {
    size_t end = endpoint.find(']');
    if (end == std::string::npos || end <= 1) return "";
    if (end + 2 > endpoint.size() || endpoint[end + 1] != ':') return "";
    std::string port = endpoint.substr(end + 2);
    if (port.empty()) return "";
    if (port != "*") {
      for (char c : port) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return "";
      }
    }
    return endpoint.substr(1, end - 1);
  }
  size_t colon = endpoint.rfind(':');
  if (colon == std::string::npos || colon == 0) return "";
  std::string port = endpoint.substr(colon + 1);
  if (port.empty()) return "";
  if (port != "*") {
    for (char c : port) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return "";
    }
  }
  return endpoint.substr(0, colon);
}

static std::string endpoint_to_port(const std::string& endpoint) {
  if (endpoint.empty() || endpoint == "*") return "";
  if (endpoint.front() == '[') {
    size_t end = endpoint.find(']');
    if (end == std::string::npos || end + 2 > endpoint.size() || endpoint[end + 1] != ':') return "";
    return endpoint.substr(end + 2);
  }
  size_t colon = endpoint.rfind(':');
  if (colon == std::string::npos || colon + 1 >= endpoint.size()) return "";
  return endpoint.substr(colon + 1);
}

static bool is_ipv4_private(const std::string& ip) {
  int a = -1, b = -1, c = -1, d = -1;
  if (std::sscanf(ip.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
  if (a == 10) return true;
  if (a == 127) return true;
  if (a == 192 && b == 168) return true;
  if (a == 172 && b >= 16 && b <= 31) return true;
  if (a == 169 && b == 254) return true;
  if (a == 100 && b >= 64 && b <= 127) return true;
  return false;
}

static std::string guess_location_from_ip(const std::string& ip) {
  if (ip.empty()) return "";
  if (ip == "::1" || ip == "127.0.0.1") return "loopback";
  if (is_ipv4_private(ip)) return "private-network";
  if (ip.rfind("fc", 0) == 0 || ip.rfind("fd", 0) == 0) return "private-network-ipv6";
  if (ip.rfind("fe80", 0) == 0) return "link-local-ipv6";
  return "public-network";
}

static std::string detect_remote_ip_for_current_process() {
#ifdef _WIN32
  return "";
#else
  FILE* fp = popen("ss -tnp state established 2>/dev/null", "r");
  if (!fp) return "";
  std::array<char, 1024> buf{};
  std::string needle = "pid=" + std::to_string(::getpid()) + ",";
  std::set<std::string> strict_carrier_ips;
  std::set<std::string> fallback_ips;
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp)) {
    std::string line(buf.data());
    bool pid_matched = line.find(needle) != std::string::npos;
    std::vector<std::string> parts = split_ws(line);
    std::vector<std::string> endpoints;
    endpoints.reserve(parts.size());
    for (const auto& p : parts) {
      if (!endpoint_to_ip(p).empty()) endpoints.push_back(p);
    }
    if (endpoints.size() < 2) continue;
    std::string local_ip = endpoint_to_ip(endpoints[0]);
    std::string peer_ip = endpoint_to_ip(endpoints[1]);
    std::string peer_port = endpoint_to_port(endpoints[1]);
    std::string local_port = endpoint_to_port(endpoints[0]);
    (void)local_ip;
    (void)local_port;

    std::string ip = peer_ip;
    if (ip.empty() || ip == "127.0.0.1" || ip == "::1") continue;
    if (pid_matched && peer_port == "33445") {
      strict_carrier_ips.insert(ip);
      continue;
    }
    if (peer_port == "33445") {
      fallback_ips.insert(ip);
      continue;
    }
    if (pid_matched) fallback_ips.insert(ip);
  }
  pclose(fp);
  if (!strict_carrier_ips.empty()) return *strict_carrier_ips.begin();
  if (!fallback_ips.empty()) return *fallback_ips.begin();
  return "";
#endif
}

static std::vector<std::string> list_entries(const std::string& dirpath, bool want_dirs, bool suffix_lst) {
  std::vector<std::string> out;
  DIR* d = opendir(dirpath.c_str());
  if (!d) return out;
  struct dirent* ent = nullptr;
  while ((ent = readdir(d)) != nullptr) {
    std::string name = ent->d_name ? ent->d_name : "";
    if (name.empty() || name == "." || name == "..") continue;
    if (want_dirs && ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
    if (!want_dirs && ent->d_type == DT_DIR) continue;
    if (suffix_lst) {
      if (name.size() < 4 || name.substr(name.size() - 4) != ".lst") continue;
    }
    out.push_back(name);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

static std::vector<std::string> find_recent_crawler_lsts(const std::string& data_dir, size_t max_files) {
  std::string root = expand_home(data_dir);
  if (root.empty() || max_files == 0) return {};
  std::vector<std::string> dates = list_entries(root, true, false);
  if (dates.empty()) return {};
  std::vector<std::string> out;
  out.reserve(max_files);
  for (auto it = dates.rbegin(); it != dates.rend(); ++it) {
    std::string day_dir = root + "/" + *it;
    std::vector<std::string> files = list_entries(day_dir, false, true);
    if (files.empty()) continue;
    for (auto fit = files.rbegin(); fit != files.rend(); ++fit) {
      out.push_back(day_dir + "/" + *fit);
      if (out.size() >= max_files) return out;
    }
  }
  return out;
}

static bool parse_crawler_line(const std::string& line,
                               std::string& userid,
                               std::string& ip,
                               std::string& location) {
  size_t c1 = line.find(',');
  if (c1 == std::string::npos) return false;
  size_t c2 = line.find(',', c1 + 1);
  if (c2 == std::string::npos) return false;
  userid = trim_copy(line.substr(0, c1));
  ip = trim_copy(line.substr(c1 + 1, c2 - c1 - 1));
  location = trim_copy(line.substr(c2 + 1));
  if (userid.empty()) return false;
  if (!ip.empty() && !is_valid_ip_token(ip)) ip.clear();
  return true;
}

static void refresh_crawler_index_if_needed(RuntimeState* state) {
  if (!state || !state->db.use_crawler_index) return;
  std::time_t now = std::time(nullptr);
  {
    std::lock_guard<std::mutex> lock(state->crawler_mu);
    if (state->crawler_index_last_refresh != 0
        && now - state->crawler_index_last_refresh < state->db.crawler_refresh_seconds) {
      return;
    }
    state->crawler_index_last_refresh = now;
  }

  std::vector<std::string> files =
      find_recent_crawler_lsts(state->db.crawler_data_dir, static_cast<size_t>(state->db.crawler_lookback_files));
  if (files.empty()) return;

  std::string signature;
  signature.reserve(files.size() * 64);
  std::time_t newest_mtime = 0;
  std::string newest_file;
  for (const auto& f : files) {
    struct stat st{};
    if (stat(f.c_str(), &st) != 0) continue;
    signature += f;
    signature.push_back('|');
    signature += std::to_string(static_cast<long long>(st.st_mtime));
    signature.push_back(';');
    if (st.st_mtime >= newest_mtime) {
      newest_mtime = st.st_mtime;
      newest_file = f;
    }
  }
  if (signature.empty()) return;

  {
    std::lock_guard<std::mutex> lock(state->crawler_mu);
    if (state->crawler_index_signature == signature) return;
  }

  std::map<std::string, std::pair<std::string, std::string>> next;
  for (const auto& f : files) {
    std::ifstream in(f);
    if (!in) continue;
    std::string line;
    while (std::getline(in, line)) {
      std::string userid;
      std::string ip;
      std::string location;
      if (!parse_crawler_line(line, userid, ip, location)) continue;
      if (next.find(userid) != next.end()) continue;
      next[userid] = {ip, location};
    }
  }

  if (newest_file.empty()) newest_file = files.front();
  persist_crawler_index_to_db(state, next, newest_file, newest_mtime);
  {
    std::lock_guard<std::mutex> lock(state->crawler_mu);
    state->crawler_index.swap(next);
    state->crawler_index_signature = signature;
  }
  log_line("[beagle-sdk] crawler index loaded from last "
           + std::to_string(files.size()) + " file(s)");
}

static std::pair<std::string, std::string> lookup_ip_location_from_crawler(RuntimeState* state,
                                                                            const std::string& friendid) {
  if (!state || friendid.empty() || !state->db.use_crawler_index) return {"", ""};
  refresh_crawler_index_if_needed(state);
  {
    std::lock_guard<std::mutex> lock(state->crawler_mu);
    auto it = state->crawler_index.find(friendid);
    if (it != state->crawler_index.end()) return it->second;
  }
  return lookup_ip_location_from_crawler_cache_db(state, friendid);
}

static unsigned long long file_size_bytes(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  if (!S_ISREG(st.st_mode)) return 0;
  return static_cast<unsigned long long>(st.st_size);
}

static std::string basename_of(const std::string& path) {
  size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

static std::string sanitize_filename(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    if (c == '/' || c == '\\' || c == '\0') {
      out.push_back('_');
    } else {
      out.push_back(c);
    }
  }
  if (out.empty()) return "file.bin";
  return out;
}

static std::string lowercase(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static std::string infer_media_type_from_filename(const std::string& filename) {
  std::string lower = lowercase(filename);
  if (lower.size() >= 4 && lower.rfind(".jpg") == lower.size() - 4) return "image/jpeg";
  if (lower.size() >= 5 && lower.rfind(".jpeg") == lower.size() - 5) return "image/jpeg";
  if (lower.size() >= 4 && lower.rfind(".png") == lower.size() - 4) return "image/png";
  if (lower.size() >= 4 && lower.rfind(".gif") == lower.size() - 4) return "image/gif";
  if (lower.size() >= 5 && lower.rfind(".webp") == lower.size() - 5) return "image/webp";
  if (lower.size() >= 4 && lower.rfind(".mp4") == lower.size() - 4) return "video/mp4";
  if (lower.size() >= 4 && lower.rfind(".mp3") == lower.size() - 4) return "audio/mpeg";
  if (lower.size() >= 4 && lower.rfind(".wav") == lower.size() - 4) return "audio/wav";
  if (lower.size() >= 4 && lower.rfind(".pdf") == lower.size() - 4) return "application/pdf";
  return "application/octet-stream";
}

static std::string json_quote(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 2);
  out.push_back('"');
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
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

static bool read_file_binary(const std::string& path, std::vector<unsigned char>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  std::streamoff len = in.tellg();
  if (len <= 0) return false;
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(len));
  in.read(reinterpret_cast<char*>(out.data()), len);
  return static_cast<bool>(in);
}

static bool parse_json_string_field(const std::string& json,
                                    const std::string& key,
                                    std::string& value) {
  std::string marker = "\"" + key + "\"";
  size_t pos = json.find(marker);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + marker.size());
  if (pos == std::string::npos) return false;
  while (++pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {}
  if (pos >= json.size() || json[pos] != '"') return false;

  std::string out;
  bool escaped = false;
  for (++pos; pos < json.size(); ++pos) {
    char c = json[pos];
    if (escaped) {
      switch (c) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: out.push_back(c); break;
      }
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      value = out;
      return true;
    }
    out.push_back(c);
  }
  return false;
}

static bool parse_json_u64_field(const std::string& json,
                                 const std::string& key,
                                 uint64_t& value) {
  std::string marker = "\"" + key + "\"";
  size_t pos = json.find(marker);
  if (pos == std::string::npos) return false;
  pos = json.find(':', pos + marker.size());
  if (pos == std::string::npos) return false;
  while (++pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {}
  if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) return false;
  uint64_t out = 0;
  for (; pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])); ++pos) {
    out = out * 10 + static_cast<uint64_t>(json[pos] - '0');
  }
  value = out;
  return true;
}

static bool is_friend_offline_error(int err) {
  return err == CARRIER_GENERAL_ERROR(ERROR_FRIEND_OFFLINE);
}

static std::string trim_copy_simple(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  return s.substr(b, e - b);
}

static bool post_payload_to_express_node(RuntimeState* state,
                                         const std::string& receiver_id,
                                         const void* bytes,
                                         size_t len,
                                         std::string& detail) {
  if (!state || receiver_id.empty() || !bytes || len == 0 || state->user_id.empty()) {
    detail = "invalid args";
    return false;
  }

  char path_template[] = "/tmp/beagle_express_payload_XXXXXX";
  int fd = mkstemp(path_template);
  if (fd < 0) {
    detail = "mkstemp failed";
    return false;
  }

  bool write_ok = true;
  const unsigned char* p = static_cast<const unsigned char*>(bytes);
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, p + off, len - off);
    if (n <= 0) {
      write_ok = false;
      break;
    }
    off += static_cast<size_t>(n);
  }
  close(fd);
  if (!write_ok) {
    unlink(path_template);
    detail = "write temp payload failed";
    return false;
  }

  std::string url = "https://lens.beagle.chat:443/" + receiver_id + "/" + state->user_id;
  std::ostringstream cmd;
  cmd << "curl -sS -m 25 --connect-timeout 8 -o /dev/null -w \"%{http_code}\" "
      << "-H \"Content-Type: application/octet-stream\" "
      << "--data-binary @" << path_template << " "
      << "\"" << url << "\" 2>/dev/null";

  FILE* pipe = popen(cmd.str().c_str(), "r");
  if (!pipe) {
    unlink(path_template);
    detail = "popen curl failed";
    return false;
  }
  char buf[128];
  std::string out;
  while (fgets(buf, sizeof(buf), pipe)) out += buf;
  int rc = pclose(pipe);
  unlink(path_template);

  std::string code = trim_copy_simple(out);
  detail = "curl_rc=" + std::to_string(rc) + " http=" + code;
  return (code == "200" || code == "201");
}

struct PackedFilePayload {
  std::string filename;
  std::string content_type;
  uint64_t declared_size = 0;
  const unsigned char* bytes = nullptr;
  size_t bytes_len = 0;
};

static int b64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static bool base64_decode(const std::string& in, std::vector<unsigned char>& out) {
  out.clear();
  out.reserve((in.size() * 3) / 4);
  int val = 0;
  int valb = -8;
  for (unsigned char c : in) {
    if (std::isspace(c)) continue;
    if (c == '=') break;
    int d = b64_value(c);
    if (d < 0) return false;
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return !out.empty();
}

struct InlineJsonMedia {
  std::string filename;
  std::string media_type;
  std::vector<unsigned char> bytes;
};

static std::string trim_data_url_prefix(const std::string& data) {
  size_t comma = data.find(',');
  if (comma == std::string::npos) return data;
  std::string prefix = lowercase(data.substr(0, comma));
  if (prefix.find("base64") != std::string::npos) return data.substr(comma + 1);
  return data;
}

static bool has_file_extension(const std::string& filename) {
  size_t slash = filename.find_last_of("/\\");
  size_t dot = filename.find_last_of('.');
  return dot != std::string::npos && (slash == std::string::npos || dot > slash + 1);
}

static bool decode_inline_json_media_payload(const void* msg, size_t len, InlineJsonMedia& out) {
  if (!msg || len < 8 || len > (kMaxBeaglechatFileBytes * 2)) return false;
  std::string body(static_cast<const char*>(msg), len);
  while (!body.empty() && (body.back() == '\0' || std::isspace(static_cast<unsigned char>(body.back())))) {
    body.pop_back();
  }
  size_t start = 0;
  while (start < body.size() && std::isspace(static_cast<unsigned char>(body[start]))) start++;
  if (start > 0) body.erase(0, start);
  if (body.empty() || body.front() != '{' || body.back() != '}') return false;

  std::string type;
  if (!parse_json_string_field(body, "type", type)) return false;
  type = lowercase(type);
  if (type != "image" &&
      type != "file" &&
      type != "audio" &&
      type != "text" &&
      type != "unknown") return false;

  std::string data;
  if (!parse_json_string_field(body, "data", data) || data.empty()) return false;

  std::string filename;
  parse_json_string_field(body, "fileName", filename);
  if (filename.empty()) parse_json_string_field(body, "filename", filename);
  std::string ext;
  parse_json_string_field(body, "fileExtension", ext);
  if (!ext.empty() && ext[0] != '.') ext = "." + ext;
  if (!filename.empty() && !ext.empty() && !has_file_extension(filename)) filename += ext;
  if (filename.empty()) {
    filename = (type == "image" ? "image" : "file");
    if (!ext.empty()) filename += ext;
  }
  filename = sanitize_filename(filename);

  std::string media_type;
  if (!parse_json_string_field(body, "mediaType", media_type) || media_type.empty()) {
    media_type = infer_media_type_from_filename(filename);
  }

  std::vector<unsigned char> decoded;
  std::string b64 = trim_data_url_prefix(data);
  if (!base64_decode(b64, decoded)) return false;
  if (decoded.size() > kMaxBeaglechatFileBytes) return false;

  out.filename = filename;
  out.media_type = media_type;
  out.bytes.swap(decoded);
  return true;
}

static bool is_swift_filemodel_json_payload(const void* msg, size_t len) {
  if (!msg || len < 8 || len > (kMaxBeaglechatFileBytes * 2)) return false;
  std::string body(static_cast<const char*>(msg), len);
  while (!body.empty() && (body.back() == '\0' || std::isspace(static_cast<unsigned char>(body.back())))) {
    body.pop_back();
  }
  size_t start = 0;
  while (start < body.size() && std::isspace(static_cast<unsigned char>(body[start]))) start++;
  if (start > 0) body.erase(0, start);
  if (body.empty() || body.front() != '{' || body.back() != '}') return false;
  std::string file_name;
  std::string file_extension;
  std::string data_b64;
  if (!parse_json_string_field(body, "fileName", file_name) || file_name.empty()) return false;
  if (!parse_json_string_field(body, "fileExtension", file_extension) || file_extension.empty()) return false;
  if (!parse_json_string_field(body, "data", data_b64) || data_b64.empty()) return false;
  if (data_b64.find("base64,") != std::string::npos) return false;
  return true;
}

static bool decode_beaglechat_file_payload(const void* msg, size_t len, PackedFilePayload& out) {
  if (!msg || len < 5) return false;
  const unsigned char* p = static_cast<const unsigned char*>(msg);
  uint32_t meta_len = (static_cast<uint32_t>(p[0]) << 24)
                    | (static_cast<uint32_t>(p[1]) << 16)
                    | (static_cast<uint32_t>(p[2]) << 8)
                    | static_cast<uint32_t>(p[3]);
  if (meta_len == 0 || meta_len > 4096) return false;
  if (static_cast<size_t>(meta_len) + 4 > len) return false;

  std::string meta(reinterpret_cast<const char*>(p + 4), meta_len);
  std::string type;
  if (!parse_json_string_field(meta, "type", type) || type != "file") return false;

  std::string filename;
  if (!parse_json_string_field(meta, "filename", filename) || filename.empty()) return false;

  std::string content_type;
  if (!parse_json_string_field(meta, "contentType", content_type) || content_type.empty()) {
    content_type = infer_media_type_from_filename(filename);
  }

  uint64_t declared_size = 0;
  parse_json_u64_field(meta, "size", declared_size);

  out.filename = sanitize_filename(filename);
  out.content_type = content_type;
  out.declared_size = declared_size;
  out.bytes = p + 4 + meta_len;
  out.bytes_len = len - 4 - meta_len;
  return true;
}

static bool encode_beaglechat_file_payload(const std::string& filename,
                                           const std::string& content_type,
                                           const std::vector<unsigned char>& data,
                                           std::vector<unsigned char>& out) {
  std::ostringstream meta;
  meta << "{"
       << "\"type\":\"file\","
       << "\"filename\":" << json_quote(filename) << ","
       << "\"contentType\":" << json_quote(content_type.empty() ? "application/octet-stream" : content_type) << ","
       << "\"size\":" << data.size()
       << "}";
  std::string meta_json = meta.str();
  if (meta_json.empty() || meta_json.size() > 4096) return false;

  uint32_t meta_len = static_cast<uint32_t>(meta_json.size());
  out.resize(4 + meta_len + data.size());
  out[0] = static_cast<unsigned char>((meta_len >> 24) & 0xFF);
  out[1] = static_cast<unsigned char>((meta_len >> 16) & 0xFF);
  out[2] = static_cast<unsigned char>((meta_len >> 8) & 0xFF);
  out[3] = static_cast<unsigned char>(meta_len & 0xFF);
  std::memcpy(out.data() + 4, meta_json.data(), meta_json.size());
  if (!data.empty()) std::memcpy(out.data() + 4 + meta_json.size(), data.data(), data.size());
  return true;
}

static std::string base64_encode_bytes(const std::vector<unsigned char>& data) {
  static const char* kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= data.size()) {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16)
               | (static_cast<uint32_t>(data[i + 1]) << 8)
               | static_cast<uint32_t>(data[i + 2]);
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back(kTable[(n >> 6) & 0x3F]);
    out.push_back(kTable[n & 0x3F]);
    i += 3;
  }
  size_t rem = data.size() - i;
  if (rem == 1) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    uint32_t n = (static_cast<uint32_t>(data[i]) << 16)
               | (static_cast<uint32_t>(data[i + 1]) << 8);
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back(kTable[(n >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

static bool encode_inline_json_media_payload(const std::string& filename,
                                             const std::string& media_type,
                                             const std::vector<unsigned char>& data,
                                             std::string& out) {
  if (filename.empty() || data.empty()) return false;
  std::string mt = media_type.empty() ? infer_media_type_from_filename(filename) : media_type;
  std::string type = lowercase(mt).rfind("image/", 0) == 0 ? "image" : "file";
  std::string ext;
  size_t dot = filename.find_last_of('.');
  if (dot != std::string::npos && dot + 1 < filename.size()) {
    ext = filename.substr(dot + 1);
  }
  std::string b64 = base64_encode_bytes(data);
  if (b64.empty()) return false;
  std::string data_url = "data:" + mt + ";base64," + b64;

  std::ostringstream json;
  json << "{"
       << "\"type\":" << json_quote(type) << ","
       << "\"fileName\":" << json_quote(filename) << ","
       << "\"filename\":" << json_quote(filename) << ","
       << "\"fileExtension\":" << json_quote(ext) << ","
       << "\"mediaType\":" << json_quote(mt) << ","
       << "\"data\":" << json_quote(data_url)
       << "}";
  out = json.str();
  return !out.empty();
}

static bool encode_swift_filemodel_media_payload(const std::string& filename,
                                                 const std::string& media_type,
                                                 const std::vector<unsigned char>& data,
                                                 std::string& out) {
  if (filename.empty() || data.empty()) return false;
  std::string stem = filename;
  std::string ext = ".bin";
  size_t dot = filename.find_last_of('.');
  if (dot != std::string::npos && dot > 0 && dot + 1 < filename.size()) {
    stem = filename.substr(0, dot);
    ext = filename.substr(dot);
  }
  if (stem.empty()) stem = "file";
  if (ext.empty() || ext[0] != '.') ext = "." + ext;
  std::string mt = lowercase(media_type.empty() ? infer_media_type_from_filename(filename) : media_type);
  std::string type = "unknown";
  if (mt.rfind("image/", 0) == 0) type = "image";
  else if (mt.rfind("audio/", 0) == 0) type = "audio";
  else if (mt.rfind("text/", 0) == 0) type = "text";
  std::string b64 = base64_encode_bytes(data);
  if (b64.empty()) return false;

  std::ostringstream json;
  json << "{"
       << "\"fileName\":" << json_quote(stem) << ","
       << "\"fileExtension\":" << json_quote(ext) << ","
       << "\"data\":" << json_quote(b64) << ","
       << "\"type\":" << json_quote(type)
       << "}";
  out = json.str();
  return !out.empty();
}

static bool encode_legacy_inline_data_payload(const std::vector<unsigned char>& data,
                                              std::string& out) {
  if (data.empty()) return false;
  std::string b64 = base64_encode_bytes(data);
  if (b64.empty()) return false;
  std::ostringstream json;
  json << "{"
       << "\"data\":" << json_quote(b64)
       << "}";
  out = json.str();
  return !out.empty();
}

static bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

static bool write_file(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << data;
  return static_cast<bool>(out);
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
    }
  }
  return out;
}

static std::string dedupe_text_fragment(const std::string& text) {
  if (text.size() <= 256) return text;
  return text.substr(0, 192) + "...|" + std::to_string(text.size()) + "|..." + text.substr(text.size() - 48);
}

static std::string preview_text(const std::string& text, size_t max_len = 120) {
  std::string out;
  out.reserve(std::min(text.size(), max_len));
  for (char c : text) {
    if (c == '\0') continue;
    if (c == '\n' || c == '\r' || c == '\t') {
      out.push_back(' ');
      continue;
    }
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) continue;
    out.push_back(c);
    if (out.size() >= max_len) break;
  }
  return out;
}

static std::string build_incoming_signature(const BeagleIncomingMessage& incoming, bool offline) {
  std::ostringstream sig;
  sig << incoming.peer << "|"
      << incoming.ts << "|"
      << (offline ? 1 : 0) << "|"
      << incoming.filename << "|"
      << incoming.media_type << "|"
      << incoming.size << "|"
      << dedupe_text_fragment(incoming.text);
  return sig.str();
}

static bool remember_incoming_signature(RuntimeState* state, const std::string& signature) {
  if (!state) return true;
  constexpr size_t kMaxSeenIncoming = 20000;
  std::lock_guard<std::mutex> lock(state->state_mu);
  auto inserted = state->seen_incoming_signatures.insert(signature);
  if (!inserted.second) return false;
  state->seen_incoming_order.push_back(signature);
  while (state->seen_incoming_order.size() > kMaxSeenIncoming) {
    const std::string& oldest = state->seen_incoming_order.front();
    state->seen_incoming_signatures.erase(oldest);
    state->seen_incoming_order.pop_front();
  }
  return true;
}

static void ensure_profile_file(RuntimeState* state);

static bool append_line(const std::string& path, const std::string& line) {
  std::ofstream out(path, std::ios::app);
  if (!out) return false;
  out << line << "\n";
  return static_cast<bool>(out);
}

static void log_incoming_event(RuntimeState* state,
                               const BeagleIncomingMessage& incoming,
                               bool offline,
                               const char* action,
                               const std::string& signature) {
  if (!state || state->incoming_event_log_path.empty()) return;
  std::ostringstream line;
  line << "{"
       << "\"loggedAt\":\"" << json_escape(log_ts()) << "\","
       << "\"action\":\"" << json_escape(action ? action : "") << "\","
       << "\"peer\":\"" << json_escape(incoming.peer) << "\","
       << "\"mode\":\"" << (offline ? "offline" : "online") << "\","
       << "\"carrierTs\":" << incoming.ts << ","
       << "\"startupTs\":" << state->startup_ts_us << ","
       << "\"kind\":\"" << (incoming.media_path.empty() ? "text" : "file") << "\","
       << "\"text\":\"" << json_escape(incoming.text) << "\","
       << "\"textPreview\":\"" << json_escape(preview_text(incoming.text, 200)) << "\","
       << "\"filename\":\"" << json_escape(incoming.filename) << "\","
       << "\"mediaType\":\"" << json_escape(incoming.media_type) << "\","
       << "\"size\":" << incoming.size << ","
       << "\"signature\":\"" << json_escape(signature) << "\""
       << "}";
  append_line(state->incoming_event_log_path, line.str());
}

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

static bool extract_json_int(const std::string& body, const std::string& key, int& out) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return false;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos++;
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
  size_t end = pos;
  while (end < body.size() && (body[end] == '-' || std::isdigit(static_cast<unsigned char>(body[end])))) end++;
  if (end == pos) return false;
  out = std::atoi(body.substr(pos, end - pos).c_str());
  return true;
}

static bool extract_json_bool(const std::string& body, const std::string& key, bool& out) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return false;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos++;
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
  if (body.compare(pos, 4, "true") == 0) {
    out = true;
    return true;
  }
  if (body.compare(pos, 5, "false") == 0) {
    out = false;
    return true;
  }
  return false;
}

static std::string default_profile_json() {
  return std::string("{\n")
      + "  \"welcomeMessage\": \"Hi! I'm the Beagle OpenClaw bot. Send a message to start.\",\n"
      + "  \"profile\": {\n"
      + "    \"name\": \"Snoopy\",\n"
      + "    \"gender\": \"2218\",\n"
      + "    \"phone\": \"Claw Bot to Help\",\n"
      + "    \"email\": \"SOL:,ETH:\",\n"
      + "    \"description\": \"Ask me anything about beagle chat, Tell me who your are\",\n"
      + "    \"region\": \"California\",\n"
      + "    \"carrierUserId\": \"\",\n"
      + "    \"carrierAddress\": \"\",\n"
      + "    \"startedAt\": \"\"\n"
      + "  }\n"
      + "}\n";
}

static std::string iso8601_utc_now() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

static bool find_json_object_bounds(const std::string& body,
                                    const std::string& key,
                                    size_t& start,
                                    size_t& end) {
  std::string needle = "\"" + key + "\"";
  size_t key_pos = body.find(needle);
  if (key_pos == std::string::npos) return false;
  size_t brace = body.find('{', key_pos + needle.size());
  if (brace == std::string::npos) return false;
  int depth = 0;
  for (size_t i = brace; i < body.size(); ++i) {
    if (body[i] == '{') depth++;
    else if (body[i] == '}') {
      depth--;
      if (depth == 0) {
        start = brace;
        end = i;
        return true;
      }
    }
  }
  return false;
}

static bool replace_json_string_value(std::string& body,
                                      size_t obj_start,
                                      size_t obj_end,
                                      const std::string& key,
                                      const std::string& value) {
  std::string needle = "\"" + key + "\"";
  size_t pos = body.find(needle, obj_start);
  if (pos == std::string::npos || pos > obj_end) return false;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos || pos > obj_end) return false;
  pos = body.find('"', pos);
  if (pos == std::string::npos || pos > obj_end) return false;
  size_t end = body.find('"', pos + 1);
  if (end == std::string::npos || end > obj_end) return false;
  body.replace(pos + 1, end - pos - 1, json_escape(value));
  return true;
}

static bool insert_json_string_value(std::string& body,
                                     size_t obj_start,
                                     size_t obj_end,
                                     const std::string& key,
                                     const std::string& value) {
  size_t pos = obj_end;
  while (pos > obj_start && std::isspace(static_cast<unsigned char>(body[pos - 1]))) pos--;
  bool needs_comma = (pos > obj_start + 1 && body[pos - 1] != '{');
  std::string insert = (needs_comma ? "," : "");
  insert += "\n    \"" + key + "\": \"" + json_escape(value) + "\"";
  body.insert(obj_end, insert);
  return true;
}

static bool upsert_profile_field(std::string& body,
                                 const std::string& key,
                                 const std::string& value,
                                 bool only_if_missing) {
  if (value.empty()) return false;
  size_t obj_start = 0;
  size_t obj_end = 0;
  if (!find_json_object_bounds(body, "profile", obj_start, obj_end)) return false;
  std::string existing;
  if (extract_json_string(body.substr(obj_start, obj_end - obj_start + 1), key, existing)) {
    if (existing == value) return false;
    if (only_if_missing && !existing.empty()) return false;
    return replace_json_string_value(body, obj_start, obj_end, key, value);
  }
  return insert_json_string_value(body, obj_start, obj_end, key, value);
}

static std::string load_wallet_public_key() {
  const char* home = std::getenv("HOME");
  if (!home) return std::string();
  std::string path = std::string(home) + "/.openclaw/workspace/licode_wallet.json";
  std::string body;
  if (!read_file(path, body)) return std::string();
  std::string pubkey;
  extract_json_string(body, "publicKey", pubkey);
  return pubkey;
}

static void ensure_profile_metadata(RuntimeState* state,
                                    const std::string& user_id,
                                    const std::string& address) {
  if (!state || state->profile_path.empty()) return;
  ensure_profile_file(state);
  std::string body;
  if (!read_file(state->profile_path, body)) return;

  bool changed = false;
  changed |= upsert_profile_field(body, "carrierUserId", user_id, false);
  changed |= upsert_profile_field(body, "carrierAddress", address, false);

  std::string started_at;
  if (!extract_json_string(body, "startedAt", started_at) || started_at.empty()) {
    changed |= upsert_profile_field(body, "startedAt", iso8601_utc_now(), true);
  }

  std::string email;
  extract_json_string(body, "email", email);
  std::string wallet = load_wallet_public_key();
  if (!wallet.empty()) {
    bool placeholder = email.empty()
        || email.find("SOL:") != std::string::npos
        || email.find("ETH:") != std::string::npos;
    if (placeholder && email != wallet) {
      changed |= upsert_profile_field(body, "email", wallet, false);
    }
  }

  if (changed) {
    write_file(state->profile_path, body);
  }
}

static std::string default_db_json() {
  return std::string("{\n")
      + "  \"enabled\": false,\n"
      + "  \"host\": \"localhost\",\n"
      + "  \"port\": 3306,\n"
      + "  \"user\": \"beagle\",\n"
      + "  \"password\": \"A1anSn00py\",\n"
      + "  \"database\": \"beagle\",\n"
      + "  \"useCrawlerIndex\": false,\n"
      + "  \"crawlerDataDir\": \"~/.elacrawler\",\n"
      + "  \"crawlerRefreshSeconds\": 60,\n"
      + "  \"crawlerLookbackFiles\": 20\n"
      + "}\n";
}

static void ensure_profile_file(RuntimeState* state) {
  if (!state || state->profile_path.empty()) return;
  if (file_exists(state->profile_path)) return;
  if (!write_file(state->profile_path, default_profile_json())) {
    log_line(std::string("[beagle-sdk] failed to write default profile to ") + state->profile_path);
  }
}

static void ensure_db_file(RuntimeState* state) {
  if (!state || state->db_config_path.empty()) return;
  if (file_exists(state->db_config_path)) return;
  if (!write_file(state->db_config_path, default_db_json())) {
    log_line(std::string("[beagle-sdk] failed to write default db config to ") + state->db_config_path);
  }
}

static void load_profile(RuntimeState* state, ProfileInfo& profile) {
  if (!state) return;
  ensure_profile_file(state);
  std::string body;
  if (!read_file(state->profile_path, body)) return;
  extract_json_string(body, "welcomeMessage", state->welcome_message);
  extract_json_string(body, "name", profile.name);
  extract_json_string(body, "gender", profile.gender);
  extract_json_string(body, "phone", profile.phone);
  extract_json_string(body, "email", profile.email);
  extract_json_string(body, "description", profile.description);
  extract_json_string(body, "region", profile.region);
}

static void load_db_config(RuntimeState* state, DbConfig& db) {
  if (!state) return;
  ensure_db_file(state);
  std::string body;
  if (!read_file(state->db_config_path, body)) return;
  extract_json_bool(body, "enabled", db.enabled);
  extract_json_string(body, "host", db.host);
  extract_json_int(body, "port", db.port);
  extract_json_string(body, "user", db.user);
  extract_json_string(body, "password", db.password);
  extract_json_string(body, "database", db.database);
  extract_json_bool(body, "useCrawlerIndex", db.use_crawler_index);
  extract_json_string(body, "crawlerDataDir", db.crawler_data_dir);
  extract_json_int(body, "crawlerRefreshSeconds", db.crawler_refresh_seconds);
  extract_json_int(body, "crawlerLookbackFiles", db.crawler_lookback_files);
  if (db.crawler_refresh_seconds < 5) db.crawler_refresh_seconds = 5;
  if (db.crawler_lookback_files < 1) db.crawler_lookback_files = 1;
  if (db.crawler_lookback_files > 200) db.crawler_lookback_files = 200;
}

static void load_welcomed_peers(RuntimeState* state) {
  if (!state || state->welcome_state_path.empty()) return;
  std::ifstream in(state->welcome_state_path);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) state->welcomed_peers.insert(line);
  }
}

static void save_welcomed_peers(RuntimeState* state) {
  if (!state || state->welcome_state_path.empty()) return;
  std::ostringstream out;
  {
    std::lock_guard<std::mutex> lock(state->state_mu); // Lock access to welcomed_peers
    for (const auto& peer : state->welcomed_peers) {
      out << peer << "\n";
    }
  }
  write_file(state->welcome_state_path, out.str());
}

static std::string sanitize_tsv(const std::string& in) {
  std::string out = in;
  for (char& c : out) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return out;
}

static void load_friend_state(RuntimeState* state) {
  if (!state || state->friend_state_path.empty()) return;
  std::ifstream in(state->friend_state_path);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::vector<std::string> fields;
    std::string current;
    for (char c : line) {
      if (c == '\t') {
        fields.push_back(current);
        current.clear();
      } else {
        current.push_back(c);
      }
    }
    fields.push_back(current);
    if (fields.size() < 10) continue;
    FriendState fs;
    fs.friendid = fields[0];
    fs.name = fields[1];
    fs.gender = fields[2];
    fs.phone = fields[3];
    fs.email = fields[4];
    fs.description = fields[5];
    fs.region = fields[6];
    fs.label = fields[7];
    fs.status = std::atoi(fields[8].c_str());
    fs.presence = std::atoi(fields[9].c_str());
    if (!fs.friendid.empty()) state->friend_state[fs.friendid] = std::move(fs);
  }
}

static void save_friend_state(RuntimeState* state) {
  if (!state || state->friend_state_path.empty()) return;
  std::ostringstream out;
  for (const auto& kv : state->friend_state) {
    const FriendState& fs = kv.second;
    out << sanitize_tsv(fs.friendid) << "\t"
        << sanitize_tsv(fs.name) << "\t"
        << sanitize_tsv(fs.gender) << "\t"
        << sanitize_tsv(fs.phone) << "\t"
        << sanitize_tsv(fs.email) << "\t"
        << sanitize_tsv(fs.description) << "\t"
        << sanitize_tsv(fs.region) << "\t"
        << sanitize_tsv(fs.label) << "\t"
        << fs.status << "\t"
        << fs.presence << "\n";
  }
  write_file(state->friend_state_path, out.str());
}

static std::string sql_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    if (c == '\\' || c == '\'') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

static std::string shell_escape(const std::string& in) {
  std::string out = "'";
  for (char c : in) {
    if (c == '\'') {
      out += "'\"'\"'";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

static int mysql_exec(const DbConfig& db, const std::string& sql) {
  if (!db.enabled) return 0;
  std::ostringstream cmd;
  cmd << "mysql --protocol=TCP"
      << " --host=" << shell_escape(db.host)
      << " --port=" << db.port
      << " --user=" << shell_escape(db.user)
      << " --password=" << shell_escape(db.password);
  if (!db.database.empty()) cmd << " --database=" << shell_escape(db.database);
  cmd << " --execute=" << shell_escape(sql);
  int rc = std::system(cmd.str().c_str());
  return rc;
}

static bool mysql_query_has_rows(const DbConfig& db, const std::string& sql) {
  if (!db.enabled) return false;
  std::ostringstream cmd;
  cmd << "mysql --batch --skip-column-names --raw --protocol=TCP"
      << " --host=" << shell_escape(db.host)
      << " --port=" << db.port
      << " --user=" << shell_escape(db.user)
      << " --password=" << shell_escape(db.password);
  if (!db.database.empty()) cmd << " --database=" << shell_escape(db.database);
  cmd << " --execute=" << shell_escape(sql) << " 2>/dev/null";
  FILE* fp = popen(cmd.str().c_str(), "r");
  if (!fp) return false;
  char line[64];
  bool has = std::fgets(line, sizeof(line), fp) != nullptr;
  pclose(fp);
  return has;
}

static bool mysql_query_first_line(const DbConfig& db,
                                   const std::string& sql,
                                   std::string& out_line) {
  if (!db.enabled) return false;
  std::ostringstream cmd;
  cmd << "mysql --batch --skip-column-names --raw --protocol=TCP"
      << " --host=" << shell_escape(db.host)
      << " --port=" << db.port
      << " --user=" << shell_escape(db.user)
      << " --password=" << shell_escape(db.password);
  if (!db.database.empty()) cmd << " --database=" << shell_escape(db.database);
  cmd << " --execute=" << shell_escape(sql) << " 2>/dev/null";
  FILE* fp = popen(cmd.str().c_str(), "r");
  if (!fp) return false;
  char line[1024];
  bool ok = std::fgets(line, sizeof(line), fp) != nullptr;
  pclose(fp);
  if (!ok) return false;
  out_line = line;
  while (!out_line.empty() && (out_line.back() == '\n' || out_line.back() == '\r')) {
    out_line.pop_back();
  }
  return !out_line.empty();
}

static bool mysql_column_exists(const DbConfig& db,
                                const std::string& table,
                                const std::string& column) {
  std::ostringstream sql;
  sql << "SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA='"
      << sql_escape(db.database) << "' AND TABLE_NAME='"
      << sql_escape(table) << "' AND COLUMN_NAME='"
      << sql_escape(column) << "' LIMIT 1;";
  return mysql_query_has_rows(db, sql.str());
}

static void persist_crawler_index_to_db(RuntimeState* state,
                                        const std::map<std::string, std::pair<std::string, std::string>>& rows,
                                        const std::string& source_file,
                                        std::time_t seen_at) {
  if (!state || !state->db.enabled || rows.empty()) return;
  std::ostringstream sql;
  sql << "REPLACE INTO beagle_crawler_node_cache(userid,ip,location,source_file,seen_at,updated_at) VALUES ";
  bool first = true;
  for (const auto& kv : rows) {
    const std::string& userid = kv.first;
    const std::string& ip = kv.second.first;
    const std::string& location = kv.second.second;
    if (userid.empty()) continue;
    if (!first) sql << ",";
    first = false;
    sql << "('"
        << sql_escape(userid) << "','"
        << sql_escape(ip) << "','"
        << sql_escape(location) << "','"
        << sql_escape(source_file) << "',"
        << "FROM_UNIXTIME(" << static_cast<long long>(seen_at) << "),"
        << "NOW())";
  }
  if (first) return;
  sql << ";";
  int rc = mysql_exec(state->db, sql.str());
  if (rc != 0) {
    log_line("[beagle-sdk] crawler cache persist failed rc=" + std::to_string(rc));
  } else {
    log_line("[beagle-sdk] crawler cache persisted rows=" + std::to_string(rows.size()));
  }
}

static std::pair<std::string, std::string> lookup_ip_location_from_crawler_cache_db(RuntimeState* state,
                                                                                     const std::string& friendid) {
  if (!state || !state->db.enabled || friendid.empty()) return {"", ""};
  std::ostringstream sql;
  sql << "SELECT ip,location FROM beagle_crawler_node_cache WHERE userid='"
      << sql_escape(friendid) << "' LIMIT 1;";
  std::string line;
  if (!mysql_query_first_line(state->db, sql.str(), line)) return {"", ""};
  size_t tab = line.find('\t');
  if (tab == std::string::npos) return {"", ""};
  std::string ip = line.substr(0, tab);
  std::string location = line.substr(tab + 1);
  return {ip, location};
}

static void ensure_db(RuntimeState* state, const DbConfig& db) {
  if (!state || !db.enabled) return;
  std::string create_db = "CREATE DATABASE IF NOT EXISTS " + db.database + ";";
  {
    std::ostringstream cmd;
    cmd << "mysql --protocol=TCP"
        << " --host=" << shell_escape(db.host)
        << " --port=" << db.port
        << " --user=" << shell_escape(db.user)
        << " --password=" << shell_escape(db.password)
        << " --execute=" << shell_escape(create_db);
    std::system(cmd.str().c_str());
  }

  const char* schema =
      "CREATE TABLE IF NOT EXISTS beagle_friend_info ("
      "friendid VARCHAR(128) PRIMARY KEY,"
      "name VARCHAR(128),"
      "gender VARCHAR(64),"
      "phone VARCHAR(64),"
      "email VARCHAR(256),"
      "description TEXT,"
      "region VARCHAR(128),"
      "label VARCHAR(128),"
      "status INT,"
      "presence INT,"
      "updated_at DATETIME"
      ");"
      "CREATE TABLE IF NOT EXISTS beagle_friend_info_history ("
      "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
      "friendid VARCHAR(128),"
      "name VARCHAR(128),"
      "gender VARCHAR(64),"
      "phone VARCHAR(64),"
      "email VARCHAR(256),"
      "description TEXT,"
      "region VARCHAR(128),"
      "label VARCHAR(128),"
      "status INT,"
      "presence INT,"
      "changed_at DATETIME"
      ");"
      "CREATE TABLE IF NOT EXISTS beagle_friend_events ("
      "id BIGINT AUTO_INCREMENT PRIMARY KEY,"
      "friendid VARCHAR(128),"
      "event_type VARCHAR(32),"
      "status INT,"
      "presence INT,"
      "ip VARCHAR(64),"
      "location VARCHAR(128),"
      "ts DATETIME"
      ");"
      "CREATE TABLE IF NOT EXISTS beagle_crawler_node_cache ("
      "userid VARCHAR(128) PRIMARY KEY,"
      "ip VARCHAR(64),"
      "location VARCHAR(128),"
      "source_file VARCHAR(255),"
      "seen_at DATETIME,"
      "updated_at DATETIME,"
      "KEY idx_seen_at (seen_at)"
      ");";
  int rc = mysql_exec(db, schema);
  if (rc != 0) {
    log_line(std::string("[beagle-sdk] mysql schema init failed rc=") + std::to_string(rc));
  }
  if (!mysql_column_exists(db, "beagle_friend_events", "ip")) {
    mysql_exec(db, "ALTER TABLE beagle_friend_events ADD COLUMN ip VARCHAR(64) NULL AFTER presence;");
  }
  if (!mysql_column_exists(db, "beagle_friend_events", "location")) {
    mysql_exec(db, "ALTER TABLE beagle_friend_events ADD COLUMN location VARCHAR(128) NULL AFTER ip;");
  }
}

static std::string now_mysql_ts() {
  std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&now, &tm_buf);
  char out[32];
  if (std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &tm_buf) == 0) return "";
  return std::string(out);
}

static void log_friend_event(RuntimeState* state,
                             const std::string& friendid,
                             const std::string& event_type,
                             int status,
                             int presence) {
  std::string ts = now_mysql_ts();
  std::string ip;
  std::string location;
  if (state->db.use_crawler_index) {
    auto resolved = lookup_ip_location_from_crawler(state, friendid);
    ip = resolved.first;
    location = resolved.second;
    if (!ip.empty() && location.empty()) {
      location = guess_location_from_ip(ip);
    }
  } else {
    ip = detect_remote_ip_for_current_process();
    location = guess_location_from_ip(ip);
  }
  if (!state->friend_event_log_path.empty()) {
    std::ostringstream line;
    line << ts << "\t" << friendid << "\t" << event_type
         << "\tstatus=" << status << "\tpresence=" << presence
         << "\tip=" << (ip.empty() ? "-" : ip)
         << "\tlocation=" << (location.empty() ? "-" : location);
    append_line(state->friend_event_log_path, line.str());
  }
  if (state->db.enabled) {
    std::ostringstream sql;
    sql << "INSERT INTO beagle_friend_events(friendid,event_type,status,presence,ip,location,ts) VALUES('"
        << sql_escape(friendid) << "','"
        << sql_escape(event_type) << "',"
        << status << ","
        << presence << ",'"
        << sql_escape(ip) << "','"
        << sql_escape(location) << "','"
        << sql_escape(ts) << "');";
    mysql_exec(state->db, sql.str());
  }
}

static bool friend_info_equals(const FriendState& fs, const CarrierFriendInfo* info) {
  if (!info) return true;
  const CarrierUserInfo& ui = info->user_info;
  if (fs.name != ui.name) return false;
  if (fs.gender != ui.gender) return false;
  if (fs.phone != ui.phone) return false;
  if (fs.email != ui.email) return false;
  if (fs.description != ui.description) return false;
  if (fs.region != ui.region) return false;
  if (fs.label != info->label) return false;
  if (fs.status != static_cast<int>(info->status)) return false;
  if (fs.presence != static_cast<int>(info->presence)) return false;
  return true;
}

static FriendState from_friend_info(const std::string& friendid, const CarrierFriendInfo* info) {
  FriendState fs;
  fs.friendid = friendid;
  if (!info) return fs;
  const CarrierUserInfo& ui = info->user_info;
  fs.name = ui.name;
  fs.gender = ui.gender;
  fs.phone = ui.phone;
  fs.email = ui.email;
  fs.description = ui.description;
  fs.region = ui.region;
  fs.label = info->label;
  fs.status = static_cast<int>(info->status);
  fs.presence = static_cast<int>(info->presence);
  return fs;
}

static void store_friend_info(RuntimeState* state,
                              const std::string& friendid,
                              const CarrierFriendInfo* info) {
  if (!state || friendid.empty() || !info) return;
  std::lock_guard<std::mutex> lock(state->state_mu);
  auto it = state->friend_state.find(friendid);
  FriendState next = from_friend_info(friendid, info);
  if (it != state->friend_state.end() && friend_info_equals(it->second, info)) return;
  state->friend_state[friendid] = next;
  save_friend_state(state);

  if (state->db.enabled) {
    std::string ts = now_mysql_ts();
    const CarrierUserInfo& ui = info->user_info;
    std::ostringstream upsert;
    upsert << "REPLACE INTO beagle_friend_info(friendid,name,gender,phone,email,description,region,label,status,presence,updated_at) VALUES('"
           << sql_escape(friendid) << "','"
           << sql_escape(ui.name) << "','"
           << sql_escape(ui.gender) << "','"
           << sql_escape(ui.phone) << "','"
           << sql_escape(ui.email) << "','"
           << sql_escape(ui.description) << "','"
           << sql_escape(ui.region) << "','"
           << sql_escape(info->label) << "',"
           << static_cast<int>(info->status) << ","
           << static_cast<int>(info->presence) << ",'"
           << sql_escape(ts) << "');";
    mysql_exec(state->db, upsert.str());

    std::ostringstream history;
    history << "INSERT INTO beagle_friend_info_history(friendid,name,gender,phone,email,description,region,label,status,presence,changed_at) VALUES('"
            << sql_escape(friendid) << "','"
            << sql_escape(ui.name) << "','"
            << sql_escape(ui.gender) << "','"
            << sql_escape(ui.phone) << "','"
            << sql_escape(ui.email) << "','"
            << sql_escape(ui.description) << "','"
            << sql_escape(ui.region) << "','"
            << sql_escape(info->label) << "',"
            << static_cast<int>(info->status) << ","
            << static_cast<int>(info->presence) << ",'"
            << sql_escape(ts) << "');";
    mysql_exec(state->db, history.str());
  }
}

static void update_friend_status(RuntimeState* state,
                                 const std::string& friendid,
                                 int status,
                                 int presence,
                                 bool log_event) {
  if (!state || friendid.empty()) return;
  std::lock_guard<std::mutex> lock(state->state_mu);
  auto it = state->friend_state.find(friendid);
  if (it == state->friend_state.end()) {
    FriendState fs;
    fs.friendid = friendid;
    fs.status = status >= 0 ? status : 0;
    fs.presence = presence >= 0 ? presence : 0;
    state->friend_state[friendid] = fs;
    save_friend_state(state);
    if (log_event) log_friend_event(state, friendid, status ? "online" : "offline", status, presence);
    return;
  }
  int next_status = status >= 0 ? status : it->second.status;
  int next_presence = presence >= 0 ? presence : it->second.presence;
  bool changed = (it->second.status != next_status);
  bool presence_changed = (it->second.presence != next_presence);
  if (!changed && !presence_changed) {
    if (log_event) {
      log_friend_event(state, friendid, next_status ? "online" : "offline", next_status, next_presence);
    }
    return;
  }
  it->second.status = next_status;
  it->second.presence = next_presence;
  save_friend_state(state);
  if (log_event && changed) log_friend_event(state, friendid, next_status ? "online" : "offline", next_status, next_presence);
}

static bool is_friend_online(RuntimeState* state, const std::string& friendid, bool& known) {
  known = false;
  if (!state || friendid.empty()) return false;
  std::lock_guard<std::mutex> lock(state->state_mu);
  auto it = state->friend_state.find(friendid);
  if (it == state->friend_state.end()) return false;
  known = true;
  return it->second.status != 0;
}

static void apply_profile(RuntimeState* state, const ProfileInfo& profile) {
  if (!state || !state->carrier) return;
  CarrierUserInfo info;
  if (carrier_get_self_info(state->carrier, &info) < 0) {
    std::memset(&info, 0, sizeof(info));
  }
  if (!profile.name.empty()) std::strncpy(info.name, profile.name.c_str(), sizeof(info.name) - 1);
  if (!profile.gender.empty()) std::strncpy(info.gender, profile.gender.c_str(), sizeof(info.gender) - 1);
  if (!profile.phone.empty()) std::strncpy(info.phone, profile.phone.c_str(), sizeof(info.phone) - 1);
  if (!profile.email.empty()) std::strncpy(info.email, profile.email.c_str(), sizeof(info.email) - 1);
  if (!profile.description.empty()) std::strncpy(info.description, profile.description.c_str(), sizeof(info.description) - 1);
  if (!profile.region.empty()) std::strncpy(info.region, profile.region.c_str(), sizeof(info.region) - 1);

  int rc = carrier_set_self_info(state->carrier, &info);
  if (rc < 0) {
    std::ostringstream msg;
    msg << "[beagle-sdk] set self info failed: 0x" << std::hex << carrier_get_error() << std::dec;
    log_line(msg.str());
  } else {
    log_line("[beagle-sdk] self info updated");
  }
}

static void send_welcome_once(RuntimeState* state, const std::string& peer, const char* reason) {
  if (!state || peer.empty() || !state->carrier) return;
  {
    std::lock_guard<std::mutex> lock(state->state_mu);
    if (state->welcomed_peers.find(peer) != state->welcomed_peers.end()) return;
  }

  const std::string msg = state->welcome_message.empty()
      ? "Hi! I'm the Beagle OpenClaw bot. Send a message to start."
      : state->welcome_message;
  uint32_t msgid = 0;
  int rc = carrier_send_friend_message(state->carrier,
                                       peer.c_str(),
                                       msg.data(),
                                       msg.size(),
                                       &msgid,
                                       nullptr,
                                       nullptr);
  if (rc < 0) {
    std::ostringstream msg;
    msg << "[beagle-sdk] welcome message failed (" << reason
        << "): 0x" << std::hex << carrier_get_error() << std::dec;
    log_line(msg.str());
  } else {
    {
      std::lock_guard<std::mutex> lock(state->state_mu);
      state->welcomed_peers.insert(peer);
    }
    if (!state->welcome_state_path.empty()) {
      save_welcomed_peers(state);
    }
    log_line(std::string("[beagle-sdk] welcome message sent (") + reason + ") to " + peer);
  }
}

static void register_transfer(const std::shared_ptr<TransferContext>& ctx) {
  if (!ctx || !ctx->ft) return;
  std::lock_guard<std::mutex> lock(g_ft_mu);
  g_transfers[ctx->ft] = ctx;
}

static std::shared_ptr<TransferContext> get_transfer(CarrierFileTransfer* ft) {
  if (!ft) return nullptr;
  std::lock_guard<std::mutex> lock(g_ft_mu);
  auto it = g_transfers.find(ft);
  if (it == g_transfers.end()) return nullptr;
  return it->second;
}

static std::shared_ptr<TransferContext> take_transfer(CarrierFileTransfer* ft) {
  if (!ft) return nullptr;
  std::lock_guard<std::mutex> lock(g_ft_mu);
  auto it = g_transfers.find(ft);
  if (it == g_transfers.end()) return nullptr;
  auto ctx = it->second;
  g_transfers.erase(it);
  return ctx;
}

static void emit_incoming_file_event(const std::shared_ptr<TransferContext>& ctx) {
  if (!ctx || !ctx->state || !ctx->state->on_incoming) return;
  BeagleIncomingMessage incoming;
  incoming.peer = ctx->peer;
  incoming.text = "";
  incoming.media_path = ctx->target_path;
  incoming.media_type = ctx->media_type;
  incoming.filename = ctx->filename;
  incoming.size = static_cast<unsigned long long>(ctx->transferred);
  incoming.ts = static_cast<long long>(std::time(nullptr));
  ctx->state->on_incoming(incoming);

  std::ostringstream line;
  line << "[beagle-sdk] received file from " << ctx->peer
       << " file=" << ctx->filename
       << " size=" << ctx->transferred
       << " path=" << ctx->target_path;
  log_line(line.str());
}

static void filetransfer_state_changed_callback(CarrierFileTransfer* ft,
                                                FileTransferConnection state,
                                                void* context) {
  (void)context;
  auto ctx = get_transfer(ft);
  if (!ctx) return;
  ctx->connected = (state == FileTransferConnection_connected);
  std::string state_name = "unknown";
  if (state == FileTransferConnection_initialized) state_name = "initialized";
  else if (state == FileTransferConnection_connecting) state_name = "connecting";
  else if (state == FileTransferConnection_connected) state_name = "connected";
  else if (state == FileTransferConnection_closed) state_name = "closed";
  else if (state == FileTransferConnection_failed) state_name = "failed";
  log_line(std::string("[beagle-sdk] filetransfer state ")
           + state_name
           + " peer=" + ctx->peer
           + " file=" + ctx->filename
           + " sender=" + (ctx->is_sender ? "1" : "0"));

  if (state == FileTransferConnection_connected && !ctx->is_sender && !ctx->fileid.empty()) {
    carrier_filetransfer_pull(ft, ctx->fileid.c_str(), 0);
  }
  if (ctx->is_sender) {
    if (state == FileTransferConnection_connected) {
      std::lock_guard<std::mutex> lock(ctx->connect_mu);
      ctx->connect_done = true;
      ctx->connect_ok = true;
      ctx->connect_cv.notify_all();
    } else if (state == FileTransferConnection_failed || state == FileTransferConnection_closed) {
      std::lock_guard<std::mutex> lock(ctx->connect_mu);
      if (!ctx->connect_done) {
        ctx->connect_done = true;
        ctx->connect_ok = false;
        ctx->connect_cv.notify_all();
      }
      std::string reason = (state == FileTransferConnection_failed) ? "state_failed" : "state_closed";
      mark_sender_transfer_result(ctx, false, reason);
    }
  }

  if (state == FileTransferConnection_closed || state == FileTransferConnection_failed) {
    auto done = take_transfer(ft);
    if (!done) return;
    if (done->source.is_open()) done->source.close();
    if (done->target.is_open()) done->target.close();
    carrier_filetransfer_close(ft);
  }
}

static void filetransfer_file_callback(CarrierFileTransfer* ft,
                                       const char* fileid,
                                       const char* filename,
                                       uint64_t size,
                                       void* context) {
  (void)context;
  auto ctx = get_transfer(ft);
  if (!ctx) return;
  if (fileid && *fileid) ctx->fileid = fileid;
  if (filename && *filename) ctx->filename = sanitize_filename(filename);
  if (size > 0) ctx->expected_size = size;
  if (ctx->media_type.empty()) ctx->media_type = infer_media_type_from_filename(ctx->filename);
  if (!ctx->is_sender && ctx->connected && !ctx->fileid.empty()) {
    carrier_filetransfer_pull(ft, ctx->fileid.c_str(), 0);
  }
}

static void filetransfer_pull_callback(CarrierFileTransfer* ft,
                                       const char* fileid,
                                       uint64_t offset,
                                       void* context) {
  (void)context;
  auto ctx = get_transfer(ft);
  if (!ctx || !ctx->is_sender) return;
  if (!fileid || ctx->fileid != fileid) return;

  if (!ctx->source.is_open()) {
    ctx->source.open(ctx->source_path, std::ios::binary);
    if (!ctx->source) {
      mark_sender_transfer_result(ctx, false, "open_source_failed");
      carrier_filetransfer_cancel(ft, ctx->fileid.c_str(), -1, "open source failed");
      return;
    }
  }
  ctx->source.clear();
  ctx->source.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!ctx->source.good()) {
    mark_sender_transfer_result(ctx, false, "seek_source_failed");
    carrier_filetransfer_cancel(ft, ctx->fileid.c_str(), -1, "seek source failed");
    return;
  }

  uint8_t buf[CARRIER_MAX_USER_DATA_LEN];
  while (ctx->source.good()) {
    ctx->source.read(reinterpret_cast<char*>(buf), sizeof(buf));
    std::streamsize got = ctx->source.gcount();
    if (got <= 0) break;
    size_t off = 0;
    size_t total = static_cast<size_t>(got);
    while (off < total) {
      size_t chunk = total - off;
      if (chunk > CARRIER_MAX_USER_DATA_LEN) chunk = CARRIER_MAX_USER_DATA_LEN;
      ssize_t sent = carrier_filetransfer_send(ft,
                                               ctx->fileid.c_str(),
                                               buf + off,
                                               chunk);
      if (sent < 0) {
        mark_sender_transfer_result(ctx, false, "send_chunk_failed");
        carrier_filetransfer_cancel(ft, ctx->fileid.c_str(), -1, "send chunk failed");
        return;
      }
      if (sent == 0) {
        mark_sender_transfer_result(ctx, false, "send_chunk_zero");
        carrier_filetransfer_cancel(ft, ctx->fileid.c_str(), -1, "send chunk returned zero");
        return;
      }
      off += static_cast<size_t>(sent);
      ctx->transferred += static_cast<uint64_t>(sent);
    }
  }

  ssize_t finish_rc = carrier_filetransfer_send(ft, ctx->fileid.c_str(), nullptr, 0);
  if (finish_rc < 0) {
    mark_sender_transfer_result(ctx, false, "send_finish_failed");
    carrier_filetransfer_cancel(ft, ctx->fileid.c_str(), -1, "send finish failed");
    return;
  }
  mark_sender_transfer_result(ctx, true, "send_complete");
}

static bool filetransfer_data_callback(CarrierFileTransfer* ft,
                                       const char* fileid,
                                       const uint8_t* data,
                                       size_t length,
                                       void* context) {
  (void)context;
  auto ctx = get_transfer(ft);
  if (!ctx || ctx->is_sender) return false;
  if (!fileid || ctx->fileid != fileid) return false;
  if (!ctx->target.is_open()) return false;

  if (length == 0) {
    ctx->target.flush();
    ctx->target.close();
    ctx->completed = true;
    emit_incoming_file_event(ctx);
    return false;
  }

  ctx->target.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(length));
  if (!ctx->target.good()) return false;
  ctx->transferred += static_cast<uint64_t>(length);
  return true;
}

static void filetransfer_pending_callback(CarrierFileTransfer* ft,
                                          const char* fileid,
                                          void* context) {
  (void)ft;
  (void)fileid;
  (void)context;
}

static void filetransfer_resume_callback(CarrierFileTransfer* ft,
                                         const char* fileid,
                                         void* context) {
  (void)ft;
  (void)fileid;
  (void)context;
}

static void filetransfer_cancel_callback(CarrierFileTransfer* ft,
                                         const char* fileid,
                                         int status,
                                         const char* reason,
                                         void* context) {
  (void)fileid;
  (void)context;
  std::ostringstream msg;
  msg << "[beagle-sdk] file transfer canceled status=" << status
      << " reason=" << (reason ? reason : "");
  log_line(msg.str());
  auto ctx = take_transfer(ft);
  if (!ctx) return;
  mark_sender_transfer_result(ctx, false, std::string("canceled:") + (reason ? reason : ""));
  if (ctx->source.is_open()) ctx->source.close();
  if (ctx->target.is_open()) ctx->target.close();
  carrier_filetransfer_close(ft);
}

static CarrierFileTransferCallbacks build_filetransfer_callbacks() {
  CarrierFileTransferCallbacks cbs;
  std::memset(&cbs, 0, sizeof(cbs));
  cbs.state_changed = filetransfer_state_changed_callback;
  cbs.file = filetransfer_file_callback;
  cbs.pull = filetransfer_pull_callback;
  cbs.data = filetransfer_data_callback;
  cbs.pending = filetransfer_pending_callback;
  cbs.resume = filetransfer_resume_callback;
  cbs.cancel = filetransfer_cancel_callback;
  return cbs;
}

static bool send_media_via_filetransfer(RuntimeState* state,
                                        const std::string& peer,
                                        const std::string& media_path,
                                        const std::string& send_filename,
                                        const std::string& send_media_type,
                                        uint64_t expected_size,
                                        int wait_connect_ms,
                                        int wait_transfer_ms,
                                        std::string& detail) {
  if (!state || !state->carrier) {
    detail = "carrier_not_ready";
    return false;
  }
  if (peer.empty() || media_path.empty() || send_filename.empty()) {
    detail = "invalid_args";
    return false;
  }

  auto ctx = std::make_shared<TransferContext>();
  ctx->state = state;
  ctx->is_sender = true;
  ctx->peer = peer;
  ctx->source_path = media_path;
  ctx->filename = send_filename;
  ctx->media_type = send_media_type;
  ctx->expected_size = expected_size;

  CarrierFileTransferInfo info;
  std::memset(&info, 0, sizeof(info));
  std::snprintf(info.filename, sizeof(info.filename), "%s", send_filename.c_str());
  info.size = expected_size;
  if (!carrier_filetransfer_fileid(info.fileid, sizeof(info.fileid))) {
    detail = "fileid_generate_failed";
    return false;
  }
  ctx->fileid = info.fileid;

  CarrierFileTransferCallbacks cbs = build_filetransfer_callbacks();
  ctx->ft = carrier_filetransfer_new(state->carrier, peer.c_str(), &info, &cbs, ctx.get());
  if (!ctx->ft) {
    std::ostringstream oss;
    oss << "new_failed:0x" << std::hex << carrier_get_error() << std::dec;
    detail = oss.str();
    return false;
  }
  register_transfer(ctx);

  if (carrier_filetransfer_connect(ctx->ft) < 0) {
    std::ostringstream oss;
    oss << "connect_failed:0x" << std::hex << carrier_get_error() << std::dec;
    detail = oss.str();
    take_transfer(ctx->ft);
    carrier_filetransfer_close(ctx->ft);
    return false;
  }

  std::unique_lock<std::mutex> lock(ctx->connect_mu);
  bool done = ctx->connect_cv.wait_for(lock,
                                       std::chrono::milliseconds(wait_connect_ms),
                                       [&]() { return ctx->connect_done; });
  if (!done) {
    detail = "connect_timeout";
    return false;
  }
  if (!ctx->connect_ok) {
    detail = "connect_not_ok";
    return false;
  }
  std::unique_lock<std::mutex> transfer_lock(ctx->transfer_mu);
  bool transfer_done = ctx->transfer_cv.wait_for(transfer_lock,
                                                 std::chrono::milliseconds(wait_transfer_ms),
                                                 [&]() { return ctx->transfer_done; });
  if (!transfer_done) {
    detail = "send_timeout";
    return false;
  }
  if (!ctx->transfer_ok) {
    detail = ctx->transfer_detail.empty() ? "send_not_ok" : ctx->transfer_detail;
    return false;
  }
  detail = ctx->transfer_detail.empty() ? "send_complete" : ctx->transfer_detail;
  return true;
}

static void filetransfer_connect_callback(Carrier* carrier,
                                          const char* address,
                                          const CarrierFileTransferInfo* fileinfo,
                                          void* context) {
  auto* state = static_cast<RuntimeState*>(context);
  if (!state || !carrier || !address || !fileinfo) return;

  auto ctx = std::make_shared<TransferContext>();
  ctx->state = state;
  ctx->is_sender = false;
  ctx->peer = address;
  ctx->fileid = fileinfo->fileid;
  ctx->filename = sanitize_filename(fileinfo->filename);
  ctx->expected_size = fileinfo->size;
  ctx->media_type = infer_media_type_from_filename(ctx->filename);

  ensure_dir(state->media_dir);
  std::ostringstream path;
  path << state->media_dir << "/" << std::time(nullptr) << "_" << ctx->filename;
  ctx->target_path = path.str();
  ctx->target.open(ctx->target_path, std::ios::binary | std::ios::trunc);
  if (!ctx->target) {
    log_line(std::string("[beagle-sdk] failed to open target file: ") + ctx->target_path);
    return;
  }

  CarrierFileTransferCallbacks cbs = build_filetransfer_callbacks();
  ctx->ft = carrier_filetransfer_new(carrier, address, fileinfo, &cbs, ctx.get());
  if (!ctx->ft) {
    std::ostringstream msg;
    msg << "[beagle-sdk] carrier_filetransfer_new(receiver) failed: 0x" << std::hex
        << carrier_get_error() << std::dec;
    log_line(msg.str());
    ctx->target.close();
    return;
  }
  register_transfer(ctx);
  if (carrier_filetransfer_accept_connect(ctx->ft) < 0) {
    std::ostringstream msg;
    msg << "[beagle-sdk] carrier_filetransfer_accept_connect failed: 0x" << std::hex
        << carrier_get_error() << std::dec;
    log_line(msg.str());
    take_transfer(ctx->ft);
    carrier_filetransfer_close(ctx->ft);
    ctx->target.close();
  }
}

bool friend_list_callback(const CarrierFriendInfo* info, void* context);

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
  PackedFilePayload file_payload;
  if (decode_beaglechat_file_payload(msg, len, file_payload)) {
    {
      std::lock_guard<std::mutex> lock(state->state_mu);
      state->peer_prefers_inline_media[incoming.peer] = false;
      state->peer_media_payload_hint[incoming.peer] = "packed";
    }
    if (file_payload.bytes_len > kMaxBeaglechatFileBytes) {
      incoming.filename = file_payload.filename;
      incoming.media_type = file_payload.content_type;
      incoming.size = static_cast<unsigned long long>(file_payload.bytes_len);
      incoming.text = "[file rejected: exceeds 5MB beaglechat payload limit]";
      log_line(std::string("[beagle-sdk] rejected incoming beaglechat file from ")
               + incoming.peer + " file=" + incoming.filename
               + " size=" + std::to_string(file_payload.bytes_len));
    } else {
    ensure_dir(state->media_dir);
    std::ostringstream path;
    path << state->media_dir << "/" << std::time(nullptr) << "_" << file_payload.filename;
    std::ofstream out(path.str(), std::ios::binary | std::ios::trunc);
    if (out) {
      out.write(reinterpret_cast<const char*>(file_payload.bytes),
                static_cast<std::streamsize>(file_payload.bytes_len));
      out.close();
      incoming.media_path = path.str();
      incoming.filename = file_payload.filename;
      incoming.media_type = file_payload.content_type;
      incoming.size = static_cast<unsigned long long>(file_payload.bytes_len);
      incoming.text.clear();
      log_line(std::string("[beagle-sdk] received beaglechat file from ")
               + incoming.peer + " file=" + incoming.filename
               + " size=" + std::to_string(file_payload.bytes_len));
    } else {
      log_line(std::string("[beagle-sdk] failed to persist incoming file from ")
               + incoming.peer + " file=" + file_payload.filename);
      incoming.text.assign(static_cast<const char*>(msg), len);
    }
    }
  } else {
    InlineJsonMedia inline_media;
    if (decode_inline_json_media_payload(msg, len, inline_media)) {
      {
        std::lock_guard<std::mutex> lock(state->state_mu);
        state->peer_prefers_inline_media[incoming.peer] = true;
        state->peer_media_payload_hint[incoming.peer] =
            is_swift_filemodel_json_payload(msg, len) ? "swift-json" : "inline-json";
      }
      ensure_dir(state->media_dir);
      std::ostringstream path;
      path << state->media_dir << "/" << std::time(nullptr) << "_" << inline_media.filename;
      std::ofstream out(path.str(), std::ios::binary | std::ios::trunc);
      if (out) {
        out.write(reinterpret_cast<const char*>(inline_media.bytes.data()),
                  static_cast<std::streamsize>(inline_media.bytes.size()));
        out.close();
        incoming.media_path = path.str();
        incoming.filename = inline_media.filename;
        incoming.media_type = inline_media.media_type;
        incoming.size = static_cast<unsigned long long>(inline_media.bytes.size());
        incoming.text.clear();
        log_line(std::string("[beagle-sdk] received inline json media from ")
                 + incoming.peer + " file=" + incoming.filename
                 + " size=" + std::to_string(inline_media.bytes.size()));
      } else {
        log_line(std::string("[beagle-sdk] failed to persist inline json media from ")
                 + incoming.peer + " file=" + inline_media.filename);
        incoming.text.assign(static_cast<const char*>(msg), len);
      }
    } else {
      if (len > 1024) {
        log_line(std::string("[beagle-sdk] inline json media decode miss peer=") + incoming.peer
                 + " len=" + std::to_string(len));
      }
      incoming.text.assign(static_cast<const char*>(msg), len);
    }
  }
  incoming.ts = timestamp;
  std::string signature = build_incoming_signature(incoming, offline);
  // Carrier sometimes replays very old offline messages from Express.
  // Drop stale offline payloads immediately to avoid re-triggering agents.
  if (offline && state->startup_ts_us > 0 && incoming.ts > 0) {
    constexpr int64_t kOfflineStaleWindowUs = 5LL * 60LL * 1000LL * 1000LL;
    if (incoming.ts < (state->startup_ts_us - kOfflineStaleWindowUs)) {
      log_incoming_event(state, incoming, offline, "dropped_stale_offline", signature);
      std::ostringstream msg;
      msg << "[beagle-sdk] dropped stale offline message"
          << " peer=" << incoming.peer
          << " ts=" << incoming.ts
          << " startup_ts=" << state->startup_ts_us;
      if (!incoming.media_path.empty()) {
        msg << " kind=file"
            << " filename=" << incoming.filename
            << " media_type=" << incoming.media_type
            << " size=" << incoming.size;
      } else {
        msg << " kind=text"
            << " text=\"" << preview_text(incoming.text) << "\"";
      }
      log_line(msg.str());
      return;
    }
  }
  if (!remember_incoming_signature(state, signature)) {
    log_incoming_event(state, incoming, offline, "skipped_replay", signature);
    std::ostringstream msg;
    msg << "[beagle-sdk] skipped replayed incoming message"
        << " peer=" << incoming.peer
        << " mode=" << (offline ? "offline" : "online")
        << " ts=" << incoming.ts;
    if (!incoming.media_path.empty()) {
      msg << " kind=file"
          << " filename=" << incoming.filename
          << " media_type=" << incoming.media_type
          << " size=" << incoming.size;
    } else {
      msg << " kind=text"
          << " text=\"" << preview_text(incoming.text) << "\"";
    }
    msg << " signature=\"" << preview_text(signature, 160) << "\"";
    log_line(msg.str());
    return;
  }
  log_incoming_event(state, incoming, offline, "forwarded", signature);
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

  std::ostringstream line;
  line << "[beagle-sdk] message (" << (offline ? "offline" : "online")
       << ") from " << incoming.peer;
  if (!incoming.media_path.empty()) {
    line << " [file] " << incoming.filename << " (" << incoming.size << " bytes)";
  } else {
    line << ": " << incoming.text;
  }
  log_line(line.str());
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
    std::ostringstream msg;
    msg << "[beagle-sdk] accept friend failed: 0x" << std::hex << carrier_get_error() << std::dec;
    log_line(msg.str());
  } else {
    log_line(std::string("[beagle-sdk] accepted friend: ") + userid);
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
  log_line(std::string("[beagle-sdk] connection status: ")
           + (status == CarrierConnectionStatus_Connected ? "connected" : "disconnected"));
}

void ready_callback(Carrier* carrier, void* context) {
  (void)carrier;
  auto* state = static_cast<RuntimeState*>(context);
  if (state) {
    std::lock_guard<std::mutex> lock(state->state_mu);
    state->status.ready = true;
  }
  log_line("[beagle-sdk] ready");
  if (carrier && state) {
    int rc = carrier_get_friends(carrier, friend_list_callback, state);
    if (rc < 0) {
      std::ostringstream msg;
      msg << "[beagle-sdk] carrier_get_friends failed: 0x" << std::hex << carrier_get_error() << std::dec;
      log_line(msg.str());
    } else {
      log_line("[beagle-sdk] carrier_get_friends ok");
    }
  }
}

void friend_connection_callback(Carrier* carrier,
                                const char* friendid,
                                CarrierConnectionStatus status,
                                void* context) {
  (void)carrier;
  auto* state = static_cast<RuntimeState*>(context);
  log_line(std::string("[beagle-sdk] friend ") + (friendid ? friendid : "")
           + " is " + (status == CarrierConnectionStatus_Connected ? "online" : "offline"));
  if (status == CarrierConnectionStatus_Connected && friendid) {
    send_welcome_once(state, friendid, "online");
  }
  if (friendid) {
    update_friend_status(state,
                         friendid,
                         status == CarrierConnectionStatus_Connected ? 1 : 0,
                         -1,
                         true);
  }
}

void friend_info_callback(Carrier* carrier,
                          const char* friendid,
                          const CarrierFriendInfo* info,
                          void* context) {
  (void)carrier;
  auto* state = static_cast<RuntimeState*>(context);
  if (!state || !friendid || !info) return;
  log_line(std::string("[beagle-sdk] friend info update for ") + friendid);
  store_friend_info(state, friendid, info);
}

void friend_added_callback(Carrier* carrier,
                           const CarrierFriendInfo* info,
                           void* context) {
  (void)carrier;
  auto* state = static_cast<RuntimeState*>(context);
  if (!state || !info) return;
  log_line(std::string("[beagle-sdk] friend added ") + info->user_info.userid);
  store_friend_info(state, info->user_info.userid, info);
}

void friend_presence_callback(Carrier* carrier,
                              const char* friendid,
                              CarrierPresenceStatus presence,
                              void* context) {
  (void)carrier;
  auto* state = static_cast<RuntimeState*>(context);
  if (!state || !friendid) return;
  update_friend_status(state, friendid, -1, static_cast<int>(presence), false);
}

bool friend_list_callback(const CarrierFriendInfo* info, void* context) {
  auto* state = static_cast<RuntimeState*>(context);
  if (!state || !info) return false;
  const char* fid = info->user_info.userid;
  if (!fid || !*fid) return true;
  log_line(std::string("[beagle-sdk] friend list item ") + fid);
  store_friend_info(state, fid, info);
  return true;
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
  log_line(std::string("[beagle-sdk] invite from ") + (from ? from : "") + " data=" + payload);

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
    log_line("[beagle-sdk] missing config file path");
    return false;
  }

  CarrierOptions opts;
  if (!carrier_config_load(options.config_path.c_str(), nullptr, &opts)) {
    log_line(std::string("[beagle-sdk] failed to load config: ") + options.config_path);
    return false;
  }

  if (!options.data_dir.empty()) {
    g_state.persistent_location = options.data_dir;
    ensure_dir(g_state.persistent_location);
    // carrier_config_free() will free this field, so allocate with strdup.
    opts.persistent_location = strdup(g_state.persistent_location.c_str());
    g_state.profile_path = g_state.persistent_location + "/beagle_profile.json";
    g_state.welcome_state_path = g_state.persistent_location + "/welcomed_peers.txt";
    g_state.db_config_path = g_state.persistent_location + "/beagle_db.json";
    g_state.friend_state_path = g_state.persistent_location + "/friend_state.tsv";
    g_state.friend_event_log_path = g_state.persistent_location + "/friend_events.log";
    g_state.incoming_event_log_path = g_state.persistent_location + "/incoming_events.jsonl";
    g_state.media_dir = g_state.persistent_location + "/media";
    ensure_dir(g_state.media_dir);
  } else {
    g_state.media_dir = "./media";
    g_state.incoming_event_log_path = "./incoming_events.jsonl";
    ensure_dir(g_state.media_dir);
  }

  CarrierCallbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.connection_status = connection_status_callback;
  callbacks.ready = ready_callback;
  callbacks.self_info = nullptr;
  callbacks.friend_connection = friend_connection_callback;
  callbacks.friend_info = friend_info_callback;
  callbacks.friend_presence = friend_presence_callback;
  callbacks.friend_message = friend_message_callback;
  callbacks.friend_request = friend_request_callback;
  callbacks.friend_added = friend_added_callback;
  callbacks.friend_invite = friend_invite_callback;

  g_state.on_incoming = std::move(on_incoming);
  g_state.startup_ts_us = static_cast<int64_t>(std::time(nullptr)) * 1000000LL;

  Carrier* carrier = carrier_new(&opts, &callbacks, &g_state);
  carrier_config_free(&opts);
  if (!carrier) {
    std::ostringstream msg;
    msg << "[beagle-sdk] carrier_new failed: 0x" << std::hex << carrier_get_error() << std::dec;
    log_line(msg.str());
    return false;
  }

  g_state.carrier = carrier;

  if (carrier_filetransfer_init(g_state.carrier, filetransfer_connect_callback, &g_state) < 0) {
    std::ostringstream msg;
    msg << "[beagle-sdk] carrier_filetransfer_init failed: 0x" << std::hex
        << carrier_get_error() << std::dec;
    log_line(msg.str());
  }

  char buf[CARRIER_MAX_ADDRESS_LEN + 1] = {0};
  char idbuf[CARRIER_MAX_ID_LEN + 1] = {0};
  carrier_get_userid(carrier, idbuf, sizeof(idbuf));
  carrier_get_address(carrier, buf, sizeof(buf));
  g_state.user_id = idbuf;
  g_state.address = buf;
  user_id_ = g_state.user_id;
  address_ = g_state.address;

  log_line(std::string("[beagle-sdk] User ID: ") + user_id_);
  log_line(std::string("[beagle-sdk] Address: ") + address_);

  ensure_profile_metadata(&g_state, g_state.user_id, g_state.address);

  ProfileInfo profile;
  load_profile(&g_state, profile);
  load_welcomed_peers(&g_state);
  apply_profile(&g_state, profile);

  load_db_config(&g_state, g_state.db);
  ensure_db(&g_state, g_state.db);
  if (g_state.db.use_crawler_index) {
    refresh_crawler_index_if_needed(&g_state);
  }
  load_friend_state(&g_state);

  g_state.loop_thread = std::thread([]() {
    int rc = carrier_run(g_state.carrier, 10);
    if (rc != 0) {
      std::ostringstream msg;
      msg << "[beagle-sdk] carrier_run failed: 0x" << std::hex << carrier_get_error() << std::dec;
      log_line(msg.str());
    }
  });

  return true;
}

void BeagleSdk::stop() {
  if (!g_state.carrier) return;
  carrier_filetransfer_cleanup(g_state.carrier);
  carrier_kill(g_state.carrier);
  if (g_state.loop_thread.joinable()) g_state.loop_thread.join();
  {
    std::lock_guard<std::mutex> lock(g_ft_mu);
    for (auto& it : g_transfers) {
      if (it.second && it.second->source.is_open()) it.second->source.close();
      if (it.second && it.second->target.is_open()) it.second->target.close();
      if (it.first) carrier_filetransfer_close(it.first);
    }
    g_transfers.clear();
  }
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
    int err = carrier_get_error();
    std::string detail;
    if (post_payload_to_express_node(&g_state, peer, text.data(), text.size(), detail)) {
      log_line(std::string("[beagle-sdk] send_text express fallback ok peer=")
               + peer + " bytes=" + std::to_string(text.size())
               + " detail=" + detail
               + " carrier_err=0x" + [&]() {
                   std::ostringstream oss;
                   oss << std::hex << err;
                   return oss.str();
                 }());
      return true;
    }
    log_line(std::string("[beagle-sdk] send_text express fallback failed peer=")
             + peer + " detail=" + detail);
    std::ostringstream msg;
    msg << "[beagle-sdk] send_text failed: 0x" << std::hex << err << std::dec;
    log_line(msg.str());
    return false;
  }
  log_line(std::string("[beagle-sdk] send_text ok msgid=") + std::to_string(msgid) + " peer=" + peer);
  return true;
}

bool BeagleSdk::send_media(const std::string& peer,
                           const std::string& caption,
                           const std::string& media_path,
                           const std::string& media_url,
                           const std::string& media_type,
                           const std::string& filename,
                           const std::string& out_format) {
  if (!g_state.carrier) return false;

  if (media_path.empty()) {
    std::string payload;
    if (!caption.empty()) payload += caption;
    if (!media_url.empty()) {
      if (!payload.empty()) payload += "\n";
      payload += media_url;
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

  unsigned long long size = file_size_bytes(media_path);
  if (size == 0) {
    log_line(std::string("[beagle-sdk] send_media invalid file path: ") + media_path);
    return false;
  }
  std::string send_filename = sanitize_filename(!filename.empty() ? filename : basename_of(media_path));
  if (send_filename.empty()) send_filename = "file.bin";
  std::string send_media_type = !media_type.empty() ? media_type : infer_media_type_from_filename(send_filename);
  uint64_t expected_size = static_cast<uint64_t>(size);

  std::vector<unsigned char> file_bytes;
  if (!read_file_binary(media_path, file_bytes)) {
    log_line(std::string("[beagle-sdk] send_media failed to read file: ") + media_path);
    return false;
  }
  if (file_bytes.size() > kMaxBeaglechatFileBytes) {
    log_line(std::string("[beagle-sdk] send_media file too large for beaglechat payload: ")
             + media_path + " size=" + std::to_string(file_bytes.size())
             + " max=" + std::to_string(kMaxBeaglechatFileBytes));
    return false;
  }

  std::string out_mode = lowercase(trim_copy(out_format));
  if (out_mode.empty()) {
    const char* mode_env = std::getenv("BEAGLE_MEDIA_OUT_FORMAT");
    out_mode = lowercase(trim_copy(mode_env ? mode_env : ""));
  }
  if (out_mode.empty()) out_mode = "auto";
  if (!out_mode.empty() &&
      out_mode != "auto" &&
      out_mode != "filetransfer" &&
      out_mode != "packed" &&
      out_mode != "swift-json" &&
      out_mode != "inline-json" &&
      out_mode != "legacy-inline") {
    out_mode = "auto";
  }
  bool force_filetransfer = (out_mode == "filetransfer");
  bool try_filetransfer_first = (out_mode == "auto" || force_filetransfer);
  bool use_packed = (out_mode == "packed" || out_mode == "auto");
  bool use_swift_json = (out_mode == "swift-json");
  bool use_legacy_inline = (out_mode == "legacy-inline");
  bool prefer_message_media_path = false;
  log_line(std::string("[beagle-sdk] send_media mode peer=") + peer
           + " out_mode=" + out_mode
           + " force_filetransfer=" + (force_filetransfer ? "1" : "0")
           + " try_filetransfer_first=" + (try_filetransfer_first ? "1" : "0")
           + " use_packed=" + (use_packed ? "1" : "0")
           + " use_swift_json=" + (use_swift_json ? "1" : "0")
           + " use_legacy_inline=" + (use_legacy_inline ? "1" : "0"));
  const char* legacy_peers_env = std::getenv("BEAGLE_MEDIA_LEGACY_INLINE_PEERS");
  if (legacy_peers_env && csv_has_token(legacy_peers_env, peer)) {
    use_legacy_inline = true;
    use_packed = false;
    use_swift_json = false;
  }
  const char* swift_peers_env = std::getenv("BEAGLE_MEDIA_SWIFT_JSON_PEERS");
  if (swift_peers_env && csv_has_token(swift_peers_env, peer)) {
    use_packed = false;
    use_swift_json = true;
    use_legacy_inline = false;
  }
  const char* inline_peers_env = std::getenv("BEAGLE_MEDIA_INLINE_PEERS");
  if (inline_peers_env && csv_has_token(inline_peers_env, peer)) {
    use_packed = false;
    use_swift_json = false;
    use_legacy_inline = false;
  }
  if (out_mode == "auto") {
    std::lock_guard<std::mutex> lock(g_state.state_mu);
    auto it = g_state.peer_media_payload_hint.find(peer);
    if (it != g_state.peer_media_payload_hint.end()) {
      if (it->second == "swift-json") {
        use_packed = false;
        use_swift_json = true;
        use_legacy_inline = false;
        prefer_message_media_path = true;
      } else if (it->second == "inline-json") {
        use_packed = false;
        use_swift_json = false;
        use_legacy_inline = false;
        prefer_message_media_path = true;
      } else if (it->second == "packed") {
        use_packed = true;
        use_swift_json = false;
        use_legacy_inline = false;
      }
    }
  }

  if (out_mode == "auto" && prefer_message_media_path && !force_filetransfer) {
    try_filetransfer_first = false;
    log_line(std::string("[beagle-sdk] send_media auto prefers message media payload for peer=") + peer);
  }

  bool peer_online_known = false;
  bool peer_online = is_friend_online(&g_state, peer, peer_online_known);
  if (try_filetransfer_first && peer_online_known && !peer_online) {
    if (force_filetransfer) {
      // In force mode, do not trust cached friend presence enough to abort.
      // Filetransfer connect can still succeed when presence state is stale.
      log_line(std::string("[beagle-sdk] send_media filetransfer-only mode with offline cache; still trying: ") + peer);
    } else {
      try_filetransfer_first = false;
      log_line(std::string("[beagle-sdk] send_media skipping filetransfer because peer offline: ") + peer);
    }
  }

  if (try_filetransfer_first) {
    log_line(std::string("[beagle-sdk] send_media trying filetransfer peer=") + peer
             + " file=" + send_filename);
    std::string ft_detail;
    int wait_ms = 8000;
    const char* wait_env = std::getenv("BEAGLE_FILETRANSFER_WAIT_MS");
    if (wait_env && *wait_env) {
      int v = std::atoi(wait_env);
      if (v >= 1000 && v <= 60000) wait_ms = v;
    }
    int wait_send_ms = 15000;
    const char* wait_send_env = std::getenv("BEAGLE_FILETRANSFER_SEND_WAIT_MS");
    if (wait_send_env && *wait_send_env) {
      int v = std::atoi(wait_send_env);
      if (v >= 1000 && v <= 120000) wait_send_ms = v;
    }
    if (send_media_via_filetransfer(&g_state,
                                    peer,
                                    media_path,
                                    send_filename,
                                    send_media_type,
                                    expected_size,
                                    wait_ms,
                                    wait_send_ms,
                                    ft_detail)) {
      log_line(std::string("[beagle-sdk] send_media(filetransfer) ok peer=")
               + peer + " file=" + send_filename
               + " size=" + std::to_string(size)
               + " type=" + send_media_type
               + " detail=" + ft_detail);
      return true;
    }
    log_line(std::string("[beagle-sdk] send_media(filetransfer) failed peer=")
             + peer + " file=" + send_filename
             + " detail=" + ft_detail);
    if (force_filetransfer) return false;
  }

  std::vector<unsigned char> payload_packed;
  std::string payload_inline;
  const void* payload_ptr = nullptr;
  size_t payload_len = 0;
  std::string payload_mode = "inline-json";

  if (use_packed) {
    if (!encode_beaglechat_file_payload(send_filename, send_media_type, file_bytes, payload_packed)) {
      log_line("[beagle-sdk] send_media failed to pack beaglechat payload");
      return false;
    }
    payload_ptr = payload_packed.data();
    payload_len = payload_packed.size();
    payload_mode = "packed";
  } else {
    if (use_legacy_inline) {
      if (!encode_legacy_inline_data_payload(file_bytes, payload_inline)) {
        log_line("[beagle-sdk] send_media failed to encode legacy inline data payload");
        return false;
      }
      payload_mode = "legacy-inline";
    } else if (use_swift_json) {
      if (!encode_swift_filemodel_media_payload(send_filename, send_media_type, file_bytes, payload_inline)) {
        log_line("[beagle-sdk] send_media failed to encode swift filemodel payload");
        return false;
      }
      payload_mode = "swift-json";
    } else {
      if (!encode_inline_json_media_payload(send_filename, send_media_type, file_bytes, payload_inline)) {
        log_line("[beagle-sdk] send_media failed to encode inline json media payload");
        return false;
      }
      payload_mode = "inline-json";
    }
    payload_ptr = payload_inline.data();
    payload_len = payload_inline.size();
  }

  uint32_t msgid = 0;
  int rc = carrier_send_friend_message(g_state.carrier,
                                       peer.c_str(),
                                       payload_ptr,
                                       payload_len,
                                       &msgid,
                                       nullptr,
                                       nullptr);
  if (rc < 0) {
    int err = carrier_get_error();
    std::string detail;
    if (post_payload_to_express_node(&g_state, peer, payload_ptr, payload_len, detail)) {
      log_line(std::string("[beagle-sdk] send_media(") + payload_mode
               + ") express fallback ok peer="
               + peer + " file=" + send_filename
               + " bytes=" + std::to_string(payload_len)
               + " detail=" + detail
               + " carrier_err=0x" + [&]() {
                   std::ostringstream oss;
                   oss << std::hex << err;
                   return oss.str();
                 }());
      return true;
    }
    log_line(std::string("[beagle-sdk] send_media(") + payload_mode
             + ") express fallback failed peer="
             + peer + " file=" + send_filename + " detail=" + detail);
    std::ostringstream msg;
    msg << "[beagle-sdk] send_media(" << payload_mode
        << ") failed: 0x" << std::hex
        << err << std::dec;
    log_line(msg.str());
    return false;
  }
  log_line(std::string("[beagle-sdk] send_media(") + payload_mode
           + ") ok msgid="
           + std::to_string(msgid)
           + " peer=" + peer
           + " file=" + send_filename
           + " size=" + std::to_string(size)
           + " type=" + send_media_type);
  return true;
}

#if !BEAGLE_SDK_STUB
BeagleStatus BeagleSdk::status() const {
  std::lock_guard<std::mutex> lock(g_state.state_mu);
  return g_state.status;
}
#endif

#endif
