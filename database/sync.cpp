#include "database.h"
#include <mysql.h>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace bronx {

// ---------------------------------------------------------------------------
// save_server_sync_pairs
// Replaces all existing pairs for (local_server_id, remote_platform) with the
// supplied list in one transaction.
// ---------------------------------------------------------------------------
void Database::save_server_sync_pairs(const std::string &local_server_id,
                                      const std::string &remote_platform,
                                      const std::vector<ServerSyncPair> &pairs) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return;

    std::string esc_server  = escape_string(local_server_id);
    std::string esc_plat    = escape_string(remote_platform);

    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
        .count());

    // Delete old rows for this (server, platform) combination
    std::string del = "DELETE FROM server_sync_pairs "
                      "WHERE local_server_id = '" + esc_server +
                      "' AND remote_platform = '" + esc_plat + "'";
    if (mysql_query(mysql_, del.c_str())) {
        std::cerr << "[bronx db] save_server_sync_pairs delete failed: "
                  << mysql_error(mysql_) << "\n";
        return;
    }

    // Insert each new pair
    for (const auto &p : pairs) {
        std::string esc_local_ch  = escape_string(p.local_channel_id);
        std::string esc_rem_srv   = escape_string(p.remote_server_id);
        std::string esc_rem_ch    = escape_string(p.remote_channel_id);

        std::string ins =
            "INSERT INTO server_sync_pairs "
            "(local_server_id, local_channel_id, remote_platform, "
            " remote_server_id, remote_channel_id, created_at) "
            "VALUES ('" + esc_server  + "', '" + esc_local_ch  + "', '"
                       + esc_plat    + "', '" + esc_rem_srv   + "', '"
                       + esc_rem_ch  + "', " + std::to_string(now) + ") "
            "ON DUPLICATE KEY UPDATE "
            "  remote_server_id  = '" + esc_rem_srv  + "', "
            "  remote_channel_id = '" + esc_rem_ch   + "', "
            "  created_at = "         + std::to_string(now);

        if (mysql_query(mysql_, ins.c_str())) {
            std::cerr << "[bronx db] save_server_sync_pairs insert failed: "
                      << mysql_error(mysql_) << "\n";
        }
    }
}

// ---------------------------------------------------------------------------
// get_server_sync_pairs
// Returns all pairs for a given local server (all platforms).
// ---------------------------------------------------------------------------
std::vector<Database::ServerSyncPair>
Database::get_server_sync_pairs(const std::string &local_server_id) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    std::vector<ServerSyncPair> results;
    if (!mysql_) return results;

    std::string esc_server = escape_string(local_server_id);
    std::string q =
        "SELECT id, local_server_id, local_channel_id, remote_platform, "
        "       remote_server_id, remote_channel_id, created_at "
        "FROM server_sync_pairs "
        "WHERE local_server_id = '" + esc_server + "'";

    if (mysql_query(mysql_, q.c_str()) != 0) {
        std::cerr << "[bronx db] get_server_sync_pairs query failed: "
                  << mysql_error(mysql_) << "\n";
        return results;
    }

    MYSQL_RES *res = mysql_store_result(mysql_);
    if (!res) return results;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        ServerSyncPair p;
        p.id               = row[0] ? std::stoi(row[0]) : 0;
        p.local_server_id  = row[1] ? row[1] : "";
        p.local_channel_id = row[2] ? row[2] : "";
        p.remote_platform  = row[3] ? row[3] : "";
        p.remote_server_id = row[4] ? row[4] : "";
        p.remote_channel_id= row[5] ? row[5] : "";
        p.created_at       = row[6] ? static_cast<uint64_t>(std::stoull(row[6])) : 0;
        results.push_back(std::move(p));
    }
    mysql_free_result(res);
    return results;
}

// ---------------------------------------------------------------------------
// get_server_sync_pair
// Looks up the single pair for a specific local channel + remote platform.
// ---------------------------------------------------------------------------
std::optional<Database::ServerSyncPair>
Database::get_server_sync_pair(const std::string &local_channel_id,
                               const std::string &remote_platform) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return std::nullopt;

    std::string esc_ch   = escape_string(local_channel_id);
    std::string esc_plat = escape_string(remote_platform);

    std::string q =
        "SELECT id, local_server_id, local_channel_id, remote_platform, "
        "       remote_server_id, remote_channel_id, created_at "
        "FROM server_sync_pairs "
        "WHERE local_channel_id = '" + esc_ch +
        "'  AND remote_platform = '" + esc_plat + "' LIMIT 1";

    if (mysql_query(mysql_, q.c_str()) != 0) {
        std::cerr << "[bronx db] get_server_sync_pair query failed: "
                  << mysql_error(mysql_) << "\n";
        return std::nullopt;
    }

    MYSQL_RES *res = mysql_store_result(mysql_);
    if (!res) return std::nullopt;

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) { mysql_free_result(res); return std::nullopt; }

    ServerSyncPair p;
    p.id               = row[0] ? std::stoi(row[0]) : 0;
    p.local_server_id  = row[1] ? row[1] : "";
    p.local_channel_id = row[2] ? row[2] : "";
    p.remote_platform  = row[3] ? row[3] : "";
    p.remote_server_id = row[4] ? row[4] : "";
    p.remote_channel_id= row[5] ? row[5] : "";
    p.created_at       = row[6] ? static_cast<uint64_t>(std::stoull(row[6])) : 0;
    mysql_free_result(res);
    return p;
}

// ---------------------------------------------------------------------------
// remove_server_sync_pairs
// Deletes all pairs for (local_server_id, remote_platform).
// ---------------------------------------------------------------------------
void Database::remove_server_sync_pairs(const std::string &local_server_id,
                                        const std::string &remote_platform) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return;

    std::string esc_server = escape_string(local_server_id);
    std::string esc_plat   = escape_string(remote_platform);

    std::string q = "DELETE FROM server_sync_pairs "
                    "WHERE local_server_id = '" + esc_server +
                    "'  AND remote_platform = '" + esc_plat + "'";

    if (mysql_query(mysql_, q.c_str())) {
        std::cerr << "[bronx db] remove_server_sync_pairs failed: "
                  << mysql_error(mysql_) << "\n";
    }
}

// ---------------------------------------------------------------------------
// Server Backups, Presets & Sync Settings
// ---------------------------------------------------------------------------
bool Database::save_server_backup(const ServerBackup &b) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return false;

    std::string q = "INSERT INTO server_backups "
                    "(backup_id, server_id, creator_id, platform, name, data_json, is_auto, created_at) "
                    "VALUES ('" + escape_string(b.backup_id) + "', '" +
                    escape_string(b.server_id) + "', '" +
                    escape_string(b.creator_id) + "', '" +
                    escape_string(b.platform) + "', '" +
                    escape_string(b.name) + "', '" +
                    escape_string(b.data_json) + "', " +
                    std::to_string(b.is_auto ? 1 : 0) + ", " +
                    std::to_string(b.created_at) + ") "
                    "ON DUPLICATE KEY UPDATE name=VALUES(name), data_json=VALUES(data_json), created_at=VALUES(created_at)";
    if (mysql_query(mysql_, q.c_str())) {
        std::cerr << "[bronx db] save_server_backup failed: " << mysql_error(mysql_) << "\n";
        return false;
    }
    return true;
}

std::optional<ServerBackup> Database::get_server_backup(const std::string &backup_id) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return std::nullopt;

    std::string q = "SELECT backup_id, server_id, creator_id, platform, name, data_json, is_auto, created_at "
                    "FROM server_backups WHERE backup_id = '" + escape_string(backup_id) + "' LIMIT 1";
    if (mysql_query(mysql_, q.c_str())) return std::nullopt;
    MYSQL_RES *res = mysql_store_result(mysql_);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return std::nullopt;
    }
    ServerBackup b;
    b.backup_id  = row[0] ? row[0] : "";
    b.server_id  = row[1] ? row[1] : "";
    b.creator_id = row[2] ? row[2] : "";
    b.platform   = row[3] ? row[3] : "";
    b.name       = row[4] ? row[4] : "";
    b.data_json  = row[5] ? row[5] : "";
    b.is_auto    = row[6] && std::stoi(row[6]) != 0;
    b.created_at = row[7] ? std::stoull(row[7]) : 0;
    mysql_free_result(res);
    return b;
}

std::vector<ServerBackup> Database::list_server_backups(const std::string &server_id,
                                                          const std::string &creator_id) {
    std::vector<ServerBackup> out;
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return out;

    std::string q = "SELECT backup_id, server_id, creator_id, platform, name, data_json, is_auto, created_at "
                    "FROM server_backups WHERE server_id = '" + escape_string(server_id) + "'";
    if (!creator_id.empty()) {
        q += " OR creator_id = '" + escape_string(creator_id) + "'";
    }
    q += " ORDER BY created_at DESC LIMIT 25";
    if (mysql_query(mysql_, q.c_str())) return out;
    MYSQL_RES *res = mysql_store_result(mysql_);
    if (!res) return out;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        ServerBackup b;
        b.backup_id  = row[0] ? row[0] : "";
        b.server_id  = row[1] ? row[1] : "";
        b.creator_id = row[2] ? row[2] : "";
        b.platform   = row[3] ? row[3] : "";
        b.name       = row[4] ? row[4] : "";
        b.data_json  = row[5] ? row[5] : "";
        b.is_auto    = row[6] && std::stoi(row[6]) != 0;
        b.created_at = row[7] ? std::stoull(row[7]) : 0;
        out.push_back(std::move(b));
    }
    mysql_free_result(res);
    return out;
}

bool Database::delete_server_backup(const std::string &backup_id) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return false;
    std::string q = "DELETE FROM server_backups WHERE backup_id = '" + escape_string(backup_id) + "'";
    if (mysql_query(mysql_, q.c_str())) return false;
    return mysql_affected_rows(mysql_) > 0;
}

bool Database::save_server_preset(const ServerPreset &p) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return false;

    std::string q = "INSERT INTO server_presets "
                    "(preset_name, creator_id, description, data_json, is_public, created_at) "
                    "VALUES ('" + escape_string(p.preset_name) + "', '" +
                    escape_string(p.creator_id) + "', '" +
                    escape_string(p.description) + "', '" +
                    escape_string(p.data_json) + "', " +
                    std::to_string(p.is_public ? 1 : 0) + ", " +
                    std::to_string(p.created_at) + ") "
                    "ON DUPLICATE KEY UPDATE description=VALUES(description), data_json=VALUES(data_json), is_public=VALUES(is_public)";
    if (mysql_query(mysql_, q.c_str())) {
        std::cerr << "[bronx db] save_server_preset failed: " << mysql_error(mysql_) << "\n";
        return false;
    }
    return true;
}

std::optional<ServerPreset> Database::get_server_preset(const std::string &preset_name) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return std::nullopt;
    std::string q = "SELECT preset_name, creator_id, description, data_json, is_public, created_at "
                    "FROM server_presets WHERE LOWER(preset_name) = LOWER('" + escape_string(preset_name) + "') LIMIT 1";
    if (mysql_query(mysql_, q.c_str())) return std::nullopt;
    MYSQL_RES *res = mysql_store_result(mysql_);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return std::nullopt;
    }
    ServerPreset p;
    p.preset_name = row[0] ? row[0] : "";
    p.creator_id  = row[1] ? row[1] : "";
    p.description = row[2] ? row[2] : "";
    p.data_json   = row[3] ? row[3] : "";
    p.is_public   = row[4] && std::stoi(row[4]) != 0;
    p.created_at  = row[5] ? std::stoull(row[5]) : 0;
    mysql_free_result(res);
    return p;
}

std::vector<ServerPreset> Database::list_server_presets() {
    std::vector<ServerPreset> out;
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return out;
    std::string q = "SELECT preset_name, creator_id, description, data_json, is_public, created_at "
                    "FROM server_presets WHERE is_public = 1 ORDER BY preset_name ASC LIMIT 50";
    if (mysql_query(mysql_, q.c_str())) return out;
    MYSQL_RES *res = mysql_store_result(mysql_);
    if (!res) return out;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        ServerPreset p;
        p.preset_name = row[0] ? row[0] : "";
        p.creator_id  = row[1] ? row[1] : "";
        p.description = row[2] ? row[2] : "";
        p.data_json   = row[3] ? row[3] : "";
        p.is_public   = row[4] && std::stoi(row[4]) != 0;
        p.created_at  = row[5] ? std::stoull(row[5]) : 0;
        out.push_back(std::move(p));
    }
    mysql_free_result(res);
    return out;
}

bool Database::delete_server_preset(const std::string &preset_name) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return false;
    std::string q = "DELETE FROM server_presets WHERE LOWER(preset_name) = LOWER('" + escape_string(preset_name) + "')";
    if (mysql_query(mysql_, q.c_str())) return false;
    return mysql_affected_rows(mysql_) > 0;
}

void Database::set_auto_backup_config(const ServerAutoBackupConfig &cfg) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return;
    std::string q = "INSERT INTO server_auto_backups (server_id, platform, interval_hours, last_backup_at, enabled) "
                    "VALUES ('" + escape_string(cfg.server_id) + "', '" + escape_string(cfg.platform) + "', " +
                    std::to_string(cfg.interval_hours) + ", " + std::to_string(cfg.last_backup_at) + ", " +
                    std::to_string(cfg.enabled ? 1 : 0) + ") "
                    "ON DUPLICATE KEY UPDATE interval_hours=VALUES(interval_hours), enabled=VALUES(enabled)";
    mysql_query(mysql_, q.c_str());
}

std::optional<ServerAutoBackupConfig> Database::get_auto_backup_config(const std::string &server_id) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return std::nullopt;
    std::string q = "SELECT server_id, platform, interval_hours, last_backup_at, enabled "
                    "FROM server_auto_backups WHERE server_id = '" + escape_string(server_id) + "' LIMIT 1";
    if (mysql_query(mysql_, q.c_str())) return std::nullopt;
    MYSQL_RES *res = mysql_store_result(mysql_);
    if (!res) return std::nullopt;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        return std::nullopt;
    }
    ServerAutoBackupConfig c;
    c.server_id      = row[0] ? row[0] : "";
    c.platform       = row[1] ? row[1] : "";
    c.interval_hours = row[2] ? std::stoi(row[2]) : 24;
    c.last_backup_at = row[3] ? std::stoull(row[3]) : 0;
    c.enabled        = row[4] && std::stoi(row[4]) != 0;
    mysql_free_result(res);
    return c;
}

std::vector<ServerAutoBackupConfig> Database::get_due_auto_backups(const std::string &platform, uint64_t now_ts) {
    std::vector<ServerAutoBackupConfig> out;
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return out;
    std::string q = "SELECT server_id, platform, interval_hours, last_backup_at, enabled "
                    "FROM server_auto_backups WHERE platform = '" + escape_string(platform) +
                    "' AND enabled = 1 AND (" + std::to_string(now_ts) + " - last_backup_at) >= (interval_hours * 3600)";
    if (mysql_query(mysql_, q.c_str())) return out;
    MYSQL_RES *res = mysql_store_result(mysql_);
    if (!res) return out;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        ServerAutoBackupConfig c;
        c.server_id      = row[0] ? row[0] : "";
        c.platform       = row[1] ? row[1] : "";
        c.interval_hours = row[2] ? std::stoi(row[2]) : 24;
        c.last_backup_at = row[3] ? std::stoull(row[3]) : 0;
        c.enabled        = true;
        out.push_back(std::move(c));
    }
    mysql_free_result(res);
    return out;
}

void Database::update_auto_backup_last_run(const std::string &server_id, uint64_t ts) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return;
    std::string q = "UPDATE server_auto_backups SET last_backup_at = " + std::to_string(ts) +
                    " WHERE server_id = '" + escape_string(server_id) + "'";
    mysql_query(mysql_, q.c_str());
}

void Database::save_server_sync_settings(const ServerSyncSettings &cfg) {
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return;
    std::string q = "INSERT INTO server_sync_settings "
                    "(local_server_id, remote_platform, sync_messages, sync_bans, sync_roles, sync_channels) "
                    "VALUES ('" + escape_string(cfg.local_server_id) + "', '" +
                    escape_string(cfg.remote_platform) + "', " +
                    std::to_string(cfg.sync_messages ? 1 : 0) + ", " +
                    std::to_string(cfg.sync_bans ? 1 : 0) + ", " +
                    std::to_string(cfg.sync_roles ? 1 : 0) + ", " +
                    std::to_string(cfg.sync_channels ? 1 : 0) + ") "
                    "ON DUPLICATE KEY UPDATE sync_messages=VALUES(sync_messages), sync_bans=VALUES(sync_bans), "
                    "sync_roles=VALUES(sync_roles), sync_channels=VALUES(sync_channels)";
    mysql_query(mysql_, q.c_str());
}

ServerSyncSettings Database::get_server_sync_settings(const std::string &local_server_id,
                                                      const std::string &remote_platform) {
    ServerSyncSettings cfg;
    cfg.local_server_id = local_server_id;
    cfg.remote_platform = remote_platform;
    std::lock_guard<std::mutex> db_lock(db_mutex_);
    if (!mysql_) return cfg;
    std::string q = "SELECT sync_messages, sync_bans, sync_roles, sync_channels FROM server_sync_settings "
                    "WHERE local_server_id = '" + escape_string(local_server_id) +
                    "' AND remote_platform = '" + escape_string(remote_platform) + "' LIMIT 1";
    if (mysql_query(mysql_, q.c_str())) return cfg;
    MYSQL_RES *res = mysql_store_result(mysql_);
    if (!res) return cfg;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row) {
        cfg.sync_messages = !row[0] || std::stoi(row[0]) != 0;
        cfg.sync_bans     = !row[1] || std::stoi(row[1]) != 0;
        cfg.sync_roles    = !row[2] || std::stoi(row[2]) != 0;
        cfg.sync_channels = !row[3] || std::stoi(row[3]) != 0;
    }
    mysql_free_result(res);
    return cfg;
}

} // namespace bronx
