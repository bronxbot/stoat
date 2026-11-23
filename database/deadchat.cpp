#include "database.h"
#include <array>
#include <cstdlib>

namespace bronx {

bool Database::save_dead_chat_config(const DeadChatConfig &cfg) {
  if (cfg.server_id.empty() || cfg.channel_id.empty())
    return false;
  std::lock_guard<std::mutex> lock(db_mutex_);
  if (!mysql_)
    return false;

  std::string notify_ch = cfg.notify_channel_id.empty() ? cfg.channel_id : cfg.notify_channel_id;
  std::string ping_role = cfg.ping_role_id.empty() ? "NULL" : ("'" + escape_string(cfg.ping_role_id) + "'");
  std::string custom_msg = cfg.custom_message.empty() ? "NULL" : ("'" + escape_string(cfg.custom_message) + "'");

  std::string q =
      "INSERT INTO dead_chat_configs (server_id, channel_id, notify_channel_id, "
      "ping_role_id, enabled, p_threshold, min_speakers, cooldown_sec, custom_message) VALUES ('" +
      escape_string(cfg.server_id) + "', '" + escape_string(cfg.channel_id) + "', '" +
      escape_string(notify_ch) + "', " + ping_role + ", " + (cfg.enabled ? "1" : "0") + ", " +
      std::to_string(cfg.p_threshold) + ", " + std::to_string(cfg.min_speakers) + ", " +
      std::to_string(cfg.cooldown_sec) + ", " + custom_msg +
      ") ON DUPLICATE KEY UPDATE "
      "notify_channel_id=VALUES(notify_channel_id), ping_role_id=VALUES(ping_role_id), "
      "enabled=VALUES(enabled), p_threshold=VALUES(p_threshold), "
      "min_speakers=VALUES(min_speakers), cooldown_sec=VALUES(cooldown_sec), "
      "custom_message=VALUES(custom_message)";
  return mysql_query(mysql_, q.c_str()) == 0;
}

std::optional<DeadChatConfig> Database::get_dead_chat_config(const std::string &server_id,
                                                             const std::string &channel_id) {
  std::lock_guard<std::mutex> lock(db_mutex_);
  if (!mysql_)
    return std::nullopt;
  std::string q =
      "SELECT server_id, channel_id, notify_channel_id, ping_role_id, enabled, "
      "p_threshold, min_speakers, cooldown_sec, custom_message FROM dead_chat_configs "
      "WHERE server_id = '" + escape_string(server_id) + "' AND channel_id = '" +
      escape_string(channel_id) + "' LIMIT 1";
  if (mysql_query(mysql_, q.c_str()) != 0)
    return std::nullopt;
  MYSQL_RES *res = mysql_store_result(mysql_);
  if (!res)
    return std::nullopt;
  MYSQL_ROW row = mysql_fetch_row(res);
  std::optional<DeadChatConfig> out;
  if (row) {
    DeadChatConfig c;
    c.server_id = row[0] ? row[0] : "";
    c.channel_id = row[1] ? row[1] : "";
    c.notify_channel_id = row[2] ? row[2] : c.channel_id;
    c.ping_role_id = row[3] ? row[3] : "";
    c.enabled = row[4] && std::stoi(row[4]) != 0;
    c.p_threshold = row[5] ? std::stod(row[5]) : 0.02;
    c.min_speakers = row[6] ? std::stoi(row[6]) : 2;
    c.cooldown_sec = row[7] ? std::stoi(row[7]) : 3600;
    c.custom_message = row[8] ? row[8] : "";
    out = c;
  }
  mysql_free_result(res);
  return out;
}

std::vector<DeadChatConfig> Database::get_all_dead_chat_configs() {
  std::vector<DeadChatConfig> out;
  std::lock_guard<std::mutex> lock(db_mutex_);
  if (!mysql_)
    return out;
  std::string q =
      "SELECT server_id, channel_id, notify_channel_id, ping_role_id, enabled, "
      "p_threshold, min_speakers, cooldown_sec, custom_message FROM dead_chat_configs WHERE enabled = 1";
  if (mysql_query(mysql_, q.c_str()) != 0)
    return out;
  MYSQL_RES *res = mysql_store_result(mysql_);
  if (!res)
    return out;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    DeadChatConfig c;
    c.server_id = row[0] ? row[0] : "";
    c.channel_id = row[1] ? row[1] : "";
    c.notify_channel_id = row[2] ? row[2] : c.channel_id;
    c.ping_role_id = row[3] ? row[3] : "";
    c.enabled = row[4] && std::stoi(row[4]) != 0;
    c.p_threshold = row[5] ? std::stod(row[5]) : 0.02;
    c.min_speakers = row[6] ? std::stoi(row[6]) : 2;
    c.cooldown_sec = row[7] ? std::stoi(row[7]) : 3600;
    c.custom_message = row[8] ? row[8] : "";
    out.push_back(std::move(c));
  }
  mysql_free_result(res);
  return out;
}

std::vector<DeadChatConfig> Database::get_server_dead_chat_configs(const std::string &server_id) {
  std::vector<DeadChatConfig> out;
  std::lock_guard<std::mutex> lock(db_mutex_);
  if (!mysql_)
    return out;
  std::string q =
      "SELECT server_id, channel_id, notify_channel_id, ping_role_id, enabled, "
      "p_threshold, min_speakers, cooldown_sec, custom_message FROM dead_chat_configs "
      "WHERE server_id = '" + escape_string(server_id) + "'";
  if (mysql_query(mysql_, q.c_str()) != 0)
    return out;
  MYSQL_RES *res = mysql_store_result(mysql_);
  if (!res)
    return out;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    DeadChatConfig c;
    c.server_id = row[0] ? row[0] : "";
    c.channel_id = row[1] ? row[1] : "";
    c.notify_channel_id = row[2] ? row[2] : c.channel_id;
    c.ping_role_id = row[3] ? row[3] : "";
    c.enabled = row[4] && std::stoi(row[4]) != 0;
    c.p_threshold = row[5] ? std::stod(row[5]) : 0.02;
    c.min_speakers = row[6] ? std::stoi(row[6]) : 2;
    c.cooldown_sec = row[7] ? std::stoi(row[7]) : 3600;
    c.custom_message = row[8] ? row[8] : "";
    out.push_back(std::move(c));
  }
  mysql_free_result(res);
  return out;
}

bool Database::delete_dead_chat_config(const std::string &server_id,
                                       const std::string &channel_id) {
  std::lock_guard<std::mutex> lock(db_mutex_);
  if (!mysql_)
    return false;
  std::string q =
      "DELETE FROM dead_chat_configs WHERE server_id = '" + escape_string(server_id) +
      "' AND channel_id = '" + escape_string(channel_id) + "'";
  return mysql_query(mysql_, q.c_str()) == 0;
}

std::array<double, 168> Database::load_channel_hourly_baselines(const std::string &channel_id) {
  std::array<double, 168> bins{};
  bins.fill(0.0);
  if (channel_id.empty())
    return bins;
  std::lock_guard<std::mutex> lock(db_mutex_);
  if (!mysql_)
    return bins;
  std::string q =
      "SELECT bin_idx, ewma_rate FROM channel_hourly_baselines WHERE channel_id = '" +
      escape_string(channel_id) + "'";
  if (mysql_query(mysql_, q.c_str()) != 0)
    return bins;
  MYSQL_RES *res = mysql_store_result(mysql_);
  if (!res)
    return bins;
  MYSQL_ROW row;
  while ((row = mysql_fetch_row(res))) {
    int idx = row[0] ? std::stoi(row[0]) : -1;
    double val = row[1] ? std::stod(row[1]) : 0.0;
    if (idx >= 0 && idx < 168)
      bins[static_cast<size_t>(idx)] = val;
  }
  mysql_free_result(res);
  return bins;
}

void Database::save_channel_hourly_baselines(const std::string &channel_id,
                                             const std::array<double, 168> &bins) {
  if (channel_id.empty())
    return;
  std::lock_guard<std::mutex> lock(db_mutex_);
  if (!mysql_)
    return;
  std::string esc_ch = escape_string(channel_id);
  std::string q = "INSERT INTO channel_hourly_baselines (channel_id, bin_idx, ewma_rate) VALUES ";
  bool first = true;
  for (int i = 0; i < 168; ++i) {
    if (bins[static_cast<size_t>(i)] <= 0.0)
      continue;
    if (!first)
      q += ",";
    first = false;
    q += "('" + esc_ch + "'," + std::to_string(i) + "," +
         std::to_string(bins[static_cast<size_t>(i)]) + ")";
  }
  if (!first) {
    q += " ON DUPLICATE KEY UPDATE ewma_rate=VALUES(ewma_rate)";
    mysql_query(mysql_, q.c_str());
  }
}

} // namespace bronx
