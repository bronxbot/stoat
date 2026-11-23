#pragma once
#include "../database/database.h"
#include "command_config.h"
#include "helpers.h"
#include "stoatpp/bot_module.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace bronx::cogs {

class DeadChatModule : public stoatpp::bot_module {
private:
  struct ChannelState {
    std::array<double, 168> bins{};
    bool bins_loaded = false;
    bool dirty = false;
    double hawkes_m = 0.0;
    uint64_t last_msg_at = 0;
    uint64_t last_notified_at = 0;
    bool armed = false;
    uint64_t samples_seen = 0;
    std::deque<std::pair<uint64_t, std::string>> recent_speakers; // (timestamp, user_id)
  };

  std::mutex state_mutex_;
  std::unordered_map<std::string, ChannelState> states_;
  std::unordered_map<std::string, bronx::DeadChatConfig> monitored_; // channel_id -> config
  std::atomic<bool> running_{false};
  std::thread sweep_thread_;

  // Hawkes decay: 5-minute half-life (beta = ln(2)/300s), stationary branching ratio 0.6
  static constexpr double kBeta = 0.0023104906;
  static constexpr double kAlpha = 0.6 * kBeta;
  static constexpr double kEwmaLr = 0.05;

  static int current_bin(uint64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    int bin = tm_buf.tm_wday * 24 + tm_buf.tm_hour;
    return std::max(0, std::min(167, bin));
  }

  static double smoothed_mu(const std::array<double, 168> &bins, int bin) {
    int prev = (bin + 167) % 168;
    int next = (bin + 1) % 168;
    // Neighbor-smoothed messages/sec rate (with a tiny floor so silence eventually triggers in active hours)
    return 0.5 * bins[static_cast<size_t>(bin)] +
           0.25 * bins[static_cast<size_t>(prev)] +
           0.25 * bins[static_cast<size_t>(next)];
  }

  void refresh_monitored_configs() {
    if (!bronx::g_db)
      return;
    auto all = bronx::g_db->get_all_dead_chat_configs();
    std::lock_guard<std::mutex> lock(state_mutex_);
    monitored_.clear();
    for (const auto &c : all) {
      monitored_[c.channel_id] = c;
      auto &st = states_[c.channel_id];
      if (!st.bins_loaded) {
        st.bins = bronx::g_db->load_channel_hourly_baselines(c.channel_id);
        st.bins_loaded = true;
      }
    }
  }

public:
  ~DeadChatModule() override {
    running_ = false;
    if (sweep_thread_.joinable())
      sweep_thread_.join();
  }

  void register_handlers(stoatpp::cluster &bot) override {
    refresh_monitored_configs();

    // Update 168-bin Poisson baseline + O(1) Hawkes momentum on every message
    bot.on_message([this, &bot](const stoatpp::events::Message &e) {
      if (e.author.id.empty() || e.author.id == bot.current_user().id || e.author.bot)
        return;

      uint64_t now = static_cast<uint64_t>(std::time(nullptr));
      std::lock_guard<std::mutex> lock(state_mutex_);
      auto mit = monitored_.find(e.channel_id);
      if (mit == monitored_.end() || !mit->second.enabled)
        return;

      auto &st = states_[e.channel_id];
      if (!st.bins_loaded && bronx::g_db) {
        st.bins = bronx::g_db->load_channel_hourly_baselines(e.channel_id);
        st.bins_loaded = true;
      }

      if (st.last_msg_at > 0 && now >= st.last_msg_at) {
        double dt = static_cast<double>(now - st.last_msg_at);
        st.hawkes_m = st.hawkes_m * std::exp(-kBeta * dt) + kAlpha;
        double inst_rate = 1.0 / std::max(5.0, dt); // msgs/sec capped at 0.2/sec
        int bin = current_bin(now);
        st.bins[static_cast<size_t>(bin)] =
            (1.0 - kEwmaLr) * st.bins[static_cast<size_t>(bin)] + kEwmaLr * inst_rate;
        st.dirty = true;
      } else {
        st.hawkes_m += kAlpha;
      }
      st.last_msg_at = now;
      st.samples_seen++;

      // Track unique human speakers in the last 15 minutes
      st.recent_speakers.emplace_back(now, e.author.id);
      while (!st.recent_speakers.empty() && now - st.recent_speakers.front().first > 900) {
        st.recent_speakers.pop_front();
      }
      std::unordered_set<std::string> uniq;
      for (const auto &p : st.recent_speakers) {
        uniq.insert(p.second);
      }
      if (static_cast<int>(uniq.size()) >= mit->second.min_speakers) {
        st.armed = true;
      }
    });

    // Background 30s sweep + 5m baseline flush
    running_ = true;
    sweep_thread_ = std::thread([this, &bot]() {
      int tick = 0;
      while (running_) {
        for (int i = 0; i < 30 && running_; ++i) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!running_)
          break;
        tick++;

        uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        int bin = current_bin(now);

        struct PendingAlert {
          bronx::DeadChatConfig cfg;
          double prob;
          uint64_t silence_sec;
        };
        std::vector<PendingAlert> alerts;
        std::vector<std::pair<std::string, std::array<double, 168>>> to_flush;

        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          for (const auto &[ch_id, cfg] : monitored_) {
            if (!cfg.enabled)
              continue;
            auto it = states_.find(ch_id);
            if (it == states_.end())
              continue;
            auto &st = it->second;

            if (tick % 10 == 0 && st.dirty) {
              to_flush.emplace_back(ch_id, st.bins);
              st.dirty = false;
            }

            if (!st.armed || st.last_msg_at == 0)
              continue;
            if (now <= st.last_msg_at)
              continue;
            if (st.last_notified_at > 0 && now < st.last_notified_at + static_cast<uint64_t>(cfg.cooldown_sec))
              continue;

            double dt = static_cast<double>(now - st.last_msg_at);
            if (dt < 600.0) // minimum 10 minutes of silence before considering chat dead
              continue;

            double mu = smoothed_mu(st.bins, bin);
            if (mu <= 0.0 && st.samples_seen < 15) // warm-up guard on brand-new channels
              continue;
            double integral = mu * dt + (st.hawkes_m / kBeta) * (1.0 - std::exp(-kBeta * dt));
            double p_silence = std::exp(-integral);

            if (p_silence < cfg.p_threshold) {
              st.armed = false;
              st.last_notified_at = now;
              alerts.push_back({cfg, p_silence, now - st.last_msg_at});
            }
          }
        }

        if (bronx::g_db) {
          for (const auto &[ch_id, bins] : to_flush) {
            bronx::g_db->save_channel_hourly_baselines(ch_id, bins);
          }
        }

        for (const auto &a : alerts) {
          std::string target_ch = a.cfg.notify_channel_id.empty() ? a.cfg.channel_id : a.cfg.notify_channel_id;
          std::string msg_text;
          if (!a.cfg.ping_role_id.empty()) {
            msg_text += "<@&" + a.cfg.ping_role_id + "> ";
          }
          if (!a.cfg.custom_message.empty()) {
            std::string body = a.cfg.custom_message;
            size_t pos = 0;
            while ((pos = body.find("{channel}", pos)) != std::string::npos) {
              body.replace(pos, 9, "<#" + a.cfg.channel_id + ">");
              pos += a.cfg.channel_id.size() + 3;
            }
            msg_text += body;
          } else {
            uint64_t mins = a.silence_sec / 60;
            std::ostringstream ps;
            ps << std::fixed << std::setprecision(1) << (a.prob * 100.0);
            msg_text += "💀 **Dead Chat Alert!** <#" + a.cfg.channel_id + "> has been silent for **" +
                        std::to_string(mins) + "m** (surprise p=" + ps.str() + "%). Revive the chat!";
          }
          bot.send_message(target_ch, msg_text);
        }
      }
    });

    // .deadchat command
    bot.register_command({
        "deadchat",
        {"dc", "deadchatnotifier"},
        "Configure the 168-bin Poisson + Hawkes dead chat notifier.",
        "deadchat [channel|notify|role|sensitivity|speakers|cooldown|message|off]",
        {"[subcommand]", "[value]"},
        [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg, const std::vector<std::string> &args) {
          if (msg.server_id.empty() || !bronx::g_db)
            return;

          bot.fetch_member(msg.server_id, msg.author.id, [this, &bot, msg, args](stoatpp::models::Member member, bool ok) {
            if (!ok || !is_user_trusted_or_owner(bot, msg.server_id, msg.author.id, member)) {
              bot.send_message(msg.channel_id, bronx::emj_warning + " You need Manage Server permission to configure `.deadchat`.");
              return;
            }

            std::string pfx = msg.prefix.empty() ? bot.config().command_prefix : msg.prefix;
            auto extract_ch = [&](const std::string &raw) {
              if (raw.size() > 3 && raw.rfind("<#", 0) == 0 && raw.back() == '>') {
                return raw.substr(2, raw.size() - 3);
              }
              return raw;
            };
            auto resolve_role = [&](std::string q) -> std::string {
              if (q == "none" || q == "off" || q == "skip") return "";
              if (q.size() > 3 && q.rfind("<@&", 0) == 0 && q.back() == '>')
                q = q.substr(3, q.size() - 4);
              auto srv_opt = bot.get_server(msg.server_id);
              if (srv_opt) {
                std::string low_q = q;
                std::transform(low_q.begin(), low_q.end(), low_q.begin(), ::tolower);
                for (const auto &r : srv_opt->roles) {
                  std::string low_r = r.name;
                  std::transform(low_r.begin(), low_r.end(), low_r.begin(), ::tolower);
                  if (r.id == q || low_r == low_q) return r.id;
                }
              }
              return q;
            };

            std::string first_low = args.empty() ? "" : args[0];
            std::transform(first_low.begin(), first_low.end(), first_low.begin(), ::tolower);

            if (args.empty() || first_low == "setup") {
              auto list = bronx::g_db->get_server_dead_chat_configs(msg.server_id);
              std::string intro = "💀 **Dead Chat Setup Wizard**\n";
              if (!list.empty()) {
                intro += "\n| Channel | Role | p-Thresh | Status |\n| --- | --- | --- | --- |\n";
                for (const auto &c : list) {
                  std::string rdisp = c.ping_role_id.empty() ? "none" : ("<@&" + c.ping_role_id + ">");
                  intro += "| <#" + c.channel_id + "> | " + rdisp + " | " +
                           std::to_string(c.p_threshold).substr(0, 5) + " | " + (c.enabled ? "on" : "off") + " |\n";
                }
              }
              intro += "\n**Step 1/3:** Mention the channel to monitor (e.g. `<#" + msg.channel_id + ">`), reply `here` for this channel, or `cancel` to exit.";
              bot.send_message(msg.channel_id, intro);

              bot.await_message(msg.channel_id, msg.author.id, std::chrono::seconds(60),
                  [this, &bot, msg, extract_ch, resolve_role](std::optional<stoatpp::events::Message> r1) {
                if (!r1) { bot.send_message(msg.channel_id, "⏱️ Dead chat setup timed out."); return; }
                std::string t1 = r1->content;
                std::transform(t1.begin(), t1.end(), t1.begin(), ::tolower);
                if (t1 == "cancel") { bot.send_message(msg.channel_id, bronx::emj_no + " Setup canceled."); return; }
                std::string ch_id = (t1 == "here" || t1 == "this") ? msg.channel_id : extract_ch(r1->content);
                if (ch_id.empty()) ch_id = msg.channel_id;

                auto cfg = bronx::g_db->get_dead_chat_config(msg.server_id, ch_id).value_or(bronx::DeadChatConfig{});
                cfg.server_id = msg.server_id;
                cfg.channel_id = ch_id;
                if (cfg.notify_channel_id.empty()) cfg.notify_channel_id = ch_id;
                cfg.enabled = true;

                bot.send_message(msg.channel_id, "**Step 2/3:** Mention a role to ping when chat dies (or reply `none` / `skip`).");
                bot.await_message(msg.channel_id, msg.author.id, std::chrono::seconds(60),
                    [this, &bot, msg, cfg, resolve_role](std::optional<stoatpp::events::Message> r2) mutable {
                  if (!r2) { bot.send_message(msg.channel_id, "⏱️ Dead chat setup timed out."); return; }
                  std::string t2 = r2->content;
                  std::transform(t2.begin(), t2.end(), t2.begin(), ::tolower);
                  if (t2 == "cancel") { bot.send_message(msg.channel_id, bronx::emj_no + " Setup canceled."); return; }
                  if (t2 != "skip") {
                    cfg.ping_role_id = resolve_role(r2->content);
                  }

                  bot.send_message(msg.channel_id, "**Step 3/3:** Choose sensitivity (`low`, `medium`, `high`, or `skip` for `medium`).");
                  bot.await_message(msg.channel_id, msg.author.id, std::chrono::seconds(60),
                      [this, &bot, msg, cfg](std::optional<stoatpp::events::Message> r3) mutable {
                    if (!r3) { bot.send_message(msg.channel_id, "⏱️ Dead chat setup timed out."); return; }
                    std::string t3 = r3->content;
                    std::transform(t3.begin(), t3.end(), t3.begin(), ::tolower);
                    if (t3 == "cancel") { bot.send_message(msg.channel_id, bronx::emj_no + " Setup canceled."); return; }
                    if (t3 == "low") cfg.p_threshold = 0.005;
                    else if (t3 == "high") cfg.p_threshold = 0.05;
                    else if (t3 == "medium" || t3 == "med" || t3 == "skip") cfg.p_threshold = 0.02;
                    else { try { cfg.p_threshold = std::max(0.001, std::min(0.25, std::stod(t3))); } catch (...) {} }

                    bronx::g_db->save_dead_chat_config(cfg);
                    refresh_monitored_configs();
                    bot.send_message(msg.channel_id,
                        "✅ **Dead Chat Notifier Configured!**\n"
                        "• **Channel:** <#" + cfg.channel_id + ">\n"
                        "• **Ping Role:** " + (cfg.ping_role_id.empty() ? "none" : "<@&" + cfg.ping_role_id + ">") + "\n"
                        "• **Sensitivity (p-thresh):** " + std::to_string(cfg.p_threshold).substr(0, 5));
                  });
                });
              });
              return;
            }

            if (first_low == "analyze" || first_low == "analyse" || first_low == "stats") {
              std::string ch_id = (args.size() >= 2) ? extract_ch(args[1]) : msg.channel_id;
              uint64_t now = static_cast<uint64_t>(std::time(nullptr));
              int bin = current_bin(now);
              double mu = 0.0, m_val = 0.0, p_sil = 1.0;
              uint64_t dt_sec = 0, samples = 0;
              bool is_armed = false;
              {
                std::lock_guard<std::mutex> lock(state_mutex_);
                auto it = states_.find(ch_id);
                if (it != states_.end()) {
                  const auto &st = it->second;
                  mu = smoothed_mu(st.bins, bin);
                  m_val = st.hawkes_m;
                  samples = st.samples_seen;
                  is_armed = st.armed;
                  if (st.last_msg_at > 0 && now > st.last_msg_at) {
                    dt_sec = now - st.last_msg_at;
                    double dt = static_cast<double>(dt_sec);
                    double integral = mu * dt + (m_val / kBeta) * (1.0 - std::exp(-kBeta * dt));
                    p_sil = std::exp(-integral);
                  }
                }
              }
              std::ostringstream out;
              out << std::fixed << std::setprecision(4)
                  << "📊 **Dead Chat Analysis for <#" << ch_id << ">**\n\n"
                  << "| Metric | Value |\n| --- | --- |\n"
                  << "| Cyclical Bin (`dow*24+hr`) | `" << bin << "` |\n"
                  << "| Smoothed Baseline (`mu`) | `" << (mu * 60.0) << "` msgs/min |\n"
                  << "| Hawkes Momentum (`M`) | `" << m_val << "` |\n"
                  << "| Silence (`dt`) | `" << dt_sec << "s` |\n"
                  << "| Surprise `P(silence >= dt)` | **" << std::setprecision(2) << (p_sil * 100.0) << "%** |\n"
                  << "| Armed / Samples | " << (is_armed ? "yes" : "no") << " (" << samples << " msgs) |\n";
              bot.send_message(msg.channel_id, out.str());
              return;
            }

            if (first_low == "status" || first_low == "list") {
              auto list = bronx::g_db->get_server_dead_chat_configs(msg.server_id);
              std::string out = "💀 **Dead Chat Notifier (168-Bin Poisson + Hawkes)**\n\n";
              if (list.empty()) {
                out += "*No monitored channels configured.* Run `" + pfx + "deadchat` to start the setup wizard.\n";
              } else {
                out += "| Channel | Notify | Role | p-Thresh | Min Speakers | Cooldown | Status |\n"
                       "| --- | --- | --- | --- | --- | --- | --- |\n";
                for (const auto &c : list) {
                  std::string rdisp = c.ping_role_id.empty() ? "none" : ("<@&" + c.ping_role_id + ">");
                  out += "| <#" + c.channel_id + "> | <#" + c.notify_channel_id + "> | " + rdisp + " | " +
                         std::to_string(c.p_threshold).substr(0, 5) + " | " + std::to_string(c.min_speakers) +
                         " | " + std::to_string(c.cooldown_sec) + "s | " + (c.enabled ? "on" : "off") + " |\n";
                }
              }
              bot.send_message(msg.channel_id, out);
              return;
            }

            // Smart inference when passing mentions/presets directly without a subcommand
            if (args[0].rfind("<#", 0) == 0 && args[0].back() == '>') {
              std::string ch_id = extract_ch(args[0]);
              auto cfg = bronx::g_db->get_dead_chat_config(msg.server_id, ch_id).value_or(bronx::DeadChatConfig{});
              cfg.server_id = msg.server_id;
              cfg.channel_id = ch_id;
              if (cfg.notify_channel_id.empty()) cfg.notify_channel_id = ch_id;
              cfg.enabled = true;
              if (args.size() >= 2 && args[1].rfind("<@&", 0) == 0) {
                cfg.ping_role_id = resolve_role(args[1]);
              }
              bronx::g_db->save_dead_chat_config(cfg);
              refresh_monitored_configs();
              bot.send_message(msg.channel_id, "✅ Dead chat notifier enabled for <#" + cfg.channel_id + ">" +
                  (cfg.ping_role_id.empty() ? "." : (" (ping: <@&" + cfg.ping_role_id + ">).")));
              return;
            }
            if (args[0].rfind("<@&", 0) == 0 && args[0].back() == '>') {
              auto existing = bronx::g_db->get_server_dead_chat_configs(msg.server_id);
              std::string ch_id = existing.empty() ? msg.channel_id : existing[0].channel_id;
              auto cfg = bronx::g_db->get_dead_chat_config(msg.server_id, ch_id).value_or(bronx::DeadChatConfig{});
              cfg.server_id = msg.server_id;
              cfg.channel_id = ch_id;
              if (cfg.notify_channel_id.empty()) cfg.notify_channel_id = ch_id;
              cfg.enabled = true;
              cfg.ping_role_id = resolve_role(args[0]);
              bronx::g_db->save_dead_chat_config(cfg);
              refresh_monitored_configs();
              bot.send_message(msg.channel_id, "✅ Dead chat ping role set to <@&" + cfg.ping_role_id + "> for <#" + cfg.channel_id + ">.");
              return;
            }

            std::string sub = first_low;

            std::string target_ch = msg.channel_id;
            auto existing = bronx::g_db->get_server_dead_chat_configs(msg.server_id);
            if (!existing.empty() && sub != "channel") {
              target_ch = existing[0].channel_id;
            }
            if (sub == "channel" && args.size() >= 2) {
              target_ch = extract_ch(args[1]);
            } else if (sub == "off" && args.size() >= 2) {
              target_ch = extract_ch(args[1]);
            }

            auto cfg = bronx::g_db->get_dead_chat_config(msg.server_id, target_ch).value_or(bronx::DeadChatConfig{});
            cfg.server_id = msg.server_id;
            cfg.channel_id = target_ch;
            if (cfg.notify_channel_id.empty())
              cfg.notify_channel_id = target_ch;

            if (sub == "channel") {
              cfg.enabled = true;
              bronx::g_db->save_dead_chat_config(cfg);
              refresh_monitored_configs();
              bot.send_message(msg.channel_id, "✅ Dead chat notifier enabled for <#" + cfg.channel_id + ">.");
            } else if (sub == "notify" && args.size() >= 2) {
              cfg.notify_channel_id = extract_ch(args[1]);
              bronx::g_db->save_dead_chat_config(cfg);
              refresh_monitored_configs();
              bot.send_message(msg.channel_id, "✅ Dead chat alerts will be sent to <#" + cfg.notify_channel_id + ">.");
            } else if (sub == "role" && args.size() >= 2) {
              std::string q = args[1];
              if (q == "none" || q == "off") {
                cfg.ping_role_id.clear();
              } else {
                if (q.size() > 3 && q.rfind("<@&", 0) == 0 && q.back() == '>')
                  q = q.substr(3, q.size() - 4);
                auto srv_opt = bot.get_server(msg.server_id);
                if (srv_opt) {
                  std::string low_q = q;
                  std::transform(low_q.begin(), low_q.end(), low_q.begin(), ::tolower);
                  for (const auto &r : srv_opt->roles) {
                    std::string low_r = r.name;
                    std::transform(low_r.begin(), low_r.end(), low_r.begin(), ::tolower);
                    if (r.id == q || low_r == low_q) {
                      q = r.id;
                      break;
                    }
                  }
                }
                cfg.ping_role_id = q;
              }
              bronx::g_db->save_dead_chat_config(cfg);
              refresh_monitored_configs();
              bot.send_message(msg.channel_id, cfg.ping_role_id.empty()
                  ? "✅ Dead chat ping role cleared."
                  : "✅ Dead chat ping role set to <@&" + cfg.ping_role_id + ">.");
            } else if (sub == "sensitivity" && args.size() >= 2) {
              std::string v = args[1];
              std::transform(v.begin(), v.end(), v.begin(), ::tolower);
              if (v == "low") cfg.p_threshold = 0.005;
              else if (v == "medium" || v == "med") cfg.p_threshold = 0.02;
              else if (v == "high") cfg.p_threshold = 0.05;
              else {
                try { cfg.p_threshold = std::max(0.001, std::min(0.25, std::stod(v))); } catch (...) {}
              }
              bronx::g_db->save_dead_chat_config(cfg);
              refresh_monitored_configs();
              bot.send_message(msg.channel_id, "✅ Dead chat p-threshold set to **" + std::to_string(cfg.p_threshold).substr(0, 5) + "**.");
            } else if (sub == "speakers" && args.size() >= 2) {
              try { cfg.min_speakers = std::max(1, std::min(10, std::stoi(args[1]))); } catch (...) {}
              bronx::g_db->save_dead_chat_config(cfg);
              refresh_monitored_configs();
              bot.send_message(msg.channel_id, "✅ Minimum active speakers to arm set to **" + std::to_string(cfg.min_speakers) + "**.");
            } else if (sub == "cooldown" && args.size() >= 2) {
              try { cfg.cooldown_sec = std::max(120, std::min(86400, std::stoi(args[1]))); } catch (...) {}
              bronx::g_db->save_dead_chat_config(cfg);
              refresh_monitored_configs();
              bot.send_message(msg.channel_id, "✅ Dead chat cooldown set to **" + std::to_string(cfg.cooldown_sec) + "s**.");
            } else if (sub == "message" && args.size() >= 2) {
              std::string body;
              for (size_t i = 1; i < args.size(); ++i) {
                if (i > 1) body += " ";
                body += args[i];
              }
              cfg.custom_message = (body == "reset" || body == "default") ? "" : body;
              bronx::g_db->save_dead_chat_config(cfg);
              refresh_monitored_configs();
              bot.send_message(msg.channel_id, "✅ Dead chat alert message updated.");
            } else if (sub == "off" || sub == "disable") {
              cfg.enabled = false;
              bronx::g_db->save_dead_chat_config(cfg);
              refresh_monitored_configs();
              bot.send_message(msg.channel_id, "🛑 Dead chat notifier disabled for <#" + cfg.channel_id + ">.");
            } else {
              bot.send_message(msg.channel_id, "Usage: `" + pfx + "deadchat <channel|notify|role|sensitivity|speakers|cooldown|message|off>`");
            }
          });
        },
        "Utility"
    });
  }
};

} // namespace bronx::cogs
