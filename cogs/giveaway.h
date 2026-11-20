#pragma once
#include "stoatpp/bot_module.h"
#include "helpers.h"
#include "../database.h"
#include <chrono>
#include <thread>
#include <random>
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace bronx::cogs {

class GiveawayModule : public stoatpp::bot_module {
public:
    void register_handlers(stoatpp::cluster& bot) override {

        // --- .gstart <time> <winners> <prize...> [--role <roles>] [--level <N>] [--messages <N>[/days]] ---
        bot.register_command({
            "gstart",
            {"giveaway", "gcreate"},
            "start a giveaway with optional role, level, or message requirements.",
            "<time> <winners> <prize> [--role <roles>] [--level <N>] [--messages <N>[/days]]",
            {"<time>", "<winners>", "<prize>"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (msg.server_id.empty()) {
                    bot.send_message(msg.channel_id, "this command can only be used inside a server.");
                    return;
                }
                if (args.size() < 3) {
                    bot.send_message(msg.channel_id, 
                        "usage: `.gstart <time> <winners> <prize...> [--role <role1,role2>] [--level <N>] [--messages <count>[/days]]`\n"
                        "Examples:\n"
                        "• `.gstart 2h 1w Nitro Basic`\n"
                        "• `.gstart 24h 2 VIP Pass --role @Active,@Supporter --level 5`\n"
                        "• `.gstart 7d 1 10,000 Coins --messages 50/7d`");
                    return;
                }

                uint64_t dur_sec = helpers::parse_duration(args[0]);
                if (dur_sec == 0) {
                    bot.send_message(msg.channel_id, "" + bronx::emj_warning + " invalid duration (e.g. `10m`, `1h`, `1d`, `7d`).");
                    return;
                }

                int winners = 1;
                std::string win_str = args[1];
                if (!win_str.empty() && (win_str.back() == 'w' || win_str.back() == 'W')) {
                    win_str.pop_back();
                }
                try {
                    winners = std::stoi(win_str);
                    if (winners < 1) winners = 1;
                    if (winners > 50) winners = 50;
                } catch (...) {
                    bot.send_message(msg.channel_id, "" + bronx::emj_warning + " invalid number of winners.");
                    return;
                }

                std::vector<std::string> req_role_inputs;
                int req_min_level = 0;
                int req_min_messages = 0;
                int req_messages_days = 7;
                std::string prize;

                for (size_t i = 2; i < args.size(); ++i) {
                    const std::string& a = args[i];
                    if ((a == "--role" || a == "-r") && i + 1 < args.size()) {
                        std::string raw_roles = args[++i];
                        std::stringstream rss(raw_roles);
                        std::string part;
                        while (std::getline(rss, part, ',')) {
                            part.erase(0, part.find_first_not_of(" \t\r\n"));
                            part.erase(part.find_last_not_of(" \t\r\n") + 1);
                            if (!part.empty()) req_role_inputs.push_back(part);
                        }
                    } else if ((a == "--level" || a == "-l") && i + 1 < args.size()) {
                        try { req_min_level = std::stoi(args[++i]); } catch (...) {}
                    } else if ((a == "--messages" || a == "--msgs" || a == "-m") && i + 1 < args.size()) {
                        std::string mspec = args[++i];
                        auto slash = mspec.find('/');
                        if (slash != std::string::npos) {
                            try {
                                req_min_messages = std::stoi(mspec.substr(0, slash));
                                std::string d_str = mspec.substr(slash + 1);
                                if (!d_str.empty() && (d_str.back() == 'd' || d_str.back() == 'D')) d_str.pop_back();
                                req_messages_days = std::stoi(d_str);
                                if (req_messages_days < 1) req_messages_days = 1;
                            } catch (...) {}
                        } else {
                            try { req_min_messages = std::stoi(mspec); } catch (...) {}
                        }
                    } else {
                        prize += a + " ";
                    }
                }
                while (!prize.empty() && prize.back() == ' ') prize.pop_back();

                if (prize.empty()) {
                    bot.send_message(msg.channel_id, "" + bronx::emj_warning + " please specify a prize.");
                    return;
                }

                auto srv_opt = bot.get_server(msg.server_id);
                std::vector<std::pair<std::string, std::string>> matched_roles;
                std::unordered_set<std::string> seen_role_ids;

                if (!req_role_inputs.empty()) {
                    if (!srv_opt) {
                        bot.send_message(msg.channel_id, "" + bronx::emj_warning + " could not resolve server roles.");
                        return;
                    }
                    auto to_lower = [](std::string s) {
                        for (auto& c : s) c = std::tolower(c);
                        return s;
                    };

                    for (const auto& req_role_input : req_role_inputs) {
                        std::string cleaned_id = helpers::clean_role_id(req_role_input);
                        std::string lquery = to_lower(req_role_input);
                        bool found = false;
                        for (const auto& r : srv_opt->roles) {
                            if (r.id == cleaned_id || to_lower(r.name) == lquery) {
                                if (seen_role_ids.insert(r.id).second) {
                                    matched_roles.push_back({r.id, r.name});
                                }
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            bot.send_message(msg.channel_id, "" + bronx::emj_warning + " required role `" + req_role_input + "` not found in this server.");
                            return;
                        }
                    }
                }

                std::string matched_roles_csv;
                for (size_t i = 0; i < matched_roles.size(); ++i) {
                    matched_roles_csv += matched_roles[i].first + (i + 1 < matched_roles.size() ? "," : "");
                }

                auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                uint64_t end_time = now_unix + dur_sec;

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🎉 **GIVEAWAY** 🎉");
                embed.set_colour("#5865F2");

                std::string desc = "**Prize:** " + prize + "\n\n"
                                   "• React with 🎉 to enter!\n"
                                   "• **Winners:** " + std::to_string(winners) + "\n"
                                   "• **Hosted by:** <@" + msg.author.id + ">\n"
                                   "• **Ends:** <t:" + std::to_string(end_time) + ":R> (<t:" + std::to_string(end_time) + ":F>)\n";

                if (!matched_roles.empty() || req_min_level > 0 || req_min_messages > 0) {
                    desc += "\n**Entry Requirements:**\n";
                    if (!matched_roles.empty()) {
                        desc += "• **Role(s):** ";
                        for (size_t i = 0; i < matched_roles.size(); ++i) {
                            desc += "<@&" + matched_roles[i].first + "> (" + matched_roles[i].second + ")" +
                                    (i + 1 < matched_roles.size() ? ", " : "");
                        }
                        desc += "\n";
                    }
                    if (req_min_level > 0) {
                        desc += "• **Minimum Level:** Level " + std::to_string(req_min_level) + "+\n";
                    }
                    if (req_min_messages > 0) {
                        desc += "• **Activity:** " + std::to_string(req_min_messages) + "+ messages in last " + std::to_string(req_messages_days) + " days\n";
                    }
                }

                embed.set_description(desc);
                payload.add_embed(embed);

                payload.set_interactions({"🎉"}, false);

                auto res = bot.rest().create_message(msg.channel_id, payload);
                if (!res.success()) {
                    bot.send_message(msg.channel_id, "" + bronx::emj_warning + " failed to create giveaway message.");
                    return;
                }

                std::string giveaway_msg_id = res.body.value("_id", res.body.value("id", ""));

                if (!giveaway_msg_id.empty() && g_db) {
                    GiveawayEntry g;
                    g.message_id = giveaway_msg_id;
                    g.channel_id = msg.channel_id;
                    g.server_id = msg.server_id;
                    g.host_id = msg.author.id;
                    g.prize = prize;
                    g.winners_count = winners;
                    g.end_time = end_time;
                    g.ended = false;
                    g.required_role_id = matched_roles_csv;
                    g.min_level = req_min_level;
                    g.min_messages = req_min_messages;
                    g.messages_days = req_messages_days;
                    g_db->create_giveaway(g);
                }
            },
            "Utility",
            stoatpp::permissions::ManageMessages
        });

        // --- .gend <message_id> ---
        bot.register_command({
            "gend",
            {"giveawayend"},
            "immediately end an active giveaway and pick winners.",
            "<message_id>",
            {"<message_id>"},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (args.empty()) {
                    bot.send_message(msg.channel_id, "usage: `.gend <message_id>`");
                    return;
                }
                std::string mid = args[0];
                if (!g_db) return;
                auto g_opt = g_db->get_giveaway(mid);
                if (!g_opt || g_opt->ended || (!msg.server_id.empty() && g_opt->server_id != msg.server_id)) {
                    bot.send_message(msg.channel_id, "" + bronx::emj_warning + " active giveaway not found for that message ID.");
                    return;
                }
                roll_giveaway(bot, *g_opt, true);
            },
            "Utility",
            stoatpp::permissions::ManageMessages
        });

        // --- .greroll <message_id> [winners] ---
        bot.register_command({
            "greroll",
            {"reroll"},
            "reroll new winner(s) for a finished giveaway.",
            "<message_id> [winners]",
            {"<message_id>", "[winners]"},
            [this](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (args.empty()) {
                    bot.send_message(msg.channel_id, "usage: `.greroll <message_id> [winners]`");
                    return;
                }
                std::string mid = args[0];
                if (!g_db) return;
                auto g_opt = g_db->get_giveaway(mid);
                if (!g_opt || (!msg.server_id.empty() && g_opt->server_id != msg.server_id)) {
                    bot.send_message(msg.channel_id, "" + bronx::emj_warning + " giveaway not found.");
                    return;
                }
                int count = g_opt->winners_count;
                if (args.size() > 1) {
                    try {
                        count = std::stoi(args[1]);
                        if (count < 1) count = 1;
                    } catch (...) {}
                }
                auto g = *g_opt;
                g.winners_count = count;
                roll_giveaway(bot, g, false);
            },
            "Utility",
            stoatpp::permissions::ManageMessages
        });

        // --- .glist ---
        bot.register_command({
            "glist",
            {"giveaways"},
            "list all active giveaways in this server.",
            "",
            {},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>&) {
                if (msg.server_id.empty()) {
                    bot.send_message(msg.channel_id, "this command can only be used inside a server.");
                    return;
                }
                if (!g_db) {
                    bot.send_message(msg.channel_id, "database unavailable.");
                    return;
                }

                auto active = g_db->get_active_giveaways(msg.server_id);
                if (active.empty()) {
                    bot.send_message(msg.channel_id, "ℹ️ No active giveaways running in this server.");
                    return;
                }

                stoatpp::models::MessagePayload payload;
                stoatpp::models::Embed embed;
                embed.set_title("🎉 Active Giveaways (" + std::to_string(active.size()) + ")");
                embed.set_colour("#5865F2");

                std::string desc;
                for (size_t i = 0; i < active.size(); ++i) {
                    const auto& g = active[i];
                    desc += "**" + std::to_string(i + 1) + ". " + g.prize + "** (" + std::to_string(g.winners_count) + " winner" + (g.winners_count > 1 ? "s" : "") + ")\n";
                    desc += "• **Channel:** <#" + g.channel_id + "> | **ID:** `" + g.message_id + "`\n";
                    desc += "• **Ends:** <t:" + std::to_string(g.end_time) + ":R> (<t:" + std::to_string(g.end_time) + ":F>)\n";

                    std::string reqs = format_requirements(g);
                    if (!reqs.empty()) {
                        desc += "• **Requirements:** " + reqs + "\n";
                    }
                    desc += "\n";

                    if (desc.size() > 3500) {
                        desc += "... *(more truncated)*\n";
                        break;
                    }
                }
                embed.set_description(desc);
                payload.add_embed(embed);
                bot.send_message(msg.channel_id, payload);
            },
            "Utility"
        });

        // --- .gcancel <message_id> ---
        bot.register_command({
            "gcancel",
            {"gdelete"},
            "cancel and delete an active giveaway without rolling winners.",
            "<message_id>",
            {"<message_id>"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                if (msg.server_id.empty()) {
                    bot.send_message(msg.channel_id, "this command can only be used inside a server.");
                    return;
                }
                if (args.empty()) {
                    bot.send_message(msg.channel_id, "usage: `.gcancel <message_id>`");
                    return;
                }
                if (!g_db) return;

                std::string mid = args[0];
                auto g_opt = g_db->get_giveaway(mid);
                if (!g_opt || g_opt->server_id != msg.server_id) {
                    bot.send_message(msg.channel_id, "" + bronx::emj_warning + " giveaway not found in this server.");
                    return;
                }

                if (!g_db->delete_giveaway(msg.server_id, mid)) {
                    bot.send_message(msg.channel_id, "" + bronx::emj_warning + " failed to cancel giveaway.");
                    return;
                }

                auto g = *g_opt;
                std::thread([&bot, msg, g]() {
                    stoatpp::models::MessagePayload edit_payload;
                    stoatpp::models::Embed embed;
                    embed.set_title("🚫 **GIVEAWAY CANCELLED** 🚫");
                    embed.set_colour("#ED4245");
                    embed.set_description(
                        "**Prize:** " + g.prize + "\n\n"
                        "This giveaway was cancelled by <@" + msg.author.id + ">.\n"
                        "• **Hosted by:** <@" + g.host_id + ">"
                    );
                    edit_payload.add_embed(embed);
                    bot.rest().edit_message(g.channel_id, g.message_id, edit_payload.to_json());
                    bot.send_message(msg.channel_id, "" + bronx::emj_yes + " Cancelled giveaway for **" + g.prize + "** (`" + g.message_id + "`).");
                }).detach();
            },
            "Utility",
            stoatpp::permissions::ManageMessages
        });

        // --- Background Giveaway Ticker ---
        std::thread([this, &bot]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!g_db) continue;
                auto now = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                auto due = g_db->get_due_giveaways(now);
                for (const auto& g : due) {
                    roll_giveaway(bot, g, true);
                }
            }
        }).detach();
    }

private:
    static std::vector<std::string> split_csv(const std::string& csv) {
        std::vector<std::string> out;
        std::stringstream ss(csv);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item.erase(0, item.find_first_not_of(" \t\r\n"));
            item.erase(item.find_last_not_of(" \t\r\n") + 1);
            if (!item.empty()) out.push_back(item);
        }
        return out;
    }

    static std::string format_requirements(const GiveawayEntry& g) {
        std::vector<std::string> parts;
        auto req_roles = split_csv(g.required_role_id);
        if (!req_roles.empty()) {
            std::string rs = "Role(s): ";
            for (size_t i = 0; i < req_roles.size(); ++i) {
                rs += "<@&" + req_roles[i] + ">" + (i + 1 < req_roles.size() ? ", " : "");
            }
            parts.push_back(rs);
        }
        if (g.min_level > 0) {
            parts.push_back("Level " + std::to_string(g.min_level) + "+");
        }
        if (g.min_messages > 0) {
            parts.push_back(std::to_string(g.min_messages) + "+ msgs / " + std::to_string(g.messages_days) + "d");
        }
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i) {
            out += parts[i] + (i + 1 < parts.size() ? " · " : "");
        }
        return out;
    }

    void roll_giveaway(stoatpp::cluster& bot, const GiveawayEntry& g, bool mark_ended) {
        if (mark_ended && g_db) {
            g_db->end_giveaway(g.message_id);
        }

        auto res = bot.rest().get_message(g.channel_id, g.message_id);
        if (!res.success()) return;

        auto raw = res.body;
        std::vector<std::string> entrants;

        if (raw.contains("reactions") && raw["reactions"].is_object()) {
            auto rxns = raw["reactions"];
            if (rxns.contains("🎉") && rxns["🎉"].is_array()) {
                for (const auto& u : rxns["🎉"]) {
                    if (u.is_string()) {
                        std::string uid = u;
                        if (uid != bot.current_user().id) {
                            entrants.push_back(uid);
                        }
                    }
                }
            }
        }

        stoatpp::models::MessagePayload edit_payload;
        stoatpp::models::Embed embed;
        embed.set_title("🎉 **GIVEAWAY ENDED** 🎉");
        embed.set_colour("#2B2D31");

        auto req_roles = split_csv(g.required_role_id);

        // Filter entrants against requirements
        std::vector<std::string> eligible;
        for (const auto& uid : entrants) {
            // 1. Role requirement check (supports single or multi-role comma-separated IDs, with REST fallback for uncached members)
            if (!req_roles.empty()) {
                auto m_opt = bot.get_member(g.server_id, uid);
                if (!m_opt) {
                    auto m_res = bot.rest().get_server_member(g.server_id, uid);
                    if (m_res.success()) {
                        m_opt = stoatpp::models::Member::from_json(m_res.body);
                        bot.cache_member(*m_opt);
                    }
                }
                if (!m_opt) continue;

                bool has_all_roles = true;
                for (const auto& req_rid : req_roles) {
                    if (std::find(m_opt->roles.begin(), m_opt->roles.end(), req_rid) == m_opt->roles.end()) {
                        has_all_roles = false;
                        break;
                    }
                }
                if (!has_all_roles) continue;
            }

            // 2. Minimum level requirement check
            if (g.min_level > 0 && g_db) {
                auto xp_opt = g_db->get_user_xp(uid, g.server_id);
                if (!xp_opt || xp_opt->level < (uint32_t)g.min_level) {
                    continue;
                }
            }

            // 3. Minimum message count requirement over period
            if (g.min_messages > 0 && g_db) {
                int msg_count = g_db->get_user_message_count_period(g.server_id, uid, g.messages_days);
                if (msg_count < g.min_messages) {
                    continue;
                }
            }

            eligible.push_back(uid);
        }

        if (eligible.empty()) {
            std::string desc = "**Prize:** " + g.prize + "\n\n"
                               "Could not determine a winner — no eligible entries met the requirements (" + std::to_string(entrants.size()) + " total joined).\n"
                               "• **Hosted by:** <@" + g.host_id + ">";
            embed.set_description(desc);
            edit_payload.add_embed(embed);
            bot.rest().edit_message(g.channel_id, g.message_id, edit_payload.to_json());

            bot.send_message(g.channel_id, "⚠️ The giveaway for **" + g.prize + "** has ended, but there were no eligible entries meeting the requirements!");
            return;
        }

        static thread_local std::mt19937 rng(std::random_device{}());
        std::shuffle(eligible.begin(), eligible.end(), rng);

        size_t win_count = std::min((size_t)g.winners_count, eligible.size());
        std::vector<std::string> winners(eligible.begin(), eligible.begin() + win_count);

        std::string win_mentions;
        for (size_t i = 0; i < winners.size(); ++i) {
            win_mentions += "<@" + winners[i] + ">" + (i + 1 < winners.size() ? ", " : "");
        }

        std::string desc = "**Prize:** " + g.prize + "\n\n"
                           "• **Winner" + (winners.size() > 1 ? "s" : "") + ":** " + win_mentions + "\n"
                           "• **Hosted by:** <@" + g.host_id + ">\n"
                           "• **Ended:** <t:" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + ":R>\n"
                           "• **Entries:** " + std::to_string(eligible.size()) + " eligible (" + std::to_string(entrants.size()) + " total)";
        embed.set_description(desc);
        edit_payload.add_embed(embed);
        bot.rest().edit_message(g.channel_id, g.message_id, edit_payload.to_json());

        // Winner announcement
        std::string ann = "🎉 Congratulations " + win_mentions + "! You won **" + g.prize + "**! (Hosted by <@" + g.host_id + ">)";
        bot.send_message(g.channel_id, ann);
    }
};

} // namespace bronx::cogs
