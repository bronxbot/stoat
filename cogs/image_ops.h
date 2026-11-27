#pragma once
#include "helpers.h"
#include "stoatpp/bot_module.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace bronx::cogs {

// Forward declarations of helpers defined in media.h
inline std::string shell_escape(const std::string &str);
inline std::string find_url(const std::string &text);
inline std::string resolve_message_media_url(stoatpp::cluster &bot,
                                             const stoatpp::events::Message &msg);
inline std::string resolve_message_media_url(stoatpp::cluster &bot,
                                             const stoatpp::models::Message &msg);
inline bool download_file_system(const std::string &url,
                                 const std::string &out_path);

namespace image_ops {

inline std::string join_args_str(const std::vector<std::string> &args,
                                 size_t start = 0) {
  std::string res;
  for (size_t i = start; i < args.size(); ++i) {
    if (i > start)
      res += ' ';
    res += args[i];
  }
  return res;
}

inline std::string escape_im_text(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\')
      out += "\\\\";
    else if (c == '"')
      out += "\\\"";
    else if (c == '%')
      out += "%%";
    else
      out += c;
  }
  return out;
}

inline std::vector<std::string>
resolve_all_from_message_full(stoatpp::cluster &bot,
                              const stoatpp::models::Message &m) {
  std::vector<std::string> urls;
  if (m.attachments.is_array()) {
    for (const auto &att : m.attachments) {
      std::string tag = att.value("tag", "");
      std::string id = att.value("id", "");
      if (id.empty())
        id = att.value("_id", "");
      std::string filename = att.value("filename", "");
      if (!tag.empty() && !id.empty()) {
        urls.push_back(bot.config().cdn_url + "/" + tag + "/" + id + "/" +
                       filename);
      }
    }
  }
  if (m.embeds.is_array()) {
    for (const auto &emb : m.embeds) {
      if (emb.contains("image") && emb["image"].is_object()) {
        std::string u = emb["image"].value("url", "");
        if (!u.empty())
          urls.push_back(u);
      }
      if (emb.contains("media") && emb["media"].is_object()) {
        std::string u = emb["media"].value("url", "");
        if (!u.empty())
          urls.push_back(u);
      }
      if (emb.contains("icon_url") && emb["icon_url"].is_string()) {
        std::string u = emb["icon_url"].get<std::string>();
        if (!u.empty())
          urls.push_back(u);
      }
    }
  }
  if (urls.empty() && m.content) {
    std::string u = find_url(*m.content);
    if (!u.empty())
      urls.push_back(u);
  }
  return urls;
}

inline std::vector<std::string>
resolve_image_urls(stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                   const std::string &explicit_url = "") {
  std::vector<std::string> urls;
  if (!explicit_url.empty() && explicit_url.rfind("http", 0) == 0) {
    urls.push_back(explicit_url);
    return urls;
  }

  for (const auto &reply_id : msg.replies) {
    auto res = bot.rest().get_message(msg.channel_id, reply_id);
    if (res.success()) {
      auto replied = stoatpp::models::Message::from_json(res.body);
      auto found = resolve_all_from_message_full(bot, replied);
      urls.insert(urls.end(), found.begin(), found.end());
    }
  }
  if (!urls.empty()) {
    if (urls.size() > 5)
      urls.resize(5);
    return urls;
  }

  if (msg.raw.contains("attachments") && msg.raw["attachments"].is_array()) {
    for (const auto &att : msg.raw["attachments"]) {
      std::string tag = att.value("tag", "");
      std::string id = att.value("id", "");
      if (id.empty())
        id = att.value("_id", "");
      std::string filename = att.value("filename", "");
      if (!tag.empty() && !id.empty()) {
        urls.push_back(bot.config().cdn_url + "/" + tag + "/" + id + "/" +
                       filename);
      }
    }
  }
  if (urls.empty()) {
    std::string u = find_url(msg.content);
    if (!u.empty())
      urls.push_back(u);
  }
  if (!urls.empty()) {
    if (urls.size() > 5)
      urls.resize(5);
    return urls;
  }

  // Fallback: scan recent channel messages (Assyst-style)
  stoatpp::models::MessageQuery query;
  query.limit = 30;
  query.sort = "Latest";
  auto hist = bot.rest().get_messages(msg.channel_id, query);
  if (hist.success()) {
    const nlohmann::json *arr = nullptr;
    if (hist.body.is_array()) {
      arr = &hist.body;
    } else if (hist.body.is_object() && hist.body.contains("messages") &&
               hist.body["messages"].is_array()) {
      arr = &hist.body["messages"];
    }
    if (arr) {
      for (const auto &item : *arr) {
        auto m = stoatpp::models::Message::from_json(item);
        if (m.id == msg.id)
          continue;
        auto found = resolve_all_from_message_full(bot, m);
        if (!found.empty()) {
          if (found.size() > 5)
            found.resize(5);
          return found;
        }
      }
    }
  }
  return urls;
}

inline std::string resolve_image_url(stoatpp::cluster &bot,
                                     const stoatpp::events::Message &msg,
                                     const std::string &explicit_url = "") {
  auto urls = resolve_image_urls(bot, msg, explicit_url);
  return urls.empty() ? "" : urls[0];
}

inline bool detect_animated_gif(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  char hdr[6] = {};
  f.read(hdr, 6);
  if (f.gcount() < 6)
    return false;
  if (std::string(hdr, 4) != "GIF8")
    return false;

  std::string cmd = "identify -format '%n\n' " + shell_escape(path) +
                    " 2>/dev/null | head -n1";
  FILE *p = popen(cmd.c_str(), "r");
  if (!p)
    return false;
  char buf[32] = {};
  std::string out;
  if (fgets(buf, sizeof(buf), p))
    out = buf;
  pclose(p);
  try {
    return std::stoi(out) > 1;
  } catch (...) {
    return false;
  }
}

inline bool get_image_dimensions(const std::string &path, int &w, int &h) {
  std::string cmd = "identify -format '%w %h\n' " + shell_escape(path) +
                    " 2>/dev/null | head -n1";
  FILE *p = popen(cmd.c_str(), "r");
  if (!p)
    return false;
  char buf[64] = {};
  std::string out;
  if (fgets(buf, sizeof(buf), p))
    out = buf;
  pclose(p);
  std::istringstream iss(out);
  return static_cast<bool>(iss >> w >> h) && w > 0 && h > 0;
}

inline int run_convert(const std::string &in_path, bool is_gif,
                       const std::string &ops, const std::string &out_path) {
  std::string resize_cap = is_gif ? "-resize '320x320>' " : "-resize '512x512>' ";
  std::string cmd =
      "/usr/bin/convert -limit memory 256MiB -limit map 512MiB " +
      shell_escape(in_path) + (is_gif ? " -coalesce " : " ") +
      resize_cap + ops + (is_gif ? " -layers OptimizeFrame " : " ") +
      shell_escape(out_path) + " >/dev/null 2>&1";
  return std::system(cmd.c_str());
}

inline bool apply_caption(const std::string &in_path, bool is_gif,
                          const std::string &text, const std::string &temp_dir,
                          const std::string &out_path) {
  int w = 500, h = 500;
  if (!get_image_dimensions(in_path, w, h))
    w = 500;
  int target_w = std::clamp(w, 240, 800);
  int pts = std::clamp(target_w / 14, 18, 52);
  int pad = std::max(10, pts / 3);
  std::string banner = temp_dir + "/banner.png";

  std::string banner_cmd =
      "/usr/bin/convert -limit memory 256MiB -background white -fill black "
      "-font DejaVu-Sans-Bold -pointsize " +
      std::to_string(pts) + " -size " + std::to_string(target_w - pad * 2) +
      "x -gravity center caption:" + shell_escape(escape_im_text(text)) +
      " -bordercolor white -border " + std::to_string(pad) + " " +
      shell_escape(banner) + " >/dev/null 2>&1";
  if (std::system(banner_cmd.c_str()) != 0 ||
      !std::filesystem::exists(banner)) {
    return false;
  }

  int bw = target_w, bh = 80;
  get_image_dimensions(banner, bw, bh);

  if (!is_gif) {
    std::string cmd =
        "/usr/bin/convert -limit memory 256MiB " + shell_escape(banner) + " " +
        shell_escape(in_path) + " -resize " + std::to_string(bw) + "x" +
        " -append " + shell_escape(out_path) + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
  }

  std::string cmd =
      "/usr/bin/convert -limit memory 256MiB -limit map 512MiB " +
      shell_escape(in_path) + " -coalesce -resize " + std::to_string(bw) +
      "x -gravity north -background white -splice 0x" + std::to_string(bh) +
      " null: " + shell_escape(banner) +
      " -gravity north -layers composite -layers Optimize " +
      shell_escape(out_path) + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

inline bool apply_meme(const std::string &in_path, bool is_gif,
                       const std::string &top_text,
                       const std::string &bottom_text,
                       const std::string &out_path) {
  int w = 500, h = 500;
  if (!get_image_dimensions(in_path, w, h)) {
    w = 500;
    h = 500;
  }
  int target_w = std::clamp(w, 240, 800);
  int pts = std::clamp(target_w / 12, 20, 56);
  int stroke = std::max(2, pts / 12);

  std::string ops = "-resize " + std::to_string(target_w) +
                    "x -font DejaVu-Sans-Bold -pointsize " +
                    std::to_string(pts) +
                    " -fill white -stroke black -strokewidth " +
                    std::to_string(stroke);

  auto upper_str = [](std::string s) {
    for (char &c : s)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  };

  if (!top_text.empty()) {
    ops += " -gravity north -annotate +0+12 " +
           shell_escape(escape_im_text(upper_str(top_text)));
  }
  if (!bottom_text.empty()) {
    ops += " -gravity south -annotate +0+12 " +
           shell_escape(escape_im_text(upper_str(bottom_text)));
  }
  return run_convert(in_path, is_gif, ops, out_path) == 0;
}

inline bool apply_speechbubble(const std::string &in_path, bool is_gif,
                               const std::string &temp_dir,
                               const std::string &out_path) {
  int w = 400, h = 400;
  if (!get_image_dimensions(in_path, w, h)) {
    w = 400;
    h = 400;
  }
  int target_w = std::clamp(w, 160, 800);
  double scale = static_cast<double>(target_w) / std::max(1, w);
  int target_h = std::max(120, static_cast<int>(h * scale));
  int arc_h = std::max(28, target_h / 6);

  std::string mask = temp_dir + "/sb_mask.png";
  int tip_x = static_cast<int>(target_w * 0.28);
  int tip_y = arc_h + std::max(24, target_h / 7);
  int base_l = static_cast<int>(target_w * 0.24);
  int base_r = static_cast<int>(target_w * 0.40);

  std::ostringstream mask_cmd;
  mask_cmd << "/usr/bin/convert -size " << target_w << "x" << target_h
           << " xc:white -fill black -draw \"ellipse " << (target_w / 2)
           << ",0 " << (target_w / 2 + 10) << "," << arc_h
           << " 0,360 polygon " << base_l << "," << (arc_h / 2) << " " << tip_x
           << "," << tip_y << " " << base_r << "," << (arc_h / 2) << "\" "
           << shell_escape(mask) << " >/dev/null 2>&1";
  if (std::system(mask_cmd.str().c_str()) != 0)
    return false;

  if (!is_gif) {
    std::string cmd =
        "/usr/bin/convert -limit memory 256MiB " + shell_escape(in_path) +
        " -resize " + std::to_string(target_w) + "x" +
        std::to_string(target_h) + "! -alpha set " + shell_escape(mask) +
        " -compose CopyOpacity -composite " + shell_escape(out_path) +
        " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
  }

  std::string cmd =
      "/usr/bin/convert -limit memory 256MiB -limit map 512MiB " +
      shell_escape(in_path) + " -coalesce -resize " + std::to_string(target_w) +
      "x" + std::to_string(target_h) + "! -alpha set null: " +
      shell_escape(mask) +
      " -compose CopyOpacity -layers composite -layers Optimize " +
      shell_escape(out_path) + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

inline bool apply_gloss(const std::string &in_path, bool is_gif,
                        const std::string &temp_dir,
                        const std::string &out_path) {
  int w = 400, h = 400;
  if (!get_image_dimensions(in_path, w, h)) {
    w = 400;
    h = 400;
  }
  int target_w = std::clamp(w, 120, 800);
  double scale = static_cast<double>(target_w) / std::max(1, w);
  int target_h = std::max(120, static_cast<int>(h * scale));

  std::string overlay = temp_dir + "/gloss.png";
  std::ostringstream ocmd;
  ocmd << "/usr/bin/convert -size " << target_w << "x" << target_h
       << " xc:none -fill 'rgba(255,255,255,0.42)' -draw \"ellipse "
       << (target_w / 2) << "," << (target_h / 10) << " " << (target_w * 0.65)
       << "," << (target_h * 0.40) << " 0,360\" -blur 0x6 "
       << shell_escape(overlay) << " >/dev/null 2>&1";
  if (std::system(ocmd.str().c_str()) != 0)
    return false;

  if (!is_gif) {
    std::string cmd =
        "/usr/bin/convert -limit memory 256MiB " + shell_escape(in_path) +
        " -resize " + std::to_string(target_w) + "x" +
        std::to_string(target_h) +
        "! -modulate 105,125,100 -sigmoidal-contrast 3,50% " +
        shell_escape(overlay) + " -compose Over -composite " +
        shell_escape(out_path) + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
  }

  std::string cmd =
      "/usr/bin/convert -limit memory 256MiB -limit map 512MiB " +
      shell_escape(in_path) + " -coalesce -resize " + std::to_string(target_w) +
      "x" + std::to_string(target_h) +
      "! -modulate 105,125,100 -sigmoidal-contrast 3,50% null: " +
      shell_escape(overlay) + " -layers composite -layers Optimize " +
      shell_escape(out_path) + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

inline bool apply_effect(const std::string &effect, const std::string &in_path,
                         bool &is_gif, const std::string &temp_dir,
                         const std::string &out_path,
                         const std::string &arg = "") {
  if (effect == "hflip" || effect == "flop") {
    return run_convert(in_path, is_gif, "-flop", out_path) == 0;
  }
  if (effect == "vflip" || effect == "flip") {
    return run_convert(in_path, is_gif, "-flip", out_path) == 0;
  }
  if (effect == "rotate") {
    int deg = 90;
    if (!arg.empty()) {
      try {
        deg = std::stoi(arg);
      } catch (...) {
      }
    }
    return run_convert(in_path, is_gif,
                       "-background none -rotate " + std::to_string(deg),
                       out_path) == 0;
  }
  if (effect == "blur") {
    return run_convert(in_path, is_gif, "-blur 0x6", out_path) == 0;
  }
  if (effect == "zoomblur") {
    return run_convert(in_path, is_gif, "-distort SRT 1.05 -blur 0x3",
                       out_path) == 0;
  }
  if (effect == "pixelate" || effect == "pixel") {
    return run_convert(in_path, is_gif, "-scale 10% -scale 1000%", out_path) ==
           0;
  }
  if (effect == "deepfry") {
    return run_convert(
               in_path, is_gif,
               "-modulate 115,240,100 -sigmoidal-contrast 8,50% -sharpen 0x3",
               out_path) == 0;
  }
  if (effect == "invert" || effect == "negate") {
    return run_convert(in_path, is_gif, "-negate", out_path) == 0;
  }
  if (effect == "grayscale" || effect == "gray" || effect == "bw") {
    return run_convert(in_path, is_gif, "-colorspace Gray", out_path) == 0;
  }
  if (effect == "sepia") {
    return run_convert(in_path, is_gif, "-sepia-tone 80%", out_path) == 0;
  }
  if (effect == "spread") {
    return run_convert(in_path, is_gif, "-spread 8", out_path) == 0;
  }
  if (effect == "oilpaint" || effect == "paint") {
    return run_convert(in_path, is_gif, "-paint 4", out_path) == 0;
  }
  if (effect == "charcoal" || effect == "sketch") {
    return run_convert(in_path, is_gif, "-charcoal 3", out_path) == 0;
  }
  if (effect == "edge" || effect == "sobel") {
    return run_convert(in_path, is_gif, "-colorspace Gray -edge 2", out_path) ==
           0;
  }
  if (effect == "posterize") {
    return run_convert(in_path, is_gif, "-posterize 4", out_path) == 0;
  }
  if (effect == "solarize") {
    return run_convert(in_path, is_gif, "-solarize 50%", out_path) == 0;
  }
  if (effect == "noise") {
    return run_convert(in_path, is_gif, "-attenuate 1.5 +noise Gaussian",
                       out_path) == 0;
  }
  if (effect == "jpeg") {
    if (is_gif) {
      return run_convert(in_path, true, "-posterize 6 -blur 0x1", out_path) == 0;
    }
    std::string jpg = temp_dir + "/crunch.jpg";
    std::string c1 = "/usr/bin/convert -limit memory 256MiB " +
                     shell_escape(in_path) + " -resize '600x600>' -quality 4 " +
                     shell_escape(jpg) + " >/dev/null 2>&1";
    std::string c2 = "/usr/bin/convert " + shell_escape(jpg) + " " +
                     shell_escape(out_path) + " >/dev/null 2>&1";
    return std::system(c1.c_str()) == 0 && std::system(c2.c_str()) == 0;
  }
  if (effect == "swirl") {
    return run_convert(in_path, is_gif, "-swirl 180", out_path) == 0;
  }
  if (effect == "implode") {
    return run_convert(in_path, is_gif, "-implode 0.6", out_path) == 0;
  }
  if (effect == "fisheye" || effect == "bulge") {
    return run_convert(in_path, is_gif, "-implode -0.6", out_path) == 0;
  }
  if (effect == "wave") {
    return run_convert(in_path, is_gif, "-background none -wave 8x70",
                       out_path) == 0;
  }
  if (effect == "magik") {
    std::string cap = is_gif ? "-resize '240x240>' " : "";
    return run_convert(in_path, is_gif,
                       cap + "-liquid-rescale 50%x50%! -resize 200%",
                       out_path) == 0;
  }
  if (effect == "stretch") {
    return run_convert(in_path, is_gif, "-resize 200%x70%!", out_path) == 0;
  }
  if (effect == "squish") {
    return run_convert(in_path, is_gif, "-resize 60%x160%!", out_path) == 0;
  }
  if (effect == "gloss" || effect == "shiny") {
    return apply_gloss(in_path, is_gif, temp_dir, out_path);
  }
  if (effect == "speechbubble" || effect == "sb") {
    return apply_speechbubble(in_path, is_gif, temp_dir, out_path);
  }
  if (effect == "rgb") {
    std::string input_flags =
        is_gif ? "-stream_loop -1 -t 2.0 -i " : "-loop 1 -t 2.0 -i ";
    std::string cmd =
        "/usr/bin/ffmpeg -y " + input_flags + shell_escape(in_path) +
        " -vf \"scale='min(256,iw)':-2,hue=H=2*PI*t/2.0:s=1.5,fps=12,split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse\" " +
        shell_escape(out_path) + " >/dev/null 2>&1";
    if (std::system(cmd.c_str()) == 0) {
      is_gif = true;
      return true;
    }
    return false;
  }
  if (effect == "spin") {
    std::string input_flags =
        is_gif ? "-stream_loop -1 -t 1.8 -i " : "-loop 1 -t 1.8 -i ";
    std::string cmd =
        "/usr/bin/ffmpeg -y " + input_flags + shell_escape(in_path) +
        " -vf \"scale='min(240,iw)':-2,format=rgba,rotate=2*PI*t/1.8:fillcolor=none,fps=12,split[s0][s1];[s0]palettegen=reserve_transparent=on[p];[s1][p]paletteuse\" " +
        shell_escape(out_path) + " >/dev/null 2>&1";
    if (std::system(cmd.c_str()) == 0) {
      is_gif = true;
      return true;
    }
    return false;
  }
  return false;
}

inline void upload_and_send_many(
    stoatpp::cluster &bot, const std::string &channel_id,
    const std::vector<std::pair<std::string, bool>> &files,
    const std::string &caption_msg = "") {
  stoatpp::models::MessagePayload payload;
  if (!caption_msg.empty())
    payload.content = caption_msg;

  int attached = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    const auto &[file_path, is_gif] = files[i];
    if (!std::filesystem::exists(file_path) ||
        std::filesystem::file_size(file_path) == 0) {
      continue;
    }
    std::ifstream f(file_path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    f.close();

    std::string fname =
        "result_" + std::to_string(i + 1) + (is_gif ? ".gif" : ".png");
    std::string mime = is_gif ? "image/gif" : "image/png";
    auto up_res = bot.rest().upload_file("/attachments", fname, data, mime);
    if (up_res.success() && up_res.body.contains("id") &&
        up_res.body["id"].is_string()) {
      payload.add_attachment(up_res.body["id"].get<std::string>());
      ++attached;
    }
  }

  if (attached > 0) {
    bot.send_message(channel_id, payload);
  } else {
    bot.send_message(channel_id,
                     "" + bronx::emj_no + " failed to upload processed image(s).");
  }
}

inline void upload_and_send(stoatpp::cluster &bot,
                            const std::string &channel_id,
                            const std::string &file_path, bool is_gif,
                            const std::string &caption_msg = "") {
  upload_and_send_many(bot, channel_id, {{file_path, is_gif}}, caption_msg);
}

inline void register_image_commands(stoatpp::cluster &bot) {
  auto register_simple = [&bot](const std::string &name,
                                const std::vector<std::string> &aliases,
                                const std::string &desc) {
    bot.register_command(
        {name,
         aliases,
         desc,
         name + " [url]",
         {"[url]"},
         [name](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &args) {
           std::string explicit_url;
           std::string extra_arg;
           for (const auto &a : args) {
             if (explicit_url.empty() && a.rfind("http", 0) == 0)
               explicit_url = a;
             else if (extra_arg.empty())
               extra_arg = a;
           }

           std::thread([&bot, msg, name, explicit_url, extra_arg]() {
             auto urls = resolve_image_urls(bot, msg, explicit_url);
             if (urls.empty()) {
               bot.send_message(
                   msg.channel_id,
                   "" + bronx::emj_no +
                       " no image found (attach, reply, paste a URL, or send one in chat first).");
               return;
             }
             bot.begin_typing(msg.channel_id);

             std::random_device rd;
             std::string temp_dir =
                 "/tmp/bronx_img_" + std::to_string(rd());
             std::filesystem::create_directories(temp_dir);

             std::vector<std::pair<std::string, bool>> outputs;
             for (size_t idx = 0; idx < urls.size(); ++idx) {
               std::string in_path = temp_dir + "/input_" + std::to_string(idx);
               if (!download_file_system(urls[idx], in_path))
                 continue;

               bool is_gif = detect_animated_gif(in_path);
               bool will_be_gif = is_gif || name == "rgb" || name == "spin";
               std::string out_path = temp_dir + "/out_" +
                                      std::to_string(idx) +
                                      (will_be_gif ? ".gif" : ".png");
               if (apply_effect(name, in_path, is_gif, temp_dir, out_path,
                                extra_arg)) {
                 outputs.emplace_back(out_path, is_gif);
               }
             }

             if (outputs.empty()) {
               bot.send_message(msg.channel_id,
                                "" + bronx::emj_no +
                                    " failed to apply `" + name + "`.");
               std::filesystem::remove_all(temp_dir);
               return;
             }

             upload_and_send_many(bot, msg.channel_id, outputs);
             std::filesystem::remove_all(temp_dir);
           }).detach();
         },
         "Image"});
  };

  register_simple("hflip", {"flop"}, "Mirror an image horizontally.");
  register_simple("vflip", {"flip"}, "Flip an image vertically.");
  register_simple("rotate", {}, "Rotate an image (default 90 degrees).");
  register_simple("blur", {}, "Apply Gaussian blur to an image.");
  register_simple("zoomblur", {"zoom"}, "Apply radial zoom blur.");
  register_simple("pixelate", {"pixel"}, "Pixelate an image.");
  register_simple("deepfry", {"fry"}, "Deepfry an image.");
  register_simple("invert", {"negate"}, "Invert image colors.");
  register_simple("grayscale", {"gray", "bw"}, "Convert an image to grayscale.");
  register_simple("sepia", {}, "Apply sepia tone.");
  register_simple("spread", {"smear"}, "Smear/scatter pixels across an image.");
  register_simple("oilpaint", {"paint"}, "Apply an oil painting effect.");
  register_simple("charcoal", {"sketch"}, "Convert an image to a charcoal sketch.");
  register_simple("edge", {"sobel"}, "Highlight image edges.");
  register_simple("posterize", {}, "Reduce color palette to poster bands.");
  register_simple("solarize", {}, "Solarize an image.");
  register_simple("noise", {}, "Add heavy grain/noise to an image.");
  register_simple("jpeg", {"needs-more-jpeg"}, "Crush an image with heavy JPEG compression.");
  register_simple("swirl", {}, "Swirl the center of an image.");
  register_simple("implode", {"pinch"}, "Pinch/implode the center of an image.");
  register_simple("fisheye", {"bulge"}, "Bulge an image like a fisheye lens.");
  register_simple("wave", {}, "Apply a wave distortion.");
  register_simple("magik", {"liquid"}, "Content-aware liquid-rescale distortion.");
  register_simple("stretch", {"wide"}, "Stretch an image horizontally.");
  register_simple("squish", {}, "Squish an image horizontally.");
  register_simple("gloss", {"shiny"}, "Add a glossy highlight to an image.");
  register_simple("speechbubble", {"sb"}, "Cut a speech bubble out of the top of an image.");
  register_simple("rgb", {"rainbow"}, "Animate rainbow hue cycling as a GIF.");
  register_simple("spin", {}, "Spin an image as an animated GIF.");

  // White rectangle + black text caption (Assyst/iFunny style)
  bot.register_command(
      {"caption",
       {"cap"},
       "Add a white top caption bar with black text to an image or GIF.",
       "caption <text>",
       {"<text>"},
       [](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
          const std::vector<std::string> &args) {
         if (args.empty()) {
           bot.send_message(msg.channel_id,
                            "" + bronx::emj_warning + " Usage: `.caption <text>`");
           return;
         }
         std::string explicit_url;
         std::vector<std::string> text_parts;
         for (const auto &a : args) {
           if (explicit_url.empty() && a.rfind("http", 0) == 0)
             explicit_url = a;
           else
             text_parts.push_back(a);
         }
         std::string text = join_args_str(text_parts);
         if (text.empty()) {
           bot.send_message(msg.channel_id,
                            "" + bronx::emj_warning + " Please provide caption text.");
           return;
         }

         std::thread([&bot, msg, explicit_url, text]() {
           auto urls = resolve_image_urls(bot, msg, explicit_url);
           if (urls.empty()) {
             bot.send_message(msg.channel_id,
                              "" + bronx::emj_no + " no image found.");
             return;
           }
           bot.begin_typing(msg.channel_id);

           std::random_device rd;
           std::string temp_dir = "/tmp/bronx_img_" + std::to_string(rd());
           std::filesystem::create_directories(temp_dir);

           std::vector<std::pair<std::string, bool>> outputs;
           for (size_t idx = 0; idx < urls.size(); ++idx) {
             std::string in_path = temp_dir + "/input_" + std::to_string(idx);
             if (!download_file_system(urls[idx], in_path))
               continue;

             bool is_gif = detect_animated_gif(in_path);
             std::string out_path = temp_dir + "/out_" + std::to_string(idx) +
                                    (is_gif ? ".gif" : ".png");
             if (apply_caption(in_path, is_gif, text, temp_dir, out_path)) {
               outputs.emplace_back(out_path, is_gif);
             }
           }

           if (outputs.empty()) {
             bot.send_message(msg.channel_id,
                              "" + bronx::emj_no + " failed to render caption.");
             std::filesystem::remove_all(temp_dir);
             return;
           }

           upload_and_send_many(bot, msg.channel_id, outputs);
           std::filesystem::remove_all(temp_dir);
         }).detach();
       },
       "Image"});

  // Classic Impact-style top/bottom meme text
  bot.register_command(
      {"meme",
       {"toptext"},
       "Add classic top/bottom meme text (separate with |).",
       "meme <top text> [| bottom text]",
       {"<text>"},
       [](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
          const std::vector<std::string> &args) {
         if (args.empty()) {
           bot.send_message(msg.channel_id,
                            "" + bronx::emj_warning +
                                " Usage: `.meme <top text> | <bottom text>`");
           return;
         }
         std::string explicit_url;
         std::vector<std::string> text_parts;
         for (const auto &a : args) {
           if (explicit_url.empty() && a.rfind("http", 0) == 0)
             explicit_url = a;
           else
             text_parts.push_back(a);
         }
         std::string raw = join_args_str(text_parts);
         if (raw.empty()) {
           bot.send_message(msg.channel_id,
                            "" + bronx::emj_warning + " Please provide meme text.");
           return;
         }

         std::string top_text, bottom_text;
         size_t sep = raw.find('|');
         if (sep == std::string::npos)
           sep = raw.find(';');
         if (sep != std::string::npos) {
           top_text = raw.substr(0, sep);
           bottom_text = raw.substr(sep + 1);
         } else {
           top_text = raw;
         }
         auto trim = [](std::string &s) {
           while (!s.empty() && s.front() == ' ')
             s.erase(s.begin());
           while (!s.empty() && s.back() == ' ')
             s.pop_back();
         };
         trim(top_text);
         trim(bottom_text);

         std::thread([&bot, msg, explicit_url, top_text, bottom_text]() {
           auto urls = resolve_image_urls(bot, msg, explicit_url);
           if (urls.empty()) {
             bot.send_message(msg.channel_id,
                              "" + bronx::emj_no + " no image found.");
             return;
           }
           bot.begin_typing(msg.channel_id);

           std::random_device rd;
           std::string temp_dir = "/tmp/bronx_img_" + std::to_string(rd());
           std::filesystem::create_directories(temp_dir);

           std::vector<std::pair<std::string, bool>> outputs;
           for (size_t idx = 0; idx < urls.size(); ++idx) {
             std::string in_path = temp_dir + "/input_" + std::to_string(idx);
             if (!download_file_system(urls[idx], in_path))
               continue;

             bool is_gif = detect_animated_gif(in_path);
             std::string out_path = temp_dir + "/out_" + std::to_string(idx) +
                                    (is_gif ? ".gif" : ".png");
             if (apply_meme(in_path, is_gif, top_text, bottom_text, out_path)) {
               outputs.emplace_back(out_path, is_gif);
             }
           }

           if (outputs.empty()) {
             bot.send_message(msg.channel_id,
                              "" + bronx::emj_no + " failed to render meme.");
             std::filesystem::remove_all(temp_dir);
             return;
           }

           upload_and_send_many(bot, msg.channel_id, outputs);
           std::filesystem::remove_all(temp_dir);
         }).detach();
       },
       "Image"});

  // Assyst-style randomizer: chains 3 random effects sequentially
  bot.register_command(
      {"random",
       {"rand", "chaos", "assyst"},
       "Apply 3 random image effects chained together (Assyst-style).",
       "random [url]",
       {"[url]"},
       [](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
          const std::vector<std::string> &args) {
         std::string explicit_url =
             (!args.empty() && args[0].rfind("http", 0) == 0) ? args[0] : "";

         std::thread([&bot, msg, explicit_url]() {
           auto urls = resolve_image_urls(bot, msg, explicit_url);
           if (urls.empty()) {
             bot.send_message(
                 msg.channel_id,
                 "" + bronx::emj_no +
                     " no image found (attach, reply, paste a URL, or send one in chat first).");
             return;
           }
           bot.begin_typing(msg.channel_id);

           std::random_device rd;
           std::mt19937 gen(rd());
           std::string temp_dir = "/tmp/bronx_img_" + std::to_string(rd());
           std::filesystem::create_directories(temp_dir);

           std::vector<std::string> pool = {
               "hflip",    "vflip",    "rotate",  "blur",    "zoomblur",
               "pixelate", "deepfry",  "invert",  "grayscale","sepia",
               "spread",   "oilpaint", "charcoal","edge",    "posterize",
               "solarize", "noise",    "jpeg",    "swirl",   "implode",
               "fisheye",  "wave",     "magik",   "stretch", "squish",
               "gloss",    "speechbubble", "rgb", "spin"};
           std::shuffle(pool.begin(), pool.end(), gen);

           // Pick 3 effects and run any GIF-generating effect (rgb/spin) last
           // so earlier static effects don't have to process every GIF frame.
           std::vector<std::string> chosen(pool.begin(), pool.begin() + 3);
           std::stable_partition(chosen.begin(), chosen.end(),
                                 [](const std::string &fx) {
                                   return fx != "rgb" && fx != "spin";
                                 });

           std::vector<std::pair<std::string, bool>> outputs;
           std::vector<std::string> applied_summary;

           for (size_t idx = 0; idx < urls.size(); ++idx) {
             std::string cur_path = temp_dir + "/img" + std::to_string(idx) + "_0";
             if (!download_file_system(urls[idx], cur_path))
               continue;

             bool is_gif = detect_animated_gif(cur_path);
             std::vector<std::string> applied;
             for (size_t i = 0; i < chosen.size(); ++i) {
               const std::string &fx = chosen[i];
               bool next_gif = is_gif || fx == "rgb" || fx == "spin";
               std::string next_path = temp_dir + "/img" + std::to_string(idx) +
                                       "_" + std::to_string(applied.size() + 1) +
                                       (next_gif ? ".gif" : ".png");
               if (apply_effect(fx, cur_path, is_gif, temp_dir, next_path)) {
                 cur_path = next_path;
                 applied.push_back(fx);
               }
             }
             if (!applied.empty()) {
               outputs.emplace_back(cur_path, is_gif);
               if (applied_summary.empty())
                 applied_summary = applied;
             }
           }

           if (outputs.empty()) {
             bot.send_message(msg.channel_id,
                              "" + bronx::emj_no + " failed to apply random effects.");
             std::filesystem::remove_all(temp_dir);
             return;
           }

           std::string summary = "🎲 Applied: ";
           for (size_t i = 0; i < applied_summary.size(); ++i) {
             if (i > 0)
               summary += " → ";
             summary += "**" + applied_summary[i] + "**";
           }
           upload_and_send_many(bot, msg.channel_id, outputs, summary);
           std::filesystem::remove_all(temp_dir);
         }).detach();
       },
       "Image"});
}

} // namespace image_ops
} // namespace bronx::cogs
