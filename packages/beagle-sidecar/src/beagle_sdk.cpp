#include "beagle_sdk.h"

#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>
#include <vector>

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
}

namespace {
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
  std::string user_id;
  std::string address;
  BeagleStatus status;
  std::unordered_set<std::string> welcomed_peers;
  std::map<std::string, FriendState> friend_state;
  DbConfig db;
};

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

static void ensure_profile_file(RuntimeState* state);

static bool append_line(const std::string& path, const std::string& line) {
  std::ofstream out(path, std::ios::app);
  if (!out) return false;
  out << line << "\n";
  return static_cast<bool>(out);
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
      + "  \"database\": \"beagle\"\n"
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
      "ts DATETIME"
      ");";
  int rc = mysql_exec(db, schema);
  if (rc != 0) {
    log_line(std::string("[beagle-sdk] mysql schema init failed rc=") + std::to_string(rc));
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
  if (!state->friend_event_log_path.empty()) {
    std::ostringstream line;
    line << ts << "\t" << friendid << "\t" << event_type
         << "\tstatus=" << status << "\tpresence=" << presence;
    append_line(state->friend_event_log_path, line.str());
  }
  if (state->db.enabled) {
    std::ostringstream sql;
    sql << "INSERT INTO beagle_friend_events(friendid,event_type,status,presence,ts) VALUES('"
        << sql_escape(friendid) << "','"
        << sql_escape(event_type) << "',"
        << status << ","
        << presence << ",'"
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
  if (!changed && !presence_changed) return;
  it->second.status = next_status;
  it->second.presence = next_presence;
  save_friend_state(state);
  if (log_event && changed) log_friend_event(state, friendid, next_status ? "online" : "offline", next_status, next_presence);
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
      append_line(state->welcome_state_path, peer);
    }
    log_line(std::string("[beagle-sdk] welcome message sent (") + reason + ") to " + peer);
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

  std::ostringstream line;
  line << "[beagle-sdk] message (" << (offline ? "offline" : "online")
       << ") from " << incoming.peer << ": " << incoming.text;
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

  Carrier* carrier = carrier_new(&opts, &callbacks, &g_state);
  carrier_config_free(&opts);
  if (!carrier) {
    std::ostringstream msg;
    msg << "[beagle-sdk] carrier_new failed: 0x" << std::hex << carrier_get_error() << std::dec;
    log_line(msg.str());
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

  log_line(std::string("[beagle-sdk] User ID: ") + user_id_);
  log_line(std::string("[beagle-sdk] Address: ") + address_);

  ensure_profile_metadata(&g_state, g_state.user_id, g_state.address);

  ProfileInfo profile;
  load_profile(&g_state, profile);
  load_welcomed_peers(&g_state);
  apply_profile(&g_state, profile);

  load_db_config(&g_state, g_state.db);
  ensure_db(&g_state, g_state.db);
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
    std::ostringstream msg;
    msg << "[beagle-sdk] send_text failed: 0x" << std::hex << carrier_get_error() << std::dec;
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

#if !BEAGLE_SDK_STUB
BeagleStatus BeagleSdk::status() const {
  std::lock_guard<std::mutex> lock(g_state.state_mu);
  return g_state.status;
}
#endif

#endif
