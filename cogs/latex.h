#pragma once
#include "chart_renderer.h"
#include "command_config.h"
#include "helpers.h"
#include "stoatpp/bot_module.h"
#include <algorithm>
#include <atomic>
#include <cairo/cairo.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
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

  // ---- Math Expression Parser for .graph ----
  struct ExprParser {
    std::string s;
    size_t pos = 0;
    double x_val = 0.0;

    void skip_ws() {
      while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++;
    }

    double parse_expr() {
      double v = parse_term();
      while (true) {
        skip_ws();
        if (pos < s.size() && s[pos] == '+') { pos++; v += parse_term(); }
        else if (pos < s.size() && s[pos] == '-') { pos++; v -= parse_term(); }
        else break;
      }
      return v;
    }

    double parse_term() {
      double v = parse_power();
      while (true) {
        skip_ws();
        if (pos < s.size() && s[pos] == '*') { pos++; v *= parse_power(); }
        else if (pos < s.size() && s[pos] == '/') {
          pos++;
          double d = parse_power();
          v = (std::abs(d) < 1e-12) ? NAN : (v / d);
        } else break;
      }
      return v;
    }

    double parse_power() {
      double base = parse_unary();
      skip_ws();
      if (pos < s.size() && s[pos] == '^') {
        pos++;
        double exp_v = parse_power();
        return std::pow(base, exp_v);
      }
      return base;
    }

    double parse_unary() {
      skip_ws();
      if (pos < s.size() && s[pos] == '+') { pos++; return parse_unary(); }
      if (pos < s.size() && s[pos] == '-') { pos++; return -parse_unary(); }
      return parse_primary();
    }

    double parse_primary() {
      skip_ws();
      if (pos < s.size() && s[pos] == '(') {
        pos++;
        double v = parse_expr();
        skip_ws();
        if (pos < s.size() && s[pos] == ')') pos++;
        return v;
      }
      if (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.')) {
        size_t start = pos;
        while (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.')) pos++;
        try { return std::stod(s.substr(start, pos - start)); } catch (...) { return NAN; }
      }
      if (pos < s.size() && std::isalpha(static_cast<unsigned char>(s[pos]))) {
        size_t start = pos;
        while (pos < s.size() && (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '_')) pos++;
        std::string id = s.substr(start, pos - start);
        for (char &c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        skip_ws();
        if (pos < s.size() && s[pos] == '(') {
          pos++;
          double arg = parse_expr();
          skip_ws();
          if (pos < s.size() && s[pos] == ')') pos++;
          if (id == "sin") return std::sin(arg);
          if (id == "cos") return std::cos(arg);
          if (id == "tan") return std::tan(arg);
          if (id == "asin") return std::asin(arg);
          if (id == "acos") return std::acos(arg);
          if (id == "atan") return std::atan(arg);
          if (id == "sinh") return std::sinh(arg);
          if (id == "cosh") return std::cosh(arg);
          if (id == "tanh") return std::tanh(arg);
          if (id == "exp") return std::exp(arg);
          if (id == "ln") return (arg > 0) ? std::log(arg) : NAN;
          if (id == "log" || id == "log10") return (arg > 0) ? std::log10(arg) : NAN;
          if (id == "sqrt") return (arg >= 0) ? std::sqrt(arg) : NAN;
          if (id == "abs") return std::abs(arg);
          if (id == "floor") return std::floor(arg);
          if (id == "ceil") return std::ceil(arg);
          return NAN;
        }
        if (id == "x") return x_val;
        if (id == "pi") return 3.14159265358979323846;
        if (id == "e") return 2.71828182845904523536;
        return NAN;
      }
      return NAN;
    }

    static double eval(const std::string &expr, double x) {
      ExprParser p{expr, 0, x};
      return p.parse_expr();
    }
  };

  static cairo_status_t cairo_write_str_cb(void *closure, const unsigned char *data, unsigned int len) {
    static_cast<std::string *>(closure)->append(reinterpret_cast<const char *>(data), len);
    return CAIRO_STATUS_SUCCESS;
  }

  static std::string render_graph_png(const std::vector<std::string> &exprs, double xmin, double xmax) {
    const int W = 760, H = 440;
    const double pad_l = 64.0, pad_r = 24.0, pad_t = 38.0, pad_b = 46.0;
    const double plot_w = W - pad_l - pad_r;
    const double plot_h = H - pad_t - pad_b;
    const int N = 400;

    std::vector<std::vector<double>> ys(exprs.size(), std::vector<double>(N + 1, NAN));
    double ymin = 1e18, ymax = -1e18;
    for (size_t k = 0; k < exprs.size(); ++k) {
      for (int i = 0; i <= N; ++i) {
        double x = xmin + (xmax - xmin) * (static_cast<double>(i) / N);
        double y = ExprParser::eval(exprs[k], x);
        if (std::isfinite(y) && std::abs(y) < 1e6) {
          ys[k][i] = y;
          if (y < ymin) ymin = y;
          if (y > ymax) ymax = y;
        }
      }
    }
    if (ymin > ymax) { ymin = -1.0; ymax = 1.0; }
    if (std::abs(ymax - ymin) < 1e-6) { ymin -= 1.0; ymax += 1.0; }
    double y_pad = (ymax - ymin) * 0.10;
    ymin -= y_pad;
    ymax += y_pad;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(surf);

    // Dark card background
    cairo_set_source_rgb(cr, 0x1e / 255.0, 0x1f / 255.0, 0x22 / 255.0);
    cairo_paint(cr);

    auto map_x = [&](double x) { return pad_l + ((x - xmin) / (xmax - xmin)) * plot_w; };
    auto map_y = [&](double y) { return pad_t + plot_h - ((y - ymin) / (ymax - ymin)) * plot_h; };

    // Gridlines
    cairo_set_line_width(cr, 1.0);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
    const int ticks = 6;
    for (int i = 0; i <= ticks; ++i) {
      double gx = xmin + (xmax - xmin) * (static_cast<double>(i) / ticks);
      double px = map_x(gx);
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
      cairo_move_to(cr, px, pad_t);
      cairo_line_to(cr, px, pad_t + plot_h);
      cairo_stroke(cr);

      std::ostringstream ss;
      ss << std::setprecision(3) << gx;
      cairo_set_source_rgba(cr, 0.8, 0.82, 0.88, 0.85);
      cairo_move_to(cr, px - 12.0, pad_t + plot_h + 18.0);
      cairo_show_text(cr, ss.str().c_str());

      double gy = ymin + (ymax - ymin) * (static_cast<double>(i) / ticks);
      double py = map_y(gy);
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
      cairo_move_to(cr, pad_l, py);
      cairo_line_to(cr, pad_l + plot_w, py);
      cairo_stroke(cr);

      std::ostringstream yss;
      yss << std::setprecision(3) << gy;
      cairo_set_source_rgba(cr, 0.8, 0.82, 0.88, 0.85);
      cairo_move_to(cr, 8.0, py + 4.0);
      cairo_show_text(cr, yss.str().c_str());
    }

    // X=0 and Y=0 axes
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.40);
    cairo_set_line_width(cr, 1.5);
    if (ymin <= 0.0 && ymax >= 0.0) {
      double y0 = map_y(0.0);
      cairo_move_to(cr, pad_l, y0);
      cairo_line_to(cr, pad_l + plot_w, y0);
      cairo_stroke(cr);
    }
    if (xmin <= 0.0 && xmax >= 0.0) {
      double x0 = map_x(0.0);
      cairo_move_to(cr, x0, pad_t);
      cairo_line_to(cr, x0, pad_t + plot_h);
      cairo_stroke(cr);
    }

    // Clip to plot box and draw curves
    cairo_save(cr);
    cairo_rectangle(cr, pad_l, pad_t, plot_w, plot_h);
    cairo_clip(cr);

    static constexpr double palette[5][3] = {
        {0.345, 0.651, 1.000}, // #58a6ff
        {0.969, 0.506, 0.400}, // #f78166
        {0.247, 0.725, 0.314}, // #3fb950
        {0.824, 0.659, 1.000}, // #d2a8ff
        {0.945, 0.769, 0.059}  // #f1c40f
    };

    cairo_set_line_width(cr, 2.5);
    for (size_t k = 0; k < exprs.size(); ++k) {
      const auto &col = palette[k % 5];
      cairo_set_source_rgb(cr, col[0], col[1], col[2]);
      bool pen_down = false;
      double prev_py = 0.0;
      for (int i = 0; i <= N; ++i) {
        double y = ys[k][i];
        if (!std::isfinite(y)) { pen_down = false; continue; }
        double px = map_x(xmin + (xmax - xmin) * (static_cast<double>(i) / N));
        double py = map_y(y);
        if (pen_down && std::abs(py - prev_py) > plot_h * 0.75) {
          pen_down = false; // avoid vertical asymptote jump lines
        }
        if (!pen_down) {
          cairo_move_to(cr, px, py);
          pen_down = true;
        } else {
          cairo_line_to(cr, px, py);
        }
        prev_py = py;
      }
      cairo_stroke(cr);
    }
    cairo_restore(cr);

    // Legend across top
    double lx = pad_l;
    cairo_set_font_size(cr, 12.0);
    for (size_t k = 0; k < exprs.size(); ++k) {
      const auto &col = palette[k % 5];
      cairo_set_source_rgb(cr, col[0], col[1], col[2]);
      cairo_rectangle(cr, lx, 14.0, 12.0, 12.0);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 0.95, 0.96, 0.98);
      std::string lbl = "y=" + exprs[k];
      cairo_move_to(cr, lx + 17.0, 24.0);
      cairo_show_text(cr, lbl.c_str());
      lx += 28.0 + lbl.size() * 7.2;
      if (lx > W - 80.0) break;
    }

    std::string png_out;
    cairo_surface_write_to_png_stream(surf, cairo_write_str_cb, &png_out);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return png_out;
  }

  static std::string render_typst_png(const std::string &code, std::string &err_out) {
    static std::atomic<uint64_t> counter{0};
    std::string dir = "/tmp/bronx_typst_" + std::to_string(counter.fetch_add(1));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) { err_out = "failed to create temp dir"; return {}; }

    std::string preamble =
        "#set page(width: auto, height: auto, margin: 14pt, fill: rgb(\"#1e1f22\"))\n"
        "#set text(fill: white, size: 12pt)\n"
        "#set line(stroke: white)\n"
        "#set rect(stroke: white)\n"
        "#let graph(fn, xmin: -5, xmax: 5, ymin: none, ymax: none, width: 320pt, height: 180pt, color: rgb(\"#58a6ff\"), samples: 160) = {\n"
        "  let pts = range(0, samples + 1).map(i => {\n"
        "    let x = xmin + (xmax - xmin) * (i / samples)\n"
        "    (x, fn(x))\n"
        "  })\n"
        "  let ys = pts.map(p => p.at(1))\n"
        "  let y0 = if ymin != none { ymin } else { calc.min(..ys) }\n"
        "  let y1 = if ymax != none { ymax } else { calc.max(..ys) }\n"
        "  let span = if y1 == y0 { 1.0 } else { y1 - y0 }\n"
        "  box(width: width, height: height, stroke: 0.75pt + luma(90), inset: 6pt, {\n"
        "    let mapped = pts.map(p => (\n"
        "      ((p.at(0) - xmin) / (xmax - xmin)) * (width - 12pt),\n"
        "      (1.0 - (p.at(1) - y0) / span) * (height - 12pt)\n"
        "    ))\n"
        "    place(curve(stroke: 1.5pt + color, curve.move(mapped.first()), ..mapped.slice(1).map(pt => curve.line(pt))))\n"
        "  })\n"
        "}\n";

    std::string src_path = dir + "/main.typ";
    std::string out_tmpl = dir + "/out-{n}.png";
    std::string out_first = dir + "/out-1.png";
    {
      std::ofstream ofs(src_path);
      ofs << preamble << code << "\n";
    }

    std::string cmd = "typst compile --format png --ppi 220 --root \"" + dir + "\" \"" + src_path + "\" \"" + out_tmpl + "\" 2>\"" + dir + "/err.txt\"";
    int rc = std::system(cmd.c_str());

    std::string png_bytes;
    if (rc == 0 && std::filesystem::exists(out_first)) {
      std::ifstream ifs(out_first, std::ios::binary);
      png_bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    } else {
      std::ifstream efs(dir + "/err.txt");
      std::string err_txt((std::istreambuf_iterator<char>(efs)), std::istreambuf_iterator<char>());
      err_out = err_txt.empty() ? "typst compile failed" : err_txt.substr(0, 300);
    }

    std::filesystem::remove_all(dir, ec);
    return png_bytes;
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

    // .typst / .typ command (local Typst compiler)
    bot.register_command({
        "typst",
        {"typ"},
        "Render Typst markup, math, or #graph(...) plots to PNG.",
        "typst <code...>",
        {"<code...>"},
        [](stoatpp::cluster &bot, const stoatpp::events::Message &msg, const std::vector<std::string> &args) {
          std::string raw;
          for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) raw += " ";
            raw += args[i];
          }
          std::string code = strip_tex_wrappers(raw);
          if (code.empty()) {
            bot.send_message(msg.channel_id,
                "📐 **Typst Renderer**\n"
                "• `.typst $ integral_0^infinity e^(-x^2) dif x = sqrt(pi)/2 $`\n"
                "• `.typst #graph(x => calc.sin(x), xmin: -6.28, xmax: 6.28)`");
            return;
          }

          std::thread([&bot, ch = msg.channel_id, code]() {
            std::string err;
            std::string png = render_typst_png(code, err);
            if (png.empty()) {
              bot.send_message(ch, bronx::emj_no + " Typst error:\n```\n" + err + "\n```");
              return;
            }
            std::string file_id = chart::upload_chart(bot, png, "typst.png");
            if (file_id.empty()) {
              bot.send_message(ch, bronx::emj_no + " Failed to upload Typst image.");
              return;
            }
            stoatpp::models::MessagePayload payload;
            payload.content = " ";
            nlohmann::json embed = nlohmann::json::object();
            embed["colour"] = "#2B2D31";
            embed["media"] = file_id;
            payload.embeds.push_back(embed);
            bot.send_message(ch, payload);
          }).detach();
        },
        "Utility"
    });

    // .graph / .plot / .fn command (Cairo math function plotter)
    bot.register_command({
        "graph",
        {"plot", "fn"},
        "Plot one or more math functions f(x) to a PNG graph.",
        "graph <expr1, expr2...> [xmin..xmax]",
        {"<expr...>"},
        [](stoatpp::cluster &bot, const stoatpp::events::Message &msg, const std::vector<std::string> &args) {
          std::string raw;
          for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) raw += " ";
            raw += args[i];
          }
          size_t fa = raw.find_first_not_of(" \t\r\n");
          if (fa == std::string::npos) raw.clear();
          else raw = raw.substr(fa, raw.find_last_not_of(" \t\r\n") - fa + 1);

          if (raw.empty()) {
            bot.send_message(msg.channel_id,
                "📈 **Math Function Grapher**\n"
                "• `.graph sin(x)`\n"
                "• `.graph sin(x), cos(x) -6.28..6.28`\n"
                "• `.graph x^2 - 4*x + 3 -2..6`");
            return;
          }

          double xmin = -10.0, xmax = 10.0;
          size_t sp = raw.rfind(' ');
          if (sp != std::string::npos) {
            std::string tail = raw.substr(sp + 1);
            size_t dots = tail.find("..");
            if (dots != std::string::npos) {
              try {
                double a = std::stod(tail.substr(0, dots));
                double b = std::stod(tail.substr(dots + 2));
                if (b > a) {
                  xmin = a;
                  xmax = b;
                  raw = raw.substr(0, sp);
                }
              } catch (...) {}
            }
          }

          std::vector<std::string> exprs;
          std::stringstream ss(raw);
          std::string item;
          while (std::getline(ss, item, ',') && exprs.size() < 5) {
            size_t a = item.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) continue;
            size_t b = item.find_last_not_of(" \t\r\n");
            exprs.push_back(item.substr(a, b - a + 1));
          }
          if (exprs.empty()) {
            bot.send_message(msg.channel_id, bronx::emj_no + " Please provide a math expression like `sin(x)`.");
            return;
          }

          std::thread([&bot, ch = msg.channel_id, exprs, xmin, xmax]() {
            std::string png = render_graph_png(exprs, xmin, xmax);
            if (png.empty()) {
              bot.send_message(ch, bronx::emj_no + " Failed to render graph.");
              return;
            }
            std::string file_id = chart::upload_chart(bot, png, "graph.png");
            if (file_id.empty()) {
              bot.send_message(ch, bronx::emj_no + " Failed to upload graph.");
              return;
            }
            stoatpp::models::MessagePayload payload;
            payload.content = " ";
            nlohmann::json embed = nlohmann::json::object();
            embed["colour"] = "#2B2D31";
            embed["media"] = file_id;
            payload.embeds.push_back(embed);
            bot.send_message(ch, payload);
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
