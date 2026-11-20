#pragma once
#include "stoatpp/bot_module.h"
#include "helpers.h"
#include "../database.h"
#include <chrono>
#include <thread>
#include <cctype>

namespace bronx::cogs {

class RemindersModule : public stoatpp::bot_module {
private:
    static void send_reminder_list(stoatpp::cluster& bot,
                                   const stoatpp::events::Message& msg) {
        if (!g_db) {
            bot.send_message(msg.channel_id,
                             "Reminder storage is currently unavailable.");
            return;
        }

        auto rems = g_db->get_user_reminders(msg.author.id);
        if (rems.empty()) {
            bot.send_message(msg.channel_id,
                             "You don't have any active reminders. Set one with `" +
                             (msg.prefix.empty() ? bot.config().command_prefix : msg.prefix) +
                             "remind <time> <message>`.");
            return;
        }

        stoatpp::models::MessagePayload payload;
        stoatpp::models::Embed embed;
        embed.set_colour(0x5865F2);
        embed.set_title("Your reminders");

        std::string desc;
        for (size_t i = 0; i < rems.size(); ++i) {
            desc += "**" + std::to_string(i + 1) + ".** (`#" + std::to_string(rems[i].id) + "`) <t:" +
                    std::to_string(rems[i].timestamp_due) + ":R> — " +
                    rems[i].message + "\n";
            if (desc.size() > 3000) {
                desc += "... and more";
                break;
            }
        }
        embed.set_description(desc);
        payload.add_embed(embed);
        bot.send_message(msg.channel_id, payload);
    }

    static void handle_reminder_delete(stoatpp::cluster& bot,
                                       const stoatpp::events::Message& msg,
                                       const std::string& spec) {
        if (!g_db) {
            bot.send_message(msg.channel_id, "Reminder storage is currently unavailable.");
            return;
        }
        auto rems = g_db->get_user_reminders(msg.author.id);
        if (rems.empty()) {
            bot.send_message(msg.channel_id, "You don't have any active reminders to delete.");
            return;
        }

        bool explicit_id = (!spec.empty() && spec[0] == '#');
        std::string num_str = explicit_id ? spec.substr(1) : spec;
        int val = 0;
        try {
            val = std::stoi(num_str);
        } catch (...) {
            bot.send_message(msg.channel_id, "" + bronx::emj_warning + " invalid reminder number or `#id`.");
            return;
        }

        int matched_idx = -1;
        if (explicit_id) {
            for (size_t i = 0; i < rems.size(); ++i) {
                if (rems[i].id == val) { matched_idx = static_cast<int>(i); break; }
            }
        } else if (val >= 1 && static_cast<size_t>(val) <= rems.size()) {
            matched_idx = val - 1;
        } else {
            for (size_t i = 0; i < rems.size(); ++i) {
                if (rems[i].id == val) { matched_idx = static_cast<int>(i); break; }
            }
        }

        if (matched_idx < 0) {
            bot.send_message(msg.channel_id, "" + bronx::emj_warning + " could not find reminder `" + spec + "` among your active reminders.");
            return;
        }

        const auto& target = rems[matched_idx];
        if (g_db->delete_reminder(target.id)) {
            bot.send_message(msg.channel_id, "" + bronx::emj_yes + " Deleted reminder (`#" + std::to_string(target.id) + "`): *" + target.message + "*");
        } else {
            bot.send_message(msg.channel_id, "" + bronx::emj_warning + " failed to delete reminder.");
        }
    }

    static void handle_reminder_clear(stoatpp::cluster& bot,
                                      const stoatpp::events::Message& msg) {
        if (!g_db) {
            bot.send_message(msg.channel_id, "Reminder storage is currently unavailable.");
            return;
        }
        auto rems = g_db->get_user_reminders(msg.author.id);
        if (rems.empty()) {
            bot.send_message(msg.channel_id, "You don't have any active reminders to clear.");
            return;
        }
        int cleared = 0;
        for (const auto& r : rems) {
            if (g_db->delete_reminder(r.id)) cleared++;
        }
        bot.send_message(msg.channel_id, "" + bronx::emj_yes + " Cleared **" + std::to_string(cleared) + "** active reminder(s).");
    }

public:
    void register_handlers(stoatpp::cluster& bot) override {
        bot.register_command({
            "remind",
            {"remindme", "reminder"},
            "set, list, delete, or clear reminders.",
            "remind <time> <message> | remind list | remind delete <number|#id> | remind clear",
            {},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                const std::string prefix =
                    msg.prefix.empty() ? bot.config().command_prefix : msg.prefix;

                if (!args.empty()) {
                    std::string sub = args[0];
                    std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);
                    if (sub == "list") {
                        send_reminder_list(bot, msg);
                        return;
                    }
                    if (sub == "clear") {
                        handle_reminder_clear(bot, msg);
                        return;
                    }
                    if (sub == "delete" || sub == "remove" || sub == "cancel" || sub == "del") {
                        if (args.size() < 2) {
                            bot.send_message(msg.channel_id, "Usage: `" + prefix + "remind " + sub + " <number|#id>` (use `" + prefix + "remind list` to see numbers/IDs).");
                            return;
                        }
                        handle_reminder_delete(bot, msg, args[1]);
                        return;
                    }
                }

                if (args.size() < 2) {
                    bot.send_message(msg.channel_id, "Usage: `" + prefix +
                        "remind <time> <message>` (for example, `" + prefix +
                        "remind 2h check oven`), `" + prefix +
                        "remind list`, `" + prefix + "remind delete <number|#id>`, or `" + prefix + "remind clear`.");
                    return;
                }

                std::string time_str = args[0];
                uint64_t duration_sec = helpers::parse_duration(time_str);

                if (duration_sec == 0) {
                    bot.send_message(msg.channel_id, "invalid time format. examples: `10m`, `1h30m`, `2d`, `1y`");
                    return;
                }

                uint64_t max_duration = 31536000ULL * 2; // 2 years in seconds
                if (duration_sec > max_duration) {
                    bot.send_message(msg.channel_id, "reminders cannot be set for more than 2 years in the future.");
                    return;
                }

                std::string message_content = "";
                for (size_t i = 1; i < args.size(); ++i) {
                    message_content += args[i] + (i == args.size() - 1 ? "" : " ");
                }

                auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                auto due_unix = now_unix + duration_sec;

                if (g_db->add_reminder(msg.author.id, msg.channel_id, message_content, now_unix, due_unix)) {
                    bot.send_message(msg.channel_id, "⏰ okay, i'll remind you on <t:" + std::to_string(due_unix) + ":F>.");
                } else {
                    bot.send_message(msg.channel_id, "failed to save reminder to database.");
                }
            },
            "Utility"
        });

        // Compatibility shortcut; the canonical route is `.remind list`.
        bot.register_command({
            "reminders",
            {"remindlist", "reminderlist"},
            "shortcut for `remind list`.",
            "",
            {},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>&) {
                send_reminder_list(bot, msg);
            },
            "Utility"
        });

        bot.register_command({
            "delremind",
            {"rmremind", "unremind"},
            "delete one of your active reminders by list number or #id.",
            "<number|#id>",
            {"<number|#id>"},
            [](stoatpp::cluster& bot, const stoatpp::events::Message& msg, const std::vector<std::string>& args) {
                const std::string prefix =
                    msg.prefix.empty() ? bot.config().command_prefix : msg.prefix;
                if (args.empty()) {
                    bot.send_message(msg.channel_id, "Usage: `" + prefix + "delremind <number|#id>` (use `" + prefix + "remind list` to view active reminders).");
                    return;
                }
                handle_reminder_delete(bot, msg, args[0]);
            },
            "Utility"
        });

        std::thread([&bot]() {
            while (true) {
                auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                auto due_reminders = g_db->get_due_reminders(now_unix);
                for (const auto& r : due_reminders) {
                    stoatpp::models::MessagePayload payload;
                    payload.content = "<@" + r.user_id + "> reminder set on <t:" + std::to_string(r.timestamp_set) + ":f>:\n" + r.message;
                    
                    bot.send_message(r.channel_id, payload, [&bot, r, payload](stoatpp::models::Message, bool success) {
                        if (!success) {
                            // Channel might have been deleted, try to DM the user.
                            // rest client calls are synchronous, run in a background thread to avoid blocking callback dispatcher.
                            std::thread([&bot, r, payload]() {
                                auto dm_res = bot.rest().open_dm(r.user_id);
                                if (dm_res.success() && dm_res.body.is_object() && dm_res.body.contains("_id")) {
                                    std::string dm_channel_id = dm_res.body["_id"].get<std::string>();
                                    stoatpp::models::MessagePayload dm_payload = payload;
                                    dm_payload.content = "" + bronx::emj_warning + " *Original channel was deleted. Sending reminder to DMs:*\n" + dm_payload.content;
                                    bot.send_message(dm_channel_id, dm_payload);
                                }
                            }).detach();
                        }
                    });
                    g_db->delete_reminder(r.id);
                }
                std::this_thread::sleep_for(std::chrono::seconds(15));
            }
        }).detach();
    }
};

} // namespace bronx::cogs
