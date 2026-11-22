#pragma once
#include "stoatpp/bot_module.h"
#include "helpers.h"
#include "../database/database.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bronx::cogs {

class BackupModule : public stoatpp::bot_module {
public:
    static BackupModule* instance;

    ~BackupModule() {
        running_ = false;
        if (auto_thread_.joinable()) auto_thread_.join();
    }

    static std::string generate_backup_id() {
        static const char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(chars) - 2);
        std::string id;
        for (int i = 0; i < 8; ++i) id += chars[dis(gen)];
        return id;
    }

    static bool is_admin_or_owner(stoatpp::cluster& bot, const std::string& server_id,
                                   const std::string& user_id, const stoatpp::models::Member& member) {
        auto srv = bot.get_server(server_id);
        if (srv && srv->owner == user_id) return true;
        if (bronx::g_db && bronx::g_db->is_trusted(server_id, user_id, member.roles)) return true;
        if (srv) {
            for (const auto& rid : member.roles) {
                for (const auto& r : srv->roles) {
                    if (r.id == rid && (r.allowed_permissions() & (1LL << 0))) return true;
                }
            }
        }
        return false;
    }

    // Portable Xenon-style server snapshot
    static nlohmann::json snapshot_server_json(stoatpp::cluster& bot, const std::string& server_id) {
        nlohmann::json snap;
        auto srv_opt = bot.get_server(server_id);
        if (!srv_opt) {
            auto res = bot.rest().get_server(server_id);
            if (res.success() && res.body.is_object()) {
                srv_opt = stoatpp::models::Server::from_json(res.body);
            }
        }
        if (!srv_opt) return snap;
        const auto& srv = *srv_opt;

        snap["name"] = srv.name;
        snap["description"] = srv.description.value_or("");
        if (srv.raw.contains("default_permissions")) {
            snap["default_permissions"] = srv.raw["default_permissions"];
        }

        // Map role_id -> role_name for portable permission overwrites
        std::unordered_map<std::string, std::string> role_id_to_name;
        nlohmann::json roles_arr = nlohmann::json::array();
        std::vector<stoatpp::models::Role> sorted_roles = srv.roles;
        std::sort(sorted_roles.begin(), sorted_roles.end(), [](const auto& a, const auto& b) {
            return a.rank < b.rank;
        });
        for (const auto& r : sorted_roles) {
            role_id_to_name[r.id] = r.name;
            nlohmann::json rj;
            rj["name"] = r.name;
            if (r.colour) rj["colour"] = *r.colour;
            rj["rank"] = r.rank;
            rj["permissions"] = r.permissions;
            roles_arr.push_back(rj);
        }
        snap["roles"] = roles_arr;

        // Map channel_id -> category_title
        std::unordered_map<std::string, std::string> chan_to_cat;
        nlohmann::json cats_arr = nlohmann::json::array();
        if (srv.raw.contains("categories") && srv.raw["categories"].is_array()) {
            for (const auto& cat : srv.raw["categories"]) {
                std::string title = cat.value("title", "");
                if (title.empty()) continue;
                nlohmann::json cj;
                cj["title"] = title;
                nlohmann::json ch_names = nlohmann::json::array();
                if (cat.contains("channels") && cat["channels"].is_array()) {
                    for (const auto& cid_j : cat["channels"]) {
                        if (!cid_j.is_string()) continue;
                        std::string cid = cid_j.get<std::string>();
                        chan_to_cat[cid] = title;
                        auto ch_opt = bot.get_channel(cid);
                        if (ch_opt && ch_opt->name) ch_names.push_back(*ch_opt->name);
                    }
                }
                cj["channels"] = ch_names;
                cats_arr.push_back(cj);
            }
        }
        snap["categories"] = cats_arr;

        // Channels
        nlohmann::json chans_arr = nlohmann::json::array();
        for (const auto& cid : srv.channels) {
            auto ch_opt = bot.get_channel(cid);
            if (!ch_opt) {
                auto cres = bot.rest().get_channel(cid);
                if (cres.success() && cres.body.is_object()) {
                    ch_opt = stoatpp::models::Channel::from_json(cres.body);
                }
            }
            if (!ch_opt) continue;
            const auto& ch = *ch_opt;
            nlohmann::json cj;
            cj["name"] = ch.name.value_or("channel");
            cj["channel_type"] = ch.channel_type.empty() ? "TextChannel" : ch.channel_type;
            cj["description"] = ch.description.value_or("");
            cj["nsfw"] = ch.nsfw;
            if (chan_to_cat.count(cid)) cj["category"] = chan_to_cat[cid];
            if (ch.default_permissions) cj["default_permissions"] = *ch.default_permissions;
            if (ch.role_permissions && ch.role_permissions->is_object()) {
                nlohmann::json rp_by_name = nlohmann::json::object();
                for (auto& [rid, perm] : ch.role_permissions->items()) {
                    auto rit = role_id_to_name.find(rid);
                    if (rit != role_id_to_name.end()) {
                        rp_by_name[rit->second] = perm;
                    }
                }
                cj["role_permissions"] = rp_by_name;
            }
            chans_arr.push_back(cj);
        }
        snap["channels"] = chans_arr;

        // Bans
        nlohmann::json bans_arr = nlohmann::json::array();
        auto bans_res = bot.rest().get_server_bans(server_id);
        if (bans_res.success() && bans_res.body.is_object() && bans_res.body.contains("bans") && bans_res.body["bans"].is_array()) {
            for (const auto& b : bans_res.body["bans"]) {
                nlohmann::json bj;
                if (b.contains("_id") && b["_id"].is_object()) {
                    bj["user_id"] = b["_id"].value("user", "");
                } else if (b.contains("user_id")) {
                    bj["user_id"] = b.value("user_id", "");
                }
                bj["reason"] = b.value("reason", "");
                if (!bj["user_id"].get<std::string>().empty()) bans_arr.push_back(bj);
            }
        }
        snap["bans"] = bans_arr;

        return snap;
    }

    // Apply Xenon-style snapshot JSON to a target server
    static std::string apply_server_json(stoatpp::cluster& bot,
                                         const std::string& server_id,
                                         const nlohmann::json& snap,
                                         bool clear_existing,
                                         const std::string& keep_channel_id = "") {
        int roles_created = 0;
        int channels_created = 0;
        int categories_set = 0;

        auto srv_opt = bot.get_server(server_id);
        std::unordered_map<std::string, std::string> existing_roles_by_name;
        std::unordered_map<std::string, std::string> existing_chans_by_name;

        if (srv_opt) {
            if (clear_existing) {
                for (const auto& cid : srv_opt->channels) {
                    if (cid != keep_channel_id) {
                        bot.rest().delete_channel(cid);
                    }
                }
                for (const auto& r : srv_opt->roles) {
                    bot.rest().delete_role(server_id, r.id);
                }
            } else {
                for (const auto& r : srv_opt->roles) existing_roles_by_name[r.name] = r.id;
                for (const auto& cid : srv_opt->channels) {
                    auto ch_opt = bot.get_channel(cid);
                    if (ch_opt && ch_opt->name) existing_chans_by_name[*ch_opt->name] = cid;
                }
            }
        }

        // 1. Update server name/description if requested
        if (clear_existing && snap.contains("name") && snap["name"].is_string()) {
            nlohmann::json srv_patch;
            srv_patch["name"] = snap["name"].get<std::string>();
            if (snap.contains("description") && snap["description"].is_string() && !snap["description"].get<std::string>().empty()) {
                srv_patch["description"] = snap["description"].get<std::string>();
            }
            bot.rest().edit_server(server_id, srv_patch);
        }

        // 2. Create roles
        std::unordered_map<std::string, std::string> role_name_to_id = existing_roles_by_name;
        std::vector<std::string> rank_order;
        if (snap.contains("roles") && snap["roles"].is_array()) {
            for (const auto& rj : snap["roles"]) {
                std::string rname = rj.value("name", "");
                if (rname.empty()) continue;
                std::string rid;
                if (role_name_to_id.count(rname)) {
                    rid = role_name_to_id[rname];
                } else {
                    auto rres = bot.rest().create_role(server_id, rname);
                    if (rres.success() && rres.body.is_object()) {
                        if (rres.body.contains("id") && rres.body["id"].is_string()) rid = rres.body["id"].get<std::string>();
                        else if (rres.body.contains("_id") && rres.body["_id"].is_string()) rid = rres.body["_id"].get<std::string>();
                    }
                    if (!rid.empty()) {
                        roles_created++;
                        role_name_to_id[rname] = rid;
                    }
                }
                if (!rid.empty()) {
                    rank_order.push_back(rid);
                    nlohmann::json rpatch = nlohmann::json::object();
                    if (rj.contains("colour") && rj["colour"].is_string() && !rj["colour"].get<std::string>().empty()) {
                        rpatch["colour"] = rj["colour"];
                    }
                    if (rj.contains("permissions") && !rj["permissions"].is_null()) {
                        rpatch["permissions"] = rj["permissions"];
                    }
                    if (!rpatch.empty()) {
                        bot.rest().edit_role(server_id, rid, rpatch);
                    }
                }
            }
            if (!rank_order.empty()) {
                bot.rest().edit_role_ranks(server_id, rank_order);
            }
        }

        // 3. Create channels
        std::unordered_map<std::string, std::string> chan_name_to_id = existing_chans_by_name;
        if (snap.contains("channels") && snap["channels"].is_array()) {
            for (const auto& cj : snap["channels"]) {
                std::string cname = cj.value("name", "");
                if (cname.empty()) continue;
                std::string ctype_raw = cj.value("channel_type", "TextChannel");
                std::string ctype = (ctype_raw.find("Voice") != std::string::npos || ctype_raw == "voice") ? "Voice" : "Text";

                std::string cid;
                if (chan_name_to_id.count(cname)) {
                    cid = chan_name_to_id[cname];
                } else {
                    auto cres = bot.rest().create_channel(server_id, ctype, cname);
                    if (cres.success() && cres.body.is_object()) {
                        if (cres.body.contains("_id") && cres.body["_id"].is_string()) cid = cres.body["_id"].get<std::string>();
                        else if (cres.body.contains("id") && cres.body["id"].is_string()) cid = cres.body["id"].get<std::string>();
                    }
                    if (!cid.empty()) {
                        channels_created++;
                        chan_name_to_id[cname] = cid;
                    }
                }

                if (!cid.empty()) {
                    nlohmann::json cpatch = nlohmann::json::object();
                    if (cj.contains("description") && cj["description"].is_string() && !cj["description"].get<std::string>().empty()) {
                        cpatch["description"] = cj["description"];
                    }
                    if (cj.contains("nsfw") && cj["nsfw"].is_boolean() && cj["nsfw"].get<bool>()) {
                        cpatch["nsfw"] = true;
                    }
                    if (!cpatch.empty()) {
                        bot.rest().edit_channel(cid, cpatch);
                    }
                    if (cj.contains("default_permissions") && cj["default_permissions"].is_number_integer()) {
                        bot.rest().set_channel_default_permission(cid, cj["default_permissions"].get<int64_t>());
                    }
                    if (cj.contains("role_permissions") && cj["role_permissions"].is_object()) {
                        for (auto& [rname, perm] : cj["role_permissions"].items()) {
                            auto rit = role_name_to_id.find(rname);
                            if (rit != role_name_to_id.end() && perm.is_object()) {
                                int64_t allow = perm.value("a", 0LL);
                                int64_t deny = perm.value("d", 0LL);
                                bot.rest().set_channel_permission(cid, rit->second, allow, deny);
                            }
                        }
                    }
                    if (cj.contains("category") && cj["category"].is_string() && !cj["category"].get<std::string>().empty()) {
                        bot.rest().move_channel_to_category(server_id, cid, cj["category"].get<std::string>());
                    }
                }
            }
        }

        // 4. Organize categories
        if (snap.contains("categories") && snap["categories"].is_array()) {
            for (const auto& cat : snap["categories"]) {
                std::string title = cat.value("title", "");
                if (title.empty() || !cat.contains("channels") || !cat["channels"].is_array()) continue;
                categories_set++;
                for (const auto& ch_name_j : cat["channels"]) {
                    if (!ch_name_j.is_string()) continue;
                    auto cit = chan_name_to_id.find(ch_name_j.get<std::string>());
                    if (cit != chan_name_to_id.end()) {
                        bot.rest().move_channel_to_category(server_id, cit->second, title);
                    }
                }
            }
        }

        std::ostringstream ss;
        ss << "✅ **Restore complete!** Created **" << roles_created << "** roles, **"
           << channels_created << "** channels, and organized **" << categories_set << "** categories.";
        return ss.str();
    }

    static std::unordered_map<std::string, std::pair<std::string, nlohmann::json>> get_builtin_presets() {
        std::unordered_map<std::string, std::pair<std::string, nlohmann::json>> p;

        p["gaming"] = {"Gaming & Squads server with LFG, clips, and voice rooms", {
            {"roles", {
                {{"name", "Admin"}, {"colour", "#E74C3C"}, {"rank", 0}},
                {{"name", "Moderator"}, {"colour", "#3498DB"}, {"rank", 1}},
                {{"name", "VIP"}, {"colour", "#F1C40F"}, {"rank", 2}},
                {{"name", "Gamer"}, {"colour", "#2ECC71"}, {"rank", 3}}
            }},
            {"categories", {
                {{"title", "Information"}, {"channels", {"welcome", "rules", "announcements"}}},
                {{"title", "Community"}, {"channels", {"general", "clips-and-media", "looking-for-group", "memes"}}},
                {{"title", "Voice"}, {"channels", {"Lobby", "Squad 1", "Squad 2"}}}
            }},
            {"channels", {
                {{"name", "welcome"}, {"channel_type", "TextChannel"}, {"category", "Information"}},
                {{"name", "rules"}, {"channel_type", "TextChannel"}, {"category", "Information"}},
                {{"name", "announcements"}, {"channel_type", "TextChannel"}, {"category", "Information"}},
                {{"name", "general"}, {"channel_type", "TextChannel"}, {"category", "Community"}},
                {{"name", "clips-and-media"}, {"channel_type", "TextChannel"}, {"category", "Community"}},
                {{"name", "looking-for-group"}, {"channel_type", "TextChannel"}, {"category", "Community"}},
                {{"name", "memes"}, {"channel_type", "TextChannel"}, {"category", "Community"}},
                {{"name", "Lobby"}, {"channel_type", "VoiceChannel"}, {"category", "Voice"}},
                {{"name", "Squad 1"}, {"channel_type", "VoiceChannel"}, {"category", "Voice"}},
                {{"name", "Squad 2"}, {"channel_type", "VoiceChannel"}, {"category", "Voice"}}
            }}
        }};

        p["community"] = {"Chill community server with events, art, and hangout channels", {
            {"roles", {
                {{"name", "Admin"}, {"colour", "#E74C3C"}, {"rank", 0}},
                {{"name", "Mod"}, {"colour", "#9B59B6"}, {"rank", 1}},
                {{"name", "Helper"}, {"colour", "#1ABC9C"}, {"rank", 2}},
                {{"name", "Member"}, {"colour", "#95A5A6"}, {"rank", 3}}
            }},
            {"categories", {
                {{"title", "Welcome"}, {"channels", {"rules", "announcements", "introductions"}}},
                {{"title", "Lounge"}, {"channels", {"general", "media", "art-and-creations", "bot-commands"}}},
                {{"title", "Voice"}, {"channels", {"General VC", "Music"}}}
            }},
            {"channels", {
                {{"name", "rules"}, {"channel_type", "TextChannel"}, {"category", "Welcome"}},
                {{"name", "announcements"}, {"channel_type", "TextChannel"}, {"category", "Welcome"}},
                {{"name", "introductions"}, {"channel_type", "TextChannel"}, {"category", "Welcome"}},
                {{"name", "general"}, {"channel_type", "TextChannel"}, {"category", "Lounge"}},
                {{"name", "media"}, {"channel_type", "TextChannel"}, {"category", "Lounge"}},
                {{"name", "art-and-creations"}, {"channel_type", "TextChannel"}, {"category", "Lounge"}},
                {{"name", "bot-commands"}, {"channel_type", "TextChannel"}, {"category", "Lounge"}},
                {{"name", "General VC"}, {"channel_type", "VoiceChannel"}, {"category", "Voice"}},
                {{"name", "Music"}, {"channel_type", "VoiceChannel"}, {"category", "Voice"}}
            }}
        }};

        p["dev"] = {"Developer & coding hub with help channels and showcase", {
            {"roles", {
                {{"name", "Maintainer"}, {"colour", "#E67E22"}, {"rank", 0}},
                {{"name", "Contributor"}, {"colour", "#2ECC71"}, {"rank", 1}},
                {{"name", "Developer"}, {"colour", "#3498DB"}, {"rank", 2}}
            }},
            {"categories", {
                {{"title", "Project"}, {"channels", {"announcements", "changelog", "resources"}}},
                {{"title", "Code & Help"}, {"channels", {"general", "cpp-and-rust", "python-and-js", "showcase"}}},
                {{"title", "Voice"}, {"channels", {"Pair Programming", "Standup"}}}
            }},
            {"channels", {
                {{"name", "announcements"}, {"channel_type", "TextChannel"}, {"category", "Project"}},
                {{"name", "changelog"}, {"channel_type", "TextChannel"}, {"category", "Project"}},
                {{"name", "resources"}, {"channel_type", "TextChannel"}, {"category", "Project"}},
                {{"name", "general"}, {"channel_type", "TextChannel"}, {"category", "Code & Help"}},
                {{"name", "cpp-and-rust"}, {"channel_type", "TextChannel"}, {"category", "Code & Help"}},
                {{"name", "python-and-js"}, {"channel_type", "TextChannel"}, {"category", "Code & Help"}},
                {{"name", "showcase"}, {"channel_type", "TextChannel"}, {"category", "Code & Help"}},
                {{"name", "Pair Programming"}, {"channel_type", "VoiceChannel"}, {"category", "Voice"}},
                {{"name", "Standup"}, {"channel_type", "VoiceChannel"}, {"category", "Voice"}}
            }}
        }};

        p["study"] = {"Study group server with subject channels and quiet rooms", {
            {"roles", {
                {{"name", "Tutor"}, {"colour", "#1ABC9C"}, {"rank", 0}},
                {{"name", "Scholar"}, {"colour", "#3498DB"}, {"rank", 1}},
                {{"name", "Student"}, {"colour", "#95A5A6"}, {"rank", 2}}
            }},
            {"categories", {
                {{"title", "Info"}, {"channels", {"rules", "study-resources"}}},
                {{"title", "Subjects"}, {"channels", {"general", "math", "science", "computer-science"}}},
                {{"title", "Study Rooms"}, {"channels", {"Quiet Study", "Group Study"}}}
            }},
            {"channels", {
                {{"name", "rules"}, {"channel_type", "TextChannel"}, {"category", "Info"}},
                {{"name", "study-resources"}, {"channel_type", "TextChannel"}, {"category", "Info"}},
                {{"name", "general"}, {"channel_type", "TextChannel"}, {"category", "Subjects"}},
                {{"name", "math"}, {"channel_type", "TextChannel"}, {"category", "Subjects"}},
                {{"name", "science"}, {"channel_type", "TextChannel"}, {"category", "Subjects"}},
                {{"name", "computer-science"}, {"channel_type", "TextChannel"}, {"category", "Subjects"}},
                {{"name", "Quiet Study"}, {"channel_type", "VoiceChannel"}, {"category", "Study Rooms"}},
                {{"name", "Group Study"}, {"channel_type", "VoiceChannel"}, {"category", "Study Rooms"}}
            }}
        }};

        p["minimal"] = {"Clean minimal server layout", {
            {"roles", {
                {{"name", "Admin"}, {"colour", "#E74C3C"}, {"rank", 0}},
                {{"name", "Member"}, {"colour", "#95A5A6"}, {"rank", 1}}
            }},
            {"categories", {
                {{"title", "Server"}, {"channels", {"general", "off-topic", "Voice"}}}
            }},
            {"channels", {
                {{"name", "general"}, {"channel_type", "TextChannel"}, {"category", "Server"}},
                {{"name", "off-topic"}, {"channel_type", "TextChannel"}, {"category", "Server"}},
                {{"name", "Voice"}, {"channel_type", "VoiceChannel"}, {"category", "Server"}}
            }}
        }};

        return p;
    }

    void register_handlers(stoatpp::cluster& bot) override {
        instance = this;
        register_commands(bot);

        // Background auto-backup scheduler
        running_ = true;
        auto_thread_ = std::thread([this, &bot]() {
            while (running_) {
                for (int i = 0; i < 60 && running_; ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(15));
                }
                if (!running_ || !bronx::g_db) continue;
                uint64_t now_ts = static_cast<uint64_t>(std::time(nullptr));
                auto due = bronx::g_db->get_due_auto_backups("stoat", now_ts);
                for (const auto& cfg : due) {
                    nlohmann::json snap = snapshot_server_json(bot, cfg.server_id);
                    if (!snap.is_object() || snap.empty()) continue;
                    bronx::ServerBackup b;
                    b.backup_id = generate_backup_id();
                    b.server_id = cfg.server_id;
                    b.creator_id = "auto";
                    b.platform = "stoat";
                    b.name = "Auto Backup";
                    b.data_json = snap.dump();
                    b.is_auto = true;
                    b.created_at = now_ts;
                    if (bronx::g_db->save_server_backup(b)) {
                        bronx::g_db->update_auto_backup_last_run(cfg.server_id, now_ts);
                    }
                }
            }
        });
    }

    void register_commands(stoatpp::cluster& bot) {
        // .backup command
        bot.register_command({
            "backup",
            {"bk", "backups"},
            "Xenon-style server backups: create, list, info, load, delete, auto.",
            "backup <create|list|info|load|delete|auto> [args]",
            {"<subcommand>", "[args]"},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (msg.server_id.empty()) {
                    bot.send_message(msg.channel_id, bronx::emj_no + " This command can only be used in a server.");
                    return;
                }
                if (!bronx::g_db) {
                    bot.send_message(msg.channel_id, bronx::emj_warning + " Database is unavailable.");
                    return;
                }

                if (args.empty()) {
                    bot.send_message(msg.channel_id,
                        "💾 **Server Backup Commands (Xenon-style)**\n"
                        "• `.backup create [name]` — Save a full snapshot of roles, categories, channels, permissions & bans\n"
                        "• `.backup list` — List saved backups for this server & your account\n"
                        "• `.backup info <id>` — Preview a backup's contents\n"
                        "• `.backup load <id> [clear]` — Restore a backup onto this server (cross-platform!)\n"
                        "• `.backup delete <id>` — Delete a backup\n"
                        "• `.backup auto [on|off|<hours>]` — Configure automatic scheduled backups");
                    return;
                }

                std::string sub = args[0];
                std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);

                bot.fetch_member(msg.server_id, msg.author.id, [this, &bot, msg, args, sub](stoatpp::models::Member member, bool ok) {
                    if (!ok || !is_admin_or_owner(bot, msg.server_id, msg.author.id, member)) {
                        bot.send_message(msg.channel_id, bronx::emj_warning + " You must be an administrator or server owner to manage backups.");
                        return;
                    }

                    if (sub == "create") {
                        std::string name = "Server Backup";
                        if (args.size() > 1) {
                            name.clear();
                            for (size_t i = 1; i < args.size(); ++i) {
                                if (i > 1) name += " ";
                                name += args[i];
                            }
                        }
                        bot.send_message(msg.channel_id, "💾 Creating server backup...");
                        std::thread([&bot, msg, name]() {
                            nlohmann::json snap = snapshot_server_json(bot, msg.server_id);
                            if (!snap.is_object() || snap.empty()) {
                                bot.send_message(msg.channel_id, bronx::emj_no + " Failed to snapshot server.");
                                return;
                            }
                            bronx::ServerBackup b;
                            b.backup_id = generate_backup_id();
                            b.server_id = msg.server_id;
                            b.creator_id = msg.author.id;
                            b.platform = "stoat";
                            b.name = name;
                            b.data_json = snap.dump();
                            b.is_auto = false;
                            b.created_at = static_cast<uint64_t>(std::time(nullptr));

                            if (bronx::g_db->save_server_backup(b)) {
                                size_t rcount = snap.contains("roles") ? snap["roles"].size() : 0;
                                size_t ccount = snap.contains("channels") ? snap["channels"].size() : 0;
                                size_t catcount = snap.contains("categories") ? snap["categories"].size() : 0;
                                bot.send_message(msg.channel_id,
                                    "✅ **Backup Created!**\n"
                                    "• **ID:** `" + b.backup_id + "`\n"
                                    "• **Name:** " + b.name + "\n"
                                    "• **Captured:** " + std::to_string(rcount) + " roles, " +
                                    std::to_string(catcount) + " categories, " + std::to_string(ccount) + " channels.\n"
                                    "Use `.backup load " + b.backup_id + "` on any server (even across platforms!) to restore.");
                            } else {
                                bot.send_message(msg.channel_id, bronx::emj_no + " Failed to save backup to database.");
                            }
                        }).detach();
                    } else if (sub == "list") {
                        auto list = bronx::g_db->list_server_backups(msg.server_id, msg.author.id);
                        if (list.empty()) {
                            bot.send_message(msg.channel_id, "ℹ️ No backups found. Run `.backup create` to make one!");
                            return;
                        }
                        std::string out = "💾 **Saved Backups**\n";
                        for (const auto& b : list) {
                            out += "• `" + b.backup_id + "` — **" + b.name + "** (" + b.platform +
                                   (b.is_auto ? ", auto" : "") + ")\n";
                        }
                        bot.send_message(msg.channel_id, out);
                    } else if (sub == "info" && args.size() >= 2) {
                        auto b_opt = bronx::g_db->get_server_backup(args[1]);
                        if (!b_opt) {
                            bot.send_message(msg.channel_id, bronx::emj_no + " Backup `" + args[1] + "` not found.");
                            return;
                        }
                        nlohmann::json snap = nlohmann::json::parse(b_opt->data_json, nullptr, false);
                        size_t rcount = snap.contains("roles") ? snap["roles"].size() : 0;
                        size_t ccount = snap.contains("channels") ? snap["channels"].size() : 0;
                        size_t catcount = snap.contains("categories") ? snap["categories"].size() : 0;
                        size_t bcount = snap.contains("bans") ? snap["bans"].size() : 0;
                        std::string out = "💾 **Backup Info: `" + b_opt->backup_id + "`**\n"
                                          "• **Name:** " + b_opt->name + "\n"
                                          "• **Origin Platform:** " + b_opt->platform + "\n"
                                          "• **Roles:** " + std::to_string(rcount) + "\n"
                                          "• **Categories:** " + std::to_string(catcount) + "\n"
                                          "• **Channels:** " + std::to_string(ccount) + "\n"
                                          "• **Bans:** " + std::to_string(bcount) + "\n";
                        bot.send_message(msg.channel_id, out);
                    } else if (sub == "load" && args.size() >= 2) {
                        auto b_opt = bronx::g_db->get_server_backup(args[1]);
                        if (!b_opt) {
                            bot.send_message(msg.channel_id, bronx::emj_no + " Backup `" + args[1] + "` not found.");
                            return;
                        }
                        bool clear_existing = (args.size() >= 3 && (args[2] == "clear" || args[2] == "--clear"));
                        nlohmann::json snap = nlohmann::json::parse(b_opt->data_json, nullptr, false);
                        if (!snap.is_object()) {
                            bot.send_message(msg.channel_id, bronx::emj_no + " Backup data is corrupted.");
                            return;
                        }
                        bot.send_message(msg.channel_id, "⏳ Loading backup `" + b_opt->backup_id + "`" +
                            (clear_existing ? " (clearing existing channels & roles)..." : " (merging into server)..."));
                        std::thread([&bot, msg, snap, clear_existing]() {
                            std::string res = apply_server_json(bot, msg.server_id, snap, clear_existing, msg.channel_id);
                            bot.send_message(msg.channel_id, res);
                        }).detach();
                    } else if (sub == "delete" && args.size() >= 2) {
                        if (bronx::g_db->delete_server_backup(args[1])) {
                            bot.send_message(msg.channel_id, "🗑️ Deleted backup `" + args[1] + "`.");
                        } else {
                            bot.send_message(msg.channel_id, bronx::emj_no + " Backup `" + args[1] + "` not found.");
                        }
                    } else if (sub == "auto") {
                        auto cur = bronx::g_db->get_auto_backup_config(msg.server_id);
                        if (args.size() < 2) {
                            if (cur && cur->enabled) {
                                bot.send_message(msg.channel_id, "⏱️ Auto-backups are **ON** (every **" + std::to_string(cur->interval_hours) + "h**).");
                            } else {
                                bot.send_message(msg.channel_id, "⏱️ Auto-backups are **OFF**. Use `.backup auto on` or `.backup auto 24`.");
                            }
                            return;
                        }
                        std::string arg1 = args[1];
                        std::transform(arg1.begin(), arg1.end(), arg1.begin(), ::tolower);
                        bronx::ServerAutoBackupConfig cfg;
                        cfg.server_id = msg.server_id;
                        cfg.platform = "stoat";
                        cfg.last_backup_at = cur ? cur->last_backup_at : 0;
                        if (arg1 == "off" || arg1 == "disable") {
                            cfg.enabled = false;
                            cfg.interval_hours = cur ? cur->interval_hours : 24;
                            bronx::g_db->set_auto_backup_config(cfg);
                            bot.send_message(msg.channel_id, "⏱️ Auto-backups **disabled**.");
                        } else {
                            int hours = 24;
                            if (arg1 != "on" && arg1 != "enable") {
                                try { hours = std::max(1, std::min(168, std::stoi(arg1))); } catch (...) {}
                            } else if (cur && cur->interval_hours > 0) {
                                hours = cur->interval_hours;
                            }
                            cfg.enabled = true;
                            cfg.interval_hours = hours;
                            bronx::g_db->set_auto_backup_config(cfg);
                            bot.send_message(msg.channel_id, "✅ Auto-backups **enabled** (every **" + std::to_string(hours) + "h**).");
                        }
                    } else {
                        bot.send_message(msg.channel_id, bronx::emj_no + " Invalid subcommand. Run `.backup` for help.");
                    }
                });
            },
            "Utility"
        });

        // .preset command
        bot.register_command({
            "preset",
            {"template", "templates", "presets"},
            "Xenon-style server presets/templates: list, info, load, save, delete.",
            "preset <list|info|load|save|delete> [name]",
            {"<subcommand>", "[args]"},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (msg.server_id.empty()) {
                    bot.send_message(msg.channel_id, bronx::emj_no + " This command can only be used in a server.");
                    return;
                }
                if (args.empty()) {
                    bot.send_message(msg.channel_id,
                        "📋 **Server Presets & Templates**\n"
                        "• `.preset list` — Browse built-in & community presets\n"
                        "• `.preset info <name>` — Preview a preset's roles & channels\n"
                        "• `.preset load <name> [clear]` — Apply a preset to this server\n"
                        "• `.preset save <name> [description]` — Save this server as a reusable preset\n"
                        "• `.preset delete <name>` — Delete a custom preset");
                    return;
                }

                std::string sub = args[0];
                std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);
                auto builtins = get_builtin_presets();

                if (sub == "list") {
                    std::string out = "📋 **Built-in Server Presets**\n";
                    for (const auto& [name, pair] : builtins) {
                        out += "• `" + name + "` — " + pair.first + "\n";
                    }
                    if (bronx::g_db) {
                        auto custom = bronx::g_db->list_server_presets();
                        if (!custom.empty()) {
                            out += "\n🌐 **Saved Presets**\n";
                            for (const auto& p : custom) {
                                out += "• `" + p.preset_name + "`" + (p.description.empty() ? "" : " — " + p.description) + "\n";
                            }
                        }
                    }
                    bot.send_message(msg.channel_id, out);
                    return;
                }

                if (sub == "info" && args.size() >= 2) {
                    std::string name = args[1];
                    std::string low = name;
                    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                    nlohmann::json snap;
                    std::string desc;
                    if (builtins.count(low)) {
                        desc = builtins[low].first;
                        snap = builtins[low].second;
                    } else if (bronx::g_db) {
                        auto p_opt = bronx::g_db->get_server_preset(name);
                        if (p_opt) {
                            desc = p_opt->description;
                            snap = nlohmann::json::parse(p_opt->data_json, nullptr, false);
                        }
                    }
                    if (!snap.is_object()) {
                        bot.send_message(msg.channel_id, bronx::emj_no + " Preset `" + name + "` not found.");
                        return;
                    }
                    std::string out = "📋 **Preset: `" + name + "`**\n";
                    if (!desc.empty()) out += desc + "\n\n";
                    out += "**Roles:** ";
                    if (snap.contains("roles")) {
                        for (size_t i = 0; i < snap["roles"].size(); ++i) {
                            if (i > 0) out += ", ";
                            out += snap["roles"][i].value("name", "");
                        }
                    }
                    out += "\n**Channels:** ";
                    if (snap.contains("channels")) {
                        for (size_t i = 0; i < snap["channels"].size(); ++i) {
                            if (i > 0) out += ", ";
                            out += "#" + snap["channels"][i].value("name", "");
                        }
                    }
                    bot.send_message(msg.channel_id, out);
                    return;
                }

                bot.fetch_member(msg.server_id, msg.author.id, [this, &bot, msg, args, sub, builtins](stoatpp::models::Member member, bool ok) {
                    if (!ok || !is_admin_or_owner(bot, msg.server_id, msg.author.id, member)) {
                        bot.send_message(msg.channel_id, bronx::emj_warning + " You must be an administrator or server owner to manage presets.");
                        return;
                    }

                    if (sub == "load" && args.size() >= 2) {
                        std::string name = args[1];
                        std::string low = name;
                        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                        bool clear_existing = (args.size() >= 3 && (args[2] == "clear" || args[2] == "--clear"));
                        nlohmann::json snap;
                        if (builtins.count(low)) {
                            snap = builtins.at(low).second;
                        } else if (bronx::g_db) {
                            auto p_opt = bronx::g_db->get_server_preset(name);
                            if (p_opt) snap = nlohmann::json::parse(p_opt->data_json, nullptr, false);
                        }
                        if (!snap.is_object()) {
                            bot.send_message(msg.channel_id, bronx::emj_no + " Preset `" + name + "` not found.");
                            return;
                        }
                        bot.send_message(msg.channel_id, "⏳ Applying preset `" + name + "`...");
                        std::thread([&bot, msg, snap, clear_existing]() {
                            std::string res = apply_server_json(bot, msg.server_id, snap, clear_existing, msg.channel_id);
                            bot.send_message(msg.channel_id, res);
                        }).detach();
                    } else if (sub == "save" && args.size() >= 2) {
                        if (!bronx::g_db) {
                            bot.send_message(msg.channel_id, bronx::emj_warning + " Database is unavailable.");
                            return;
                        }
                        std::string name = args[1];
                        std::string desc;
                        for (size_t i = 2; i < args.size(); ++i) {
                            if (i > 2) desc += " ";
                            desc += args[i];
                        }
                        std::thread([&bot, msg, name, desc]() {
                            nlohmann::json snap = snapshot_server_json(bot, msg.server_id);
                            if (!snap.is_object() || snap.empty()) {
                                bot.send_message(msg.channel_id, bronx::emj_no + " Failed to snapshot server.");
                                return;
                            }
                            bronx::ServerPreset p;
                            p.preset_name = name;
                            p.creator_id = msg.author.id;
                            p.description = desc;
                            p.data_json = snap.dump();
                            p.is_public = true;
                            p.created_at = static_cast<uint64_t>(std::time(nullptr));
                            if (bronx::g_db->save_server_preset(p)) {
                                bot.send_message(msg.channel_id, "✅ Saved preset **`" + name + "`**! Anyone can load it with `.preset load " + name + "`.");
                            } else {
                                bot.send_message(msg.channel_id, bronx::emj_no + " Failed to save preset.");
                            }
                        }).detach();
                    } else if (sub == "delete" && args.size() >= 2) {
                        if (!bronx::g_db) return;
                        if (bronx::g_db->delete_server_preset(args[1])) {
                            bot.send_message(msg.channel_id, "🗑️ Deleted preset `" + args[1] + "`.");
                        } else {
                            bot.send_message(msg.channel_id, bronx::emj_no + " Preset `" + args[1] + "` not found.");
                        }
                    } else {
                        bot.send_message(msg.channel_id, bronx::emj_no + " Invalid subcommand. Run `.preset` for help.");
                    }
                });
            },
            "Utility"
        });
    }

private:
    std::atomic<bool> running_{false};
    std::thread auto_thread_;
};

inline BackupModule* BackupModule::instance = nullptr;

} // namespace bronx::cogs
