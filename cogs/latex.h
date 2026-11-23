#pragma once
#include "chart_renderer.h"
#include "command_config.h"
#include "helpers.h"
#include "stoatpp/bot_module.h"
#include <algorithm>
#include <curl/curl.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace bronx::cogs {

class LatexModule : public stoatpp::bot_module {
private:
  std::mutex cfg_mutex_;
  std::unordered_set<std::string> auto_enabled_servers_;

  static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    static_cast<std::string *>(userp)->append(static_cast<const char *>(contents), total);
    return total;
  }

  static std::string strip_tex_wrappers(std::string s) {
    auto trim = [](std::string &str) {
      size_t a = str.find_first_not_of(" \t\r\n");
      if (a == std::string::npos) { str.clear(); return; }
      size_t b = str.find_last_not_of(" \t\r\n");
      str = str.substr(a, b - a + 1);
    };
    trim(s);
    if (s.rfind("```", 0) == 0) {
      size_t nl = s.find('\n');
      if (nl != std::string::npos) s = s.substr(nl + 1);
      if (s.size() >= 3 && s.substr(s.size() - 3) == "```") s = s.substr(0, s.size() - 3);
      trim(s);
    }
    if (s.size() >= 4 && s.rfind("$$", 0) == 0 && s.substr(s.size() - 2) == "$$") {
      s = s.substr(2, s.size() - 4);
    } else if (s.size() >= 2 && s.front() == '$' && s.back() == '$') {
      s = s.substr(1, s.size() - 2);
    } else if (s.size() >= 4 && s.rfind("[;", 0) == 0 && s.substr(s.size() - 2) == ";]") {
      s = s.substr(2, s.size() - 4);
    }
    trim(s);
    return s;
  }

  static std::string render_latex_png(const std::string &formula, std::string &err_out) {
    CURL *curl = curl_easy_init();
    if (!curl) {
      err_out = "curl init failed";
      return {};
    }

    std::string full_tex = "\\dpi{220}\\bg{1e1f22}\\color{White}" + formula;
    char *escaped = curl_easy_escape(curl, full_tex.c_str(), static_cast<int>(full_tex.size()));
    if (!escaped) {
      curl_easy_cleanup(curl);
      err_out = "url escape failed";
      return {};
    }

    std::string url = std::string("https://latex.codecogs.com/png.image?") + escaped;
    curl_free(escaped);

    std::string png_bytes;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &png_bytes);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200 || png_bytes.size() < 32) {
      err_out = "render failed (HTTP " + std::to_string(http_code) + ")";
      return {};
    }
    return png_bytes;
  }

  static std::string resolve_target_id(stoatpp::cluster &bot,
                                       const std::string &channel_id,
                                       const std::string &server_id,
                                       bool allow_rest = false) {
    if (!server_id.empty()) return server_id;
    auto ch = bot.get_channel(channel_id);
    if (ch && ch->server && !ch->server->empty()) return *ch->server;
    if (allow_rest) {
      auto res = bot.rest().get_channel(channel_id);
      if (res.success() && res.body.is_object() && res.body.contains("server") && res.body["server"].is_string()) {
        return res.body["server"].get<std::string>();
      }
    }
    return channel_id;
  }

  static void send_rendered_latex(stoatpp::cluster &bot,
                                  const std::string &channel_id,
                                  const std::string &formula,
                                  const std::string &header_text = "") {
    std::string err;
    std::string png = render_latex_png(formula, err);
    if (png.empty()) {
      if (!header_text.empty()) return; // silent fail on auto-catch
      bot.send_message(channel_id, bronx::emj_no + " Failed to render LaTeX (" + err + ").");
      return;
    }

    std::string file_id = chart::upload_chart(bot, png, "latex.png");
    if (file_id.empty()) {
      if (!header_text.empty()) return;
      bot.send_message(channel_id, bronx::emj_no + " Failed to upload rendered LaTeX image.");
      return;
    }

    stoatpp::models::MessagePayload payload;
    payload.content = header_text.empty() ? " " : header_text;
    nlohmann::json embed = nlohmann::json::object();
    embed["colour"] = "#2B2D31";
    embed["media"] = file_id;
    payload.embeds.push_back(embed);
    bot.send_message(channel_id, payload);
  }

public:
  void register_handlers(stoatpp::cluster &bot) override {
    // .latex / .tex / .math / .texit command
    bot.register_command({
        "latex",
        {"tex", "math", "texit"},
        "Render LaTeX math to an image, or toggle automatic $$...$$ rendering.",
        "latex <formula | auto <on|off>>",
        {"<formula...>"},
        [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg, const std::vector<std::string> &args) {
          std::string raw;
          for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) raw += " ";
            raw += args[i];
          }
          // Trim raw
          size_t fa = raw.find_first_not_of(" \t\r\n");
          if (fa == std::string::npos) raw.clear();
          else raw = raw.substr(fa, raw.find_last_not_of(" \t\r\n") - fa + 1);

          if (raw.empty()) {
            bot.send_message(msg.channel_id,
                "📐 **LaTeX / TeXit**\n"
                "• `.latex <formula>` - Render any LaTeX expression\n"
                "• `.texit auto on` / `.texit auto off` - Opt-in auto-rendering for `$$...$$` and `[;...;]` (disabled by default)");
            return;
          }

          // Split first two tokens from raw (since single <formula...> param joins words into args[0])
          std::string low_raw = raw;
          std::transform(low_raw.begin(), low_raw.end(), low_raw.begin(), ::tolower);
          std::string tok1, tok2;
          {
            size_t sp = low_raw.find_first_of(" \t\r\n");
            if (sp == std::string::npos) {
              tok1 = low_raw;
            } else {
              tok1 = low_raw.substr(0, sp);
              size_t sp2 = low_raw.find_first_not_of(" \t\r\n", sp);
              if (sp2 != std::string::npos) {
                size_t sp3 = low_raw.find_first_of(" \t\r\n", sp2);
                tok2 = (sp3 == std::string::npos) ? low_raw.substr(sp2) : low_raw.substr(sp2, sp3 - sp2);
              }
            }
          }

          std::string toggle_val;
          if (tok1 == "auto") {
            if (tok2.empty()) {
              std::string target_id = resolve_target_id(bot, msg.channel_id, msg.server_id, true);
              bool enabled = false;
              {
                std::lock_guard<std::mutex> lock(cfg_mutex_);
                enabled = auto_enabled_servers_.count(target_id) > 0;
              }
              bot.send_message(msg.channel_id, std::string("📐 Auto `$$...$$` rendering is currently **") +
                  (enabled ? "on" : "off") + "**. Use `.texit auto on` or `.texit auto off`.");
              return;
            }
            toggle_val = tok2;
          } else if ((tok1 == "on" || tok1 == "off") && tok2.empty()) {
            toggle_val = tok1;
          }

          if (toggle_val == "on" || toggle_val == "off") {
            std::string srv_id = resolve_target_id(bot, msg.channel_id, msg.server_id, true);
            if (srv_id != msg.channel_id) {
              bot.fetch_member(srv_id, msg.author.id, [this, &bot, msg, srv_id, toggle_val](stoatpp::models::Member member, bool ok) {
                if (!ok || !is_user_trusted_or_owner(bot, srv_id, msg.author.id, member)) {
                  bot.send_message(msg.channel_id, bronx::emj_warning + " You need Manage Server permission to toggle `texit auto`.");
                  return;
                }
                std::lock_guard<std::mutex> lock(cfg_mutex_);
                if (toggle_val == "on") {
                  auto_enabled_servers_.insert(srv_id);
                  bot.send_message(msg.channel_id, "✅ Enabled automatic `$$...$$` LaTeX rendering in this server.");
                } else {
                  auto_enabled_servers_.erase(srv_id);
                  bot.send_message(msg.channel_id, "🛑 Disabled automatic `$$...$$` LaTeX rendering in this server.");
                }
              });
            } else {
              std::lock_guard<std::mutex> lock(cfg_mutex_);
              if (toggle_val == "on") {
                auto_enabled_servers_.insert(srv_id);
                bot.send_message(msg.channel_id, "✅ Enabled automatic `$$...$$` LaTeX rendering in this channel.");
              } else {
                auto_enabled_servers_.erase(srv_id);
                bot.send_message(msg.channel_id, "🛑 Disabled automatic `$$...$$` LaTeX rendering in this channel.");
              }
            }
            return;
          }
          if (tok1 == "auto") {
            bot.send_message(msg.channel_id, "Usage: `.texit auto on` or `.texit auto off`.");
            return;
          }

          std::string formula = strip_tex_wrappers(raw);
          if (formula.empty()) {
            bot.send_message(msg.channel_id, bronx::emj_no + " Please provide a LaTeX formula.");
            return;
          }

          std::thread([&bot, ch = msg.channel_id, formula]() {
            send_rendered_latex(bot, ch, formula);
          }).detach();
        },
        "Utility"
    });

    // TeXit-style auto-catcher for $$...$$ and [;...;]
    bot.on_message([this, &bot](const stoatpp::events::Message &e) {
      if (e.author.id.empty() || e.author.id == bot.current_user().id || e.author.bot)
        return;

      // Ignore explicit commands (e.g. .latex $$x^2$$)
      if (!e.prefix.empty() && e.content.rfind(e.prefix, 0) == 0)
        return;
      std::string def_pfx = bot.config().command_prefix;
      if (!def_pfx.empty() && e.content.rfind(def_pfx, 0) == 0)
        return;

      std::string target_id = resolve_target_id(bot, e.channel_id, e.server_id, false);
      {
        std::lock_guard<std::mutex> lock(cfg_mutex_);
        if (!auto_enabled_servers_.count(target_id) && !auto_enabled_servers_.count(e.channel_id))
          return;
      }

      // Extract $$...$$ or [;...;] blocks
      std::vector<std::string> blocks;
      std::string remaining;
      const std::string &s = e.content;
      size_t pos = 0;
      while (pos < s.size()) {
        size_t start_dd = s.find("$$", pos);
        size_t start_tx = s.find("[;", pos);
        size_t start = std::min(start_dd, start_tx);
        if (start == std::string::npos) {
          remaining += s.substr(pos);
          break;
        }
        bool is_dd = (start == start_dd);
        std::string close_tok = is_dd ? "$$" : ";]";
        size_t end = s.find(close_tok, start + 2);
        if (end == std::string::npos) {
          remaining += s.substr(pos);
          break;
        }
        remaining += s.substr(pos, start - pos);
        std::string inner = strip_tex_wrappers(s.substr(start + 2, end - (start + 2)));
        if (!inner.empty()) {
          blocks.push_back(inner);
        }
        pos = end + 2;
      }

      if (blocks.empty())
        return;

      std::string combined;
      if (blocks.size() == 1) {
        combined = blocks[0];
      } else {
        combined = "\\begin{aligned}";
        for (size_t i = 0; i < blocks.size(); ++i) {
          if (i > 0) combined += " \\\\[6pt] ";
          combined += blocks[i];
        }
        combined += "\\end{aligned}";
      }

      // Trim remaining prose
      size_t a = remaining.find_first_not_of(" \t\r\n");
      if (a == std::string::npos) remaining.clear();
      else remaining = remaining.substr(a, remaining.find_last_not_of(" \t\r\n") - a + 1);

      std::string author_name = e.author.username.empty() ? ("<@" + e.author.id + ">") : ("**" + e.author.username + "**");
      std::string header = author_name + (remaining.empty() ? ":" : (": " + remaining));

      std::thread([&bot, e, combined, header]() {
        std::string err;
        std::string png = render_latex_png(combined, err);
        if (png.empty())
          return;
        std::string file_id = chart::upload_chart(bot, png, "latex.png");
        if (file_id.empty())
          return;

        stoatpp::models::MessagePayload payload;
        payload.content = header;
        nlohmann::json embed = nlohmann::json::object();
        embed["colour"] = "#2B2D31";
        embed["media"] = file_id;
        payload.embeds.push_back(embed);

        bot.send_message(e.channel_id, payload, [&bot, e](stoatpp::models::Message, bool ok) {
          if (ok) {
            bot.delete_message(e.channel_id, e.id);
          }
        });
      }).detach();
    });
  }
};

} // namespace bronx::cogs
