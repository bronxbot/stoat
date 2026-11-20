#include "database.h"
#include <mysql.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bronx {

  void Database::start_telemetry_flusher() {
    if (telemetry_running_.exchange(true))
      return;
    telemetry_flusher_thread_ = std::thread([this]() {
      while (telemetry_running_) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!telemetry_running_)
          break;
        flush_telemetry();
      }
      flush_telemetry();
    });
  }

  void Database::stop_telemetry_flusher() {
    if (telemetry_running_.exchange(false)) {
      if (telemetry_flusher_thread_.joinable()) {
        telemetry_flusher_thread_.join();
      }
    }
  }

  void Database::flush_telemetry() {
    std::unordered_map<std::string, PendingUserActivity> user_act;
    std::unordered_map<std::string, int> ch_act;
    std::unordered_map<std::string, std::pair<int, int>> user_ch_act;

    {
      std::lock_guard<std::mutex> lock(telemetry_mutex_);
      if (pending_user_activity_.empty() && pending_channel_activity_.empty() &&
          pending_user_channel_activity_.empty()) {
        return;
      }
      user_act.swap(pending_user_activity_);
      ch_act.swap(pending_channel_activity_);
      user_ch_act.swap(pending_user_channel_activity_);
    }

    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_)
      return;

    for (const auto &[key, val] : user_act) {
      auto tab_pos = key.find('\t');
      if (tab_pos == std::string::npos)
        continue;
      std::string g_id = key.substr(0, tab_pos);
      std::string u_id = key.substr(tab_pos + 1);

      std::string sql =
          "INSERT INTO guild_user_activity_daily (guild_id, user_id, "
          "stat_date, is_bot, messages, edits, deletes, commands_used) "
          "VALUES ('" +
          escape_string(g_id) + "', '" + escape_string(u_id) + "', CURDATE(), " +
          (val.is_bot ? "TRUE" : "FALSE") + ", " +
          std::to_string(val.messages) + ", " + std::to_string(val.edits) +
          ", " + std::to_string(val.deletes) + ", " +
          std::to_string(val.commands) +
          ") ON DUPLICATE KEY UPDATE "
          "messages = messages + " + std::to_string(val.messages) +
          ", edits = edits + " + std::to_string(val.edits) +
          ", deletes = deletes + " + std::to_string(val.deletes) +
          ", commands_used = commands_used + " + std::to_string(val.commands) +
          ", is_bot = VALUES(is_bot)";
      mysql_query(mysql_, sql.c_str());
    }

    for (const auto &[key, count] : ch_act) {
      auto tab_pos = key.find('\t');
      if (tab_pos == std::string::npos)
        continue;
      std::string g_id = key.substr(0, tab_pos);
      std::string c_id = key.substr(tab_pos + 1);

      std::string sql =
          "INSERT INTO guild_channel_activity_daily (guild_id, channel_id, "
          "stat_date, messages) VALUES ('" +
          escape_string(g_id) + "', '" + escape_string(c_id) + "', CURDATE(), " +
          std::to_string(count) +
          ") ON DUPLICATE KEY UPDATE messages = messages + " +
          std::to_string(count);
      mysql_query(mysql_, sql.c_str());
    }

    for (const auto &[key, counts] : user_ch_act) {
      auto p1 = key.find('\t');
      if (p1 == std::string::npos)
        continue;
      auto p2 = key.find('\t', p1 + 1);
      if (p2 == std::string::npos)
        continue;
      std::string g_id = key.substr(0, p1);
      std::string c_id = key.substr(p1 + 1, p2 - (p1 + 1));
      std::string u_id = key.substr(p2 + 1);

      std::string sql =
          "INSERT INTO guild_user_channel_activity_daily (guild_id, channel_id, "
          "user_id, stat_date, messages, commands_used) VALUES ('" +
          escape_string(g_id) + "', '" + escape_string(c_id) + "', '" +
          escape_string(u_id) + "', CURDATE(), " +
          std::to_string(counts.first) + ", " + std::to_string(counts.second) +
          ") ON DUPLICATE KEY UPDATE messages = messages + " +
          std::to_string(counts.first) +
          ", commands_used = commands_used + " + std::to_string(counts.second);
      mysql_query(mysql_, sql.c_str());
    }
  }

  bool Database::increment_user_daily_messages(const std::string &guild_id,
                                     const std::string &user_id, bool is_bot,
                                     const std::string &type) {
    if (guild_id.empty() || user_id.empty())
      return false;
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    auto &entry = pending_user_activity_[guild_id + "\t" + user_id];
    entry.is_bot = is_bot;
    if (type == "edit")
      entry.edits++;
    else if (type == "delete")
      entry.deletes++;
    else
      entry.messages++;
    return true;
  }

  bool Database::increment_user_daily_commands(const std::string &guild_id,
                                     const std::string &user_id, bool is_bot) {
    if (guild_id.empty() || user_id.empty())
      return false;
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    auto &entry = pending_user_activity_[guild_id + "\t" + user_id];
    entry.is_bot = is_bot;
    entry.commands++;
    return true;
  }

  bool Database::increment_channel_daily_messages(const std::string &guild_id,
                                        const std::string &channel_id) {
    if (guild_id.empty() || channel_id.empty())
      return false;
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    pending_channel_activity_[guild_id + "\t" + channel_id]++;
    return true;
  }

  bool Database::increment_user_channel_messages(const std::string &guild_id,
                                       const std::string &channel_id,
                                       const std::string &user_id) {
    if (guild_id.empty() || channel_id.empty() || user_id.empty())
      return false;
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    pending_user_channel_activity_[guild_id + "\t" + channel_id + "\t" + user_id]
        .first++;
    return true;
  }

  bool Database::increment_user_channel_commands(const std::string &guild_id,
                                       const std::string &channel_id,
                                       const std::string &user_id) {
    if (guild_id.empty() || channel_id.empty() || user_id.empty())
      return false;
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    pending_user_channel_activity_[guild_id + "\t" + channel_id + "\t" + user_id]
        .second++;
    return true;
  }

  bool Database::increment_guild_member_joins(const std::string &guild_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!mysql_)
      return false;
    std::string sql = "INSERT INTO guild_member_events_daily (guild_id, "
                      "event_date, joins, leaves) VALUES ('" +
                      escape_string(guild_id) +
                      "', CURDATE(), 1, 0) "
                      "ON DUPLICATE KEY UPDATE joins = joins + 1";
    return mysql_query(mysql_, sql.c_str()) == 0;
  }

  bool Database::increment_guild_member_leaves(const std::string &guild_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!mysql_)
      return false;
    std::string sql = "INSERT INTO guild_member_events_daily (guild_id, "
                      "event_date, joins, leaves) VALUES ('" +
                      escape_string(guild_id) +
                      "', CURDATE(), 0, 1) "
                      "ON DUPLICATE KEY UPDATE leaves = leaves + 1";
    return mysql_query(mysql_, sql.c_str()) == 0;
  }

  std::vector<UserChannelActivityEntry>
  Database::get_user_channel_stats(const std::string &guild_id,
                         const std::string &user_id, int days) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<UserChannelActivityEntry> entries;
    if (!mysql_)
      return entries;

    std::string sql = "SELECT channel_id, SUM(messages), SUM(commands_used) "
                      "FROM guild_user_channel_activity_daily "
                      "WHERE guild_id = '" +
                      escape_string(guild_id) +
                      "' "
                      "AND user_id = '" +
                      escape_string(user_id) +
                      "' "
                      "AND stat_date >= DATE_SUB(CURDATE(), INTERVAL " +
                      std::to_string(days) +
                      " DAY) "
                      "GROUP BY channel_id ORDER BY SUM(messages) DESC";
    if (mysql_query(mysql_, sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
          if (row[0]) {
            UserChannelActivityEntry entry;
            entry.channel_id = row[0];
            entry.messages = row[1] ? std::stoi(row[1]) : 0;
            entry.commands_used = row[2] ? std::stoi(row[2]) : 0;
            entries.push_back(entry);
          }
        }
        mysql_free_result(res);
      }
    }
    return entries;
  }

  std::vector<UserGuildActivityEntry>
  Database::get_user_global_stats(const std::string &user_id, int days) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<UserGuildActivityEntry> entries;
    if (!mysql_)
      return entries;

    std::string sql = "SELECT guild_id, SUM(messages), SUM(commands_used) FROM "
                      "guild_user_activity_daily "
                      "WHERE user_id = '" +
                      escape_string(user_id) +
                      "' "
                      "AND stat_date >= DATE_SUB(CURDATE(), INTERVAL " +
                      std::to_string(days) +
                      " DAY) "
                      "GROUP BY guild_id ORDER BY SUM(messages) DESC";
    if (mysql_query(mysql_, sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
          if (row[0]) {
            UserGuildActivityEntry entry;
            entry.guild_id = row[0];
            entry.messages = row[1] ? std::stoi(row[1]) : 0;
            entry.commands_used = row[2] ? std::stoi(row[2]) : 0;
            entries.push_back(entry);
          }
        }
        mysql_free_result(res);
      }
    }
    return entries;
  }

  bool Database::fix_bot_flags(const std::string &user_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!mysql_ || user_id.empty())
      return false;
    std::string sql = "UPDATE guild_user_activity_daily SET is_bot = TRUE "
                      "WHERE user_id = '" +
                      escape_string(user_id) + "' AND is_bot = FALSE";
    if (mysql_query(mysql_, sql.c_str()) != 0)
      return false;
    auto affected = mysql_affected_rows(mysql_);
    if (affected > 0) {
      std::cout << "[bronx db] fixed " << affected << " row(s): marked user "
                << user_id << " as bot.\n";
    }
    return true;
  }

  bool Database::increment_command_usage(const std::string &guild_id,
                               const std::string &command_name,
                               const std::string &channel_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!mysql_)
      return false;
    std::string sql =
        "INSERT INTO guild_command_usage (guild_id, command_name, channel_id, "
        "usage_date, use_count) VALUES ('" +
        escape_string(guild_id) + "', '" + escape_string(command_name) +
        "', '" + escape_string(channel_id) +
        "', CURDATE(), 1) "
        "ON DUPLICATE KEY UPDATE use_count = use_count + 1";
    return mysql_query(mysql_, sql.c_str()) == 0;
  }

  std::vector<TopUserEntry> Database::get_top_users(const std::string &guild_id, int days,
                                          bool include_bots,
                                          int limit) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<TopUserEntry> entries;
    if (!mysql_)
      return entries;

    std::string sql =
        "SELECT user_id, SUM(messages) FROM guild_user_activity_daily "
        "WHERE guild_id = '" +
        escape_string(guild_id) + "' " +
        (include_bots ? "" : "AND is_bot = FALSE ") +
        "AND stat_date >= DATE_SUB(CURDATE(), INTERVAL " +
        std::to_string(days) +
        " DAY) "
        "GROUP BY user_id ORDER BY SUM(messages) DESC LIMIT " +
        std::to_string(limit);
    if (mysql_query(mysql_, sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
          if (row[0] && row[1]) {
            entries.push_back({row[0], std::stoi(row[1])});
          }
        }
        mysql_free_result(res);
      }
    }
    return entries;
  }

  std::vector<TopCmdEntry> Database::get_top_commands(const std::string &guild_id,
                                            int days, int limit) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<TopCmdEntry> entries;
    if (!mysql_)
      return entries;

    std::string sql =
        "SELECT command_name, SUM(use_count) FROM guild_command_usage "
        "WHERE guild_id = '" +
        escape_string(guild_id) +
        "' "
        "AND usage_date >= DATE_SUB(CURDATE(), INTERVAL " +
        std::to_string(days) +
        " DAY) "
        "GROUP BY command_name ORDER BY SUM(use_count) DESC LIMIT " +
        std::to_string(limit);
    if (mysql_query(mysql_, sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
          if (row[0] && row[1]) {
            entries.push_back({row[0], std::stoi(row[1])});
          }
        }
        mysql_free_result(res);
      }
    }
    return entries;
  }

  std::vector<TopCmdEntry> Database::get_top_commands_global(int days, int limit) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<TopCmdEntry> entries;
    if (!mysql_)
      return entries;

    std::string sql =
        "SELECT command_name, SUM(use_count) FROM guild_command_usage "
        "WHERE usage_date >= DATE_SUB(CURDATE(), INTERVAL " +
        std::to_string(days) +
        " DAY) "
        "GROUP BY command_name ORDER BY SUM(use_count) DESC LIMIT " +
        std::to_string(limit);
    if (mysql_query(mysql_, sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
          if (row[0] && row[1]) {
            entries.push_back({row[0], std::stoi(row[1])});
          }
        }
        mysql_free_result(res);
      }
    }
    return entries;
  }

  std::vector<TopServerEntry>
  Database::get_top_servers(int days, bool include_bots, int limit) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<TopServerEntry> entries;
    if (!mysql_)
      return entries;
    std::string sql =
        "SELECT guild_id, SUM(messages) FROM guild_user_activity_daily "
        "WHERE stat_date >= DATE_SUB(CURDATE(), INTERVAL " +
        std::to_string(days) + " DAY) " +
        (include_bots ? "" : "AND is_bot = FALSE ") +
        "GROUP BY guild_id ORDER BY SUM(messages) DESC LIMIT " +
        std::to_string(limit);
    if (mysql_query(mysql_, sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
          if (row[0] && row[1]) {
            entries.push_back({row[0], std::stoi(row[1])});
          }
        }
        mysql_free_result(res);
      }
    }
    return entries;
  }

  std::vector<TopUserEntry> Database::get_top_users_global(int days, bool bots,
                                                 int limit) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<TopUserEntry> entries;
    if (!mysql_)
      return entries;
    std::string sql =
        "SELECT user_id, SUM(messages) FROM guild_user_activity_daily "
        "WHERE stat_date >= DATE_SUB(CURDATE(), INTERVAL " +
        std::to_string(days) +
        " DAY) "
        "AND is_bot = " +
        (bots ? "TRUE" : "FALSE") +
        " "
        "GROUP BY user_id ORDER BY SUM(messages) DESC LIMIT " +
        std::to_string(limit);
    if (mysql_query(mysql_, sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
          if (row[0] && row[1]) {
            entries.push_back({row[0], std::stoi(row[1])});
          }
        }
        mysql_free_result(res);
      }
    }
    return entries;
  }

  std::vector<TopChannelEntry> Database::get_top_channels(const std::string &guild_id,
                                                int days, int limit) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<TopChannelEntry> entries;
    if (!mysql_)
      return entries;
    std::string sql =
        "SELECT channel_id, SUM(messages) FROM guild_channel_activity_daily "
        "WHERE guild_id = '" +
        escape_string(guild_id) +
        "' "
        "AND stat_date >= DATE_SUB(CURDATE(), INTERVAL " +
        std::to_string(days) +
        " DAY) "
        "GROUP BY channel_id ORDER BY SUM(messages) DESC LIMIT " +
        std::to_string(limit);
    if (mysql_query(mysql_, sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
          if (row[0] && row[1]) {
            entries.push_back({row[0], std::stoi(row[1])});
          }
        }
        mysql_free_result(res);
      }
    }
    return entries;
  }

  ServerTotals Database::get_server_totals(const std::string &guild_id, int days,
                                 bool include_bots) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    ServerTotals totals;
    if (!mysql_)
      return totals;

    std::string msg_sql = "SELECT SUM(messages) FROM guild_user_activity_daily "
                          "WHERE guild_id = '" +
                          escape_string(guild_id) + "' " +
                          (include_bots ? "" : "AND is_bot = FALSE ") +
                          "AND stat_date >= DATE_SUB(CURDATE(), INTERVAL " +
                          std::to_string(days) + " DAY)";
    if (mysql_query(mysql_, msg_sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) {
          totals.total_messages = std::stoi(row[0]);
        }
        mysql_free_result(res);
      }
    }

    std::string cmd_sql = "SELECT SUM(use_count) FROM guild_command_usage "
                          "WHERE guild_id = '" +
                          escape_string(guild_id) +
                          "' "
                          "AND usage_date >= DATE_SUB(CURDATE(), INTERVAL " +
                          std::to_string(days) + " DAY)";
    if (mysql_query(mysql_, cmd_sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) {
          totals.total_commands = std::stoi(row[0]);
        }
        mysql_free_result(res);
      }
    }

    std::string users_sql =
        "SELECT COUNT(DISTINCT user_id) FROM guild_user_activity_daily "
        "WHERE guild_id = '" +
        escape_string(guild_id) + "' " +
        (include_bots ? "" : "AND is_bot = FALSE ") +
        "AND stat_date >= DATE_SUB(CURDATE(), INTERVAL " +
        std::to_string(days) + " DAY)";
    if (mysql_query(mysql_, users_sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) {
          totals.active_users = std::stoi(row[0]);
        }
        mysql_free_result(res);
      }
    }

    return totals;
  }

  std::vector<std::pair<std::string, int64_t>>
  Database::get_daily_message_series(const std::string &guild_id, int days,
                           bool include_bots) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<std::pair<std::string, int64_t>> result;
    if (!mysql_)
      return result;
    std::string sql = "SELECT DATE_FORMAT(stat_date,'%m-%d'), SUM(messages) "
                      "FROM guild_user_activity_daily "
                      "WHERE guild_id = '" +
                      escape_string(guild_id) + "' " +
                      (include_bots ? "" : "AND is_bot = FALSE ") +
                      "AND stat_date >= DATE_SUB(CURDATE(), INTERVAL " +
                      std::to_string(days) +
                      " DAY) "
                      "GROUP BY stat_date ORDER BY stat_date ASC";
    if (mysql_query(mysql_, sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
          if (row[0] && row[1])
            result.push_back({row[0], std::stoll(row[1])});
        }
        mysql_free_result(res);
      }
    }
    return result;
  }

  std::vector<std::vector<double>>
  Database::get_hourly_heatmap(const std::string &guild_id, int days,
                     bool include_bots) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<std::vector<double>> matrix(7, std::vector<double>(24, 0.0));
    if (!mysql_)
      return matrix;
    // guild_user_activity_daily only has per-day granularity, so we use
    // moderation_logs created_at and message event times if available.
    // For now, distribute each day's messages evenly weighted by a heuristic.
    // The heatmap becomes accurate once we add hour-level tracking.
    // Query: sum messages grouped by weekday of stat_date as a proxy.
    std::string sql = "SELECT DAYOFWEEK(stat_date)-1, SUM(messages) "
                      "FROM guild_user_activity_daily "
                      "WHERE guild_id = '" +
                      escape_string(guild_id) + "' " +
                      (include_bots ? "" : "AND is_bot = FALSE ") +
                      "AND stat_date >= DATE_SUB(CURDATE(), INTERVAL " +
                      std::to_string(days) +
                      " DAY) "
                      "GROUP BY DAYOFWEEK(stat_date)";
    if (mysql_query(mysql_, sql.c_str()) == 0) {
      MYSQL_RES *res = mysql_store_result(mysql_);
      if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
          if (!row[0] || !row[1])
            continue;
          int dow = std::stoi(row[0]); // 0=Sun..6=Sat
          double total = std::stod(row[1]);
          if (dow < 0 || dow >= 7)
            continue;
          // Distribute across a realistic activity curve (peak 10-22h)
          const double weights[24] = {
              0.5, 0.3, 0.2, 0.1, 0.1, 0.2, // 0-5
              0.5, 0.8, 1.0, 1.1, 1.2, 1.3, // 6-11
              1.3, 1.3, 1.2, 1.2, 1.3, 1.4, // 12-17
              1.5, 1.5, 1.4, 1.2, 1.0, 0.7  // 18-23
          };
          double wsum;
          for (auto w : weights)
            wsum += w;
          for (int h; h < 24; ++h)
            matrix[dow][h] = total * (weights[h] / wsum);
        }
        mysql_free_result(res);
      }
    }
    return matrix;
  }

} // namespace bronx
