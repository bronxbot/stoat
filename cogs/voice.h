#pragma once
#include "helpers.h"
#include "stoatpp/bot_module.h"
#include "stoatpp/pulse_voice.h"
#include "stoatpp/voice_connection.h"
#include "voice_music.h"
#include "voice_tts.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <spawn.h>
#include <string>
#include <exception>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace bronx::cogs {

class VoiceModule : public stoatpp::bot_module {
public:
  ~VoiceModule() override {
    {
      std::lock_guard<std::mutex> lock(async_mutex_);
      for (auto &task : async_tasks_) {
        if (task.thread.joinable())
          task.thread.join();
      }
    }
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      sessions_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(pulse_mutex_);
      pulse_.shutdown();
    }
  }

  void register_handlers(stoatpp::cluster &bot) override {
    // 0. Listen to voice state updates to track user channels and handle bot disconnect
    bot.on_voice_state_update([this, &bot](const stoatpp::events::VoiceStateUpdate &ev) {
      {
        std::lock_guard<std::mutex> lock(voice_state_mutex_);
        if (ev.joined) {
          user_voice_channels_[ev.user_id] = ev.channel_id;
        } else {
          auto it = user_voice_channels_.find(ev.user_id);
          if (it != user_voice_channels_.end() &&
              (ev.channel_id.empty() || it->second == ev.channel_id)) {
            user_voice_channels_.erase(it);
          }
        }
      }

      if (!ev.joined && ev.user_id == bot.current_user().id) {
        auto session = find_session_by_channel(ev.channel_id);
        if (session) {
          std::lock_guard<std::mutex> operation_lock(session->operation_mutex);
          bool left_current_channel = false;
          {
            std::lock_guard<std::mutex> lock(session->voice_mutex);
            left_current_channel =
                !session->channel_id.empty() &&
                (ev.channel_id.empty() || session->channel_id == ev.channel_id);
          }
          if (left_current_channel) {
            session->music->stop();
            session->tts->stop();
            std::lock_guard<std::mutex> lock(session->voice_mutex);
            session->voice.disconnect();
            session->channel_id.clear();
            session->tts_channel_id.clear();
          }
        }
      }
    });

    // 1. Join the caller's voice channel or a named/explicit voice channel.
    bot.register_command(
        {"join",
         {},
         "join your voice channel, or a specified voice channel.",
         "join [channel_id_or_name]",
         {"[channel_id_or_name]"},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &args) {
           if (msg.server_id.empty()) {
             bot.send_message(msg.channel_id,
                              "[Voice] This command can only be used in a server.");
             return;
           }

           launch_task(bot, msg.channel_id, [this, &bot, msg, args] {
             std::string channel_id;
             std::string channel_name;
             std::string error;
             if (!resolve_voice_channel(bot, msg, args, channel_id,
                                        channel_name, error)) {
               bot.send_message(msg.channel_id, "[Voice] " + error);
               return;
             }

             bot.send_message(msg.channel_id,
                              "[Voice] Joining " + channel_name + "...");
             if (!connect_to_voice(bot, msg.server_id, channel_id, error)) {
               bot.send_message(msg.channel_id, "[Voice] " + error);
               return;
             }
             bot.send_message(msg.channel_id,
                              "[Voice] Joined " + channel_name + ".");
           });
         },
         "Voice"});

    // 2. Disconnect from voice channel command
    bot.register_command(
        {"dc",
         {"disconnect", "part", "stop"},
         "disconnect from the current voice channel.",
         "dc",
         {},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
            const std::vector<std::string> &args) {
           if (msg.server_id.empty() || active_channel_id(msg.server_id).empty()) {
             bot.send_message(msg.channel_id, "" + bronx::emj_warning + " I am not currently connected to any voice channel in this server.");
             return;
           }

           launch_task(bot, msg.channel_id, [this, &bot, msg] {
             auto session = find_session(msg.server_id);
             if (!session) {
               bot.send_message(msg.channel_id,
                                "I am not currently connected to a voice channel in this server.");
               return;
             }
             std::lock_guard<std::mutex> operation_lock(session->operation_mutex);
             session->music->stop();
             session->tts->stop();
             std::string channel_id;
             {
               std::lock_guard<std::mutex> lock(session->voice_mutex);
               channel_id = session->channel_id;
             }
             if (channel_id.empty()) {
               bot.send_message(msg.channel_id,
                                "I am not currently connected to a voice channel in this server.");
               return;
             }
             bot.ws().leave_voice_channel(channel_id);
             {
               std::lock_guard<std::mutex> lock(session->voice_mutex);
               session->voice.disconnect();
               session->channel_id.clear();
               session->tts_channel_id.clear();
             }
             bot.send_message(msg.channel_id,
                              "👋 Disconnected from the voice channel.");
           });
         },
         "Voice"});

    // 3. Play custom tone command
    bot.register_command(
        {"playtone",
         {"tone"},
         "play a custom frequency sine wave locally via PulseAudio.",
         "playtone <frequency_hz> [duration_seconds]",
         {"<frequency_hz>", "[duration_seconds]"},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
            const std::vector<std::string> &args) {
           std::cout << "[bronx command] handling playtone...\n";
           if (args.empty()) {
             bot.send_message(msg.channel_id, "" + bronx::emj_warning + " Usage: `.playtone <frequency_hz> [duration_seconds]`");
             return;
           }

           double freq = 440.0;
           double duration = 1.0;

           try {
             freq = std::stod(args[0]);
             if (args.size() > 1) {
               duration = std::stod(args[1]);
             }
           } catch (...) {
             bot.send_message(msg.channel_id, "" + bronx::emj_warning + " Invalid frequency or duration values.");
             return;
           }

           if (freq < 20.0 || freq > 20000.0) {
             bot.send_message(msg.channel_id, "" + bronx::emj_warning + " Frequency must be between 20Hz and 20kHz.");
             return;
           }

           if (duration < 0.1 || duration > 10.0) {
             bot.send_message(msg.channel_id, "" + bronx::emj_warning + " Duration must be between 0.1 and 10 seconds.");
             return;
           }

           launch_task(bot, msg.channel_id,
                       [this, &bot, msg, freq, duration] {
             std::lock_guard<std::mutex> lock(pulse_mutex_);
             if (!pulse_.is_initialized()) {
               pulse_.init("bronx-bot", "tone-stream");
             }

             bot.send_message(msg.channel_id, "🔈 Playing **" + std::to_string(static_cast<int>(freq)) + " Hz** tone for **" + std::to_string(duration) + "s**...");
             
             if (pulse_.play_tone(freq, duration)) {
               bot.send_message(msg.channel_id, "" + bronx::emj_yes + " Finished playing tone.");
             } else {
               bot.send_message(msg.channel_id, "" + bronx::emj_warning + " Failed to play tone via PulseAudio.");
             }
           });
         },
         "Voice"});

    // 4. Record and measure mic level command
    bot.register_command(
        {"speak",
         {"listen", "miclevel"},
         "record mic audio locally via PulseAudio and display volume levels.",
         "speak [duration_seconds]",
         {"[duration_seconds]"},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
            const std::vector<std::string> &args) {
           std::cout << "[bronx command] handling speak...\n";
           double duration = 3.0;

           if (!args.empty()) {
             try {
               duration = std::stod(args[0]);
             } catch (...) {
               bot.send_message(msg.channel_id, "" + bronx::emj_warning + " Invalid duration value.");
               return;
             }
           }

           if (duration < 0.5 || duration > 10.0) {
             bot.send_message(msg.channel_id, "" + bronx::emj_warning + " Recording duration must be between 0.5 and 10 seconds.");
             return;
           }

           launch_task(bot, msg.channel_id, [this, &bot, msg, duration] {
             bot.send_message(msg.channel_id, "🎤 **Recording microphone input for " + std::to_string(duration) + " seconds...** Speak now!");

             double average_rms = 0.0;
             std::vector<int16_t> samples;
             {
               std::lock_guard<std::mutex> lock(pulse_mutex_);
               if (!pulse_.is_initialized()) {
                 pulse_.init("bronx-bot", "mic-stream");
               }
               samples = pulse_.record_mic(duration, average_rms);
             }

             if (samples.empty()) {
               bot.send_message(msg.channel_id, "" + bronx::emj_warning + " Failed to record audio from PulseAudio recording source.");
               return;
             }

             // Render a cool volume meter
             int bars = static_cast<int>(average_rms * 25);
             if (bars > 25) bars = 25;
             std::string meter = "[" + std::string(bars, '#') + std::string(25 - bars, '-') + "]";

             std::stringstream ss;
             ss << "🎙️ **Audio Measurement Complete!**\n";
             ss << "Average RMS level: `" << average_rms << "`\n";
             ss << "Volume Meter: `" << meter << "`\n";
             
             if (average_rms < 0.005) {
               ss << "📢 *Status: Very quiet or no microphone input detected.*";
             } else if (average_rms < 0.05) {
               ss << "📢 *Status: Quiet / background noise detected.*";
             } else if (average_rms < 0.2) {
               ss << "📢 *Status: Normal speech level detected!*";
             } else {
               ss << "📢 *Status: Loud / audio clipping detected!*";
             }

             bot.send_message(msg.channel_id, ss.str());
           });
         },
         "Voice"});

    // 5. Resolve a YouTube/SoundCloud URL or search and enqueue it for playback.
    bot.register_command(
        {"play",
         {"p", "playyt", "yt", "youtube"},
         "play or queue a YouTube or SoundCloud track by URL or search query.",
         "play <url_or_search>",
         {"<url_or_search>"},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &args) {
           if (args.empty()) {
             bot.send_message(msg.channel_id,
                              "Usage: `.play <YouTube/SoundCloud URL or search query>`");
             return;
           }
           play_query(bot, msg, join_args(args), false);
         },
         "Voice"});

    // 5b. Search and play from SoundCloud directly.
    bot.register_command(
        {"soundcloud",
         {"sc", "playsc"},
         "search SoundCloud and play or queue the top result.",
         "soundcloud <search_or_url>",
         {"<search_or_url>"},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &args) {
           if (args.empty()) {
             bot.send_message(msg.channel_id,
                              "Usage: `.sc <SoundCloud URL or search query>`");
             return;
           }
           play_query(bot, msg, join_args(args), true);
         },
         "Voice"});

    bot.register_command(
        {"queue",
         {"list", "q"},
         "show the current track and queued tracks.",
         "queue",
         {},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &) {
           std::shared_ptr<ServerVoiceSession> session;
           std::string error;
           if (!can_control_player(msg, session, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           bot.send_message(msg.channel_id, session->music->queue_text());
         },
         "Voice"});

    bot.register_command(
        {"skip",
         {},
         "skip the current track.",
         "skip",
         {},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &) {
           std::shared_ptr<ServerVoiceSession> session;
           std::string error;
           if (!can_control_player(msg, session, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           bot.send_message(msg.channel_id, session->music->skip());
         },
         "Voice"});

    bot.register_command(
        {"pause",
         {},
         "pause music playback.",
         "pause",
         {},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &) {
           std::shared_ptr<ServerVoiceSession> session;
           std::string error;
           if (!can_control_player(msg, session, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           bot.send_message(msg.channel_id, session->music->pause());
         },
         "Voice"});

    bot.register_command(
        {"resume",
         {},
         "resume music playback.",
         "resume",
         {},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &) {
           std::shared_ptr<ServerVoiceSession> session;
           std::string error;
           if (!can_control_player(msg, session, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           bot.send_message(msg.channel_id, session->music->resume());
         },
         "Voice"});

    bot.register_command(
        {"clear",
         {},
         "clear queued tracks without stopping the current track.",
         "clear",
         {},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &) {
           std::shared_ptr<ServerVoiceSession> session;
           std::string error;
           if (!can_control_player(msg, session, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           bot.send_message(msg.channel_id, session->music->clear_queue());
         },
         "Voice"});

    bot.register_command(
        {"volume",
         {"vol"},
         "set playback volume from 0 to 200 percent.",
         "volume <0-200>",
         {"<0-200>"},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &args) {
           if (args.empty()) {
             bot.send_message(msg.channel_id, "Usage: `.volume <0-200>`");
             return;
           }
           std::shared_ptr<ServerVoiceSession> session;
           std::string error;
           if (!can_control_player(msg, session, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           try {
             size_t parsed = 0;
             const int volume = std::stoi(args[0], &parsed);
             if (parsed != args[0].size() || volume < 0 || volume > 200) {
               bot.send_message(msg.channel_id,
                                "Volume must be a whole number from 0 to 200.");
               return;
             }
             bot.send_message(
                 msg.channel_id,
                 session->music->set_volume(static_cast<double>(volume) / 100.0));
           } catch (const std::invalid_argument &) {
             bot.send_message(msg.channel_id,
                              "Volume must be a whole number from 0 to 200.");
           } catch (const std::out_of_range &) {
             bot.send_message(msg.channel_id,
                              "Volume must be a whole number from 0 to 200.");
           }
         },
         "Voice"});

    // 5. TTS chat monitor & per-user prefix/voice configuration
    bot.register_command(
        {"tts",
         {},
         "join your voice channel and read messages from this channel, or configure your TTS prefix/voice.",
         "tts [stop|skip|prefix <template|none|reset>|voice <name|list|reset>]",
         {"[subcommand]"},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &args) {
           if (!args.empty()) {
             const std::string sub = lowercase(args[0]);
             if (sub == "prefix") {
               if (!bronx::g_db) {
                 bot.send_message(msg.channel_id,
                                  "[TTS] Database is not available.");
                 return;
               }
               if (args.size() == 1) {
                 auto settings = bronx::g_db->get_tts_user_settings(msg.author.id);
                 std::string display_pfx = settings.prefix_template.empty()
                                               ? "(none)"
                                               : "`" + settings.prefix_template + "`";
                 bot.send_message(
                     msg.channel_id,
                     "[TTS] Your current prefix is " + display_pfx +
                         ".\nUsage: `.tts prefix <template>` (use `{user}` for your name, e.g. `.tts prefix {user} says`), `.tts prefix none`, or `.tts prefix reset`.");
                 return;
               }
               std::vector<std::string> rest(args.begin() + 1, args.end());
               std::string new_prefix = join_args(rest);
               std::string lower_pfx = lowercase(new_prefix);
               if (lower_pfx == "none" || lower_pfx == "off") {
                 new_prefix.clear();
               } else if (lower_pfx == "reset" || lower_pfx == "default") {
                 new_prefix = "{user} says";
               } else if (new_prefix.size() > 60) {
                 bot.send_message(
                     msg.channel_id,
                     "[TTS] Prefix template must be 60 characters or fewer.");
                 return;
               }
               if (bronx::g_db->set_tts_prefix(msg.author.id, new_prefix)) {
                 std::string msg_out =
                     new_prefix.empty()
                         ? "[TTS] Disabled spoken prefix for your messages."
                         : "[TTS] Updated your spoken prefix to `" +
                               new_prefix + "`.";
                 bot.send_message(msg.channel_id, msg_out);
               } else {
                 bot.send_message(msg.channel_id,
                                  "[TTS] Failed to save your prefix.");
               }
               return;
             }

             if (sub == "voice") {
               if (!bronx::g_db) {
                 bot.send_message(msg.channel_id,
                                  "[TTS] Database is not available.");
                 return;
               }
               if (args.size() == 1) {
                 auto settings = bronx::g_db->get_tts_user_settings(msg.author.id);
                 bot.send_message(
                     msg.channel_id,
                     "[TTS] Your current voice is `" + settings.voice +
                         "`. Use `.tts voice list` to see popular options or `.tts voice <name>` to change it.");
                 return;
               }
               const std::string arg = args[1];
               const std::string lower_arg = lowercase(arg);
               if (lower_arg == "list") {
                 bot.send_message(
                     msg.channel_id,
                     "[TTS] Popular free voices:\n"
                     "- `en-US-AndrewMultilingualNeural` (default)\n"
                     "- `en-US-AriaNeural`\n"
                     "- `en-US-GuyNeural`\n"
                     "- `en-US-JennyNeural`\n"
                     "- `en-GB-SoniaNeural`\n"
                     "- `en-GB-RyanNeural`\n"
                     "- `en-AU-NatashaNeural`");
                 return;
               }
               std::string new_voice =
                   (lower_arg == "reset" || lower_arg == "default")
                       ? "en-US-AndrewMultilingualNeural"
                       : arg;
               if (new_voice.size() > 64 ||
                   new_voice.find_first_of(" \t\r\n") != std::string::npos) {
                 bot.send_message(msg.channel_id,
                                  "[TTS] Invalid voice name.");
                 return;
               }
               if (bronx::g_db->set_tts_voice(msg.author.id, new_voice)) {
                 bot.send_message(msg.channel_id,
                                  "[TTS] Updated your voice to `" + new_voice +
                                      "`.");
               } else {
                 bot.send_message(msg.channel_id,
                                  "[TTS] Failed to save your voice.");
               }
               return;
             }

             if (sub == "stop" || sub == "off") {
               std::shared_ptr<ServerVoiceSession> session;
               std::string error;
               if (!can_control_player(msg, session, error)) {
                 bot.send_message(msg.channel_id, "[TTS] " + error);
                 return;
               }
               session->tts->stop();
               {
                 std::lock_guard<std::mutex> lock(session->voice_mutex);
                 session->tts_channel_id.clear();
               }
               bot.send_message(msg.channel_id,
                                "🔇 Stopped TTS monitoring.");
               return;
             }

             if (sub == "skip") {
               std::shared_ptr<ServerVoiceSession> session;
               std::string error;
               if (!can_control_player(msg, session, error)) {
                 bot.send_message(msg.channel_id, "[TTS] " + error);
                 return;
               }
               session->tts->skip();
               bot.send_message(msg.channel_id,
                                "⏭️ Skipped the current TTS message.");
               return;
             }
           }

           if (msg.server_id.empty()) {
             bot.send_message(msg.channel_id,
                              "[TTS] This command can only be used in a server.");
             return;
           }

           launch_task(bot, msg.channel_id, [this, &bot, msg] {
             auto session = find_session(msg.server_id);
             std::string current_vc;
             if (session) {
               std::lock_guard<std::mutex> lock(session->voice_mutex);
               if (session->voice.is_connected())
                 current_vc = session->channel_id;
             }

             if (current_vc.empty()) {
               std::string channel_id;
               std::string channel_name;
               std::string error;
               if (!resolve_voice_channel(bot, msg, {}, channel_id,
                                          channel_name, error)) {
                 bot.send_message(msg.channel_id, "[TTS] " + error);
                 return;
               }
               if (!connect_to_voice(bot, msg.server_id, channel_id, error)) {
                 bot.send_message(msg.channel_id, "[TTS] " + error);
                 return;
               }
               session = find_session(msg.server_id);
             } else if (user_voice_channel(msg.author.id) != current_vc) {
               bot.send_message(
                   msg.channel_id,
                   "[TTS] Join my voice channel to toggle TTS.");
               return;
             }

             if (!session) {
               bot.send_message(msg.channel_id,
                                "[TTS] Could not initialize voice session.");
               return;
             }

             bool enabled = false;
             {
               std::lock_guard<std::mutex> lock(session->voice_mutex);
               if (session->tts_channel_id == msg.channel_id) {
                 session->tts_channel_id.clear();
                 enabled = false;
               } else {
                 session->tts_channel_id = msg.channel_id;
                 enabled = true;
               }
             }

             if (!enabled) {
               session->tts->stop();
               bot.send_message(msg.channel_id,
                                "🔇 Stopped TTS monitoring for this channel.");
             } else {
               bot.send_message(
                   msg.channel_id,
                   "🗣️ Now reading messages from this channel in voice. Use `.tts stop` to stop, or `.tts prefix <template>` to customize your spoken prefix.");
             }
           });
         },
         "Voice"});

    // 6. Listen to messages in TTS-monitored channels
    bot.on_message([this, &bot](const stoatpp::events::Message &msg) {
      if (msg.server_id.empty() || msg.author.bot ||
          msg.author.id == bot.current_user().id || msg.content.empty()) {
        return;
      }
      if (!msg.prefix.empty() || msg.content[0] == '.') {
        return;
      }

      auto session = find_session(msg.server_id);
      if (!session)
        return;

      {
        std::lock_guard<std::mutex> lock(session->voice_mutex);
        if (!session->voice.is_connected() ||
            session->tts_channel_id != msg.channel_id) {
          return;
        }
      }

      std::string cleaned = sanitize_tts_text(msg.content);
      if (cleaned.empty())
        return;

      std::string speaker =
          (msg.author.display_name && !msg.author.display_name->empty())
              ? *msg.author.display_name
              : msg.author.username;

      bronx::Database::TtsUserSettings settings;
      if (bronx::g_db)
        settings = bronx::g_db->get_tts_user_settings(msg.author.id);

      std::string spoken;
      if (!settings.prefix_template.empty()) {
        std::string pfx = settings.prefix_template;
        const std::string token = "{user}";
        size_t pos = 0;
        while ((pos = pfx.find(token, pos)) != std::string::npos) {
          pfx.replace(pos, token.size(), speaker);
          pos += speaker.size();
        }
        spoken = pfx + " " + cleaned;
      } else {
        spoken = cleaned;
      }

      session->tts->enqueue(std::move(spoken), settings.voice);
    });
  }

private:
  static std::string sanitize_tts_text(const std::string &raw) {
    std::string out;
    out.reserve(std::min<size_t>(raw.size(), 300));
    for (size_t i = 0; i < raw.size() && out.size() < 300; ++i) {
      if (raw.compare(i, 8, "https://") == 0 ||
          raw.compare(i, 7, "http://") == 0) {
        out += "a link";
        while (i < raw.size() && !std::isspace(static_cast<unsigned char>(raw[i])))
          ++i;
        if (i < raw.size())
          out += ' ';
        continue;
      }
      if (raw[i] == '<' && i + 2 < raw.size()) {
        size_t close = raw.find('>', i + 1);
        if (close != std::string::npos && close - i < 64) {
          if (raw[i + 1] == '@') {
            out += "someone";
            i = close;
            continue;
          }
          if (raw[i + 1] == '#') {
            out += "a channel";
            i = close;
            continue;
          }
          if (raw[i + 1] == ':') {
            size_t second_colon = raw.find(':', i + 2);
            if (second_colon != std::string::npos && second_colon < close) {
              out += raw.substr(i + 2, second_colon - (i + 2));
              i = close;
              continue;
            }
          }
        }
      }
      char c = raw[i];
      if (c == '*' || c == '_' || c == '`' || c == '~' || c == '>')
        continue;
      if (c == '\n' || c == '\r' || c == '\t')
        c = ' ';
      if (c == ' ' && (out.empty() || out.back() == ' '))
        continue;
      out += c;
    }
    while (!out.empty() && out.back() == ' ')
      out.pop_back();
    return out;
  }
  struct ServerVoiceSession {
    explicit ServerVoiceSession(stoatpp::cluster &bot) {
      auto write_pcm = [this](const int16_t *samples, size_t count) {
        std::lock_guard<std::mutex> lock(voice_mutex);
        return voice.is_connected() && voice.write_pcm(samples, count);
      };
      music = std::make_unique<VoiceMusicPlayer>(
          write_pcm,
          [&bot](const std::string &channel_id, const std::string &message) {
            bot.send_message(channel_id, message);
          },
          [this]() { return tts && tts->is_speaking(); });
      tts = std::make_unique<VoiceTtsPlayer>(
          write_pcm, [this]() { return music && music->is_playing(); });
    }
    ~ServerVoiceSession() {
      if (tts)
        tts->shutdown();
      if (music)
        music->shutdown();
      std::lock_guard<std::mutex> lock(voice_mutex);
      voice.disconnect();
    }

    stoatpp::voice_connection voice;
    std::unique_ptr<VoiceMusicPlayer> music;
    std::unique_ptr<VoiceTtsPlayer> tts;
    std::string channel_id;
    std::string tts_channel_id;
    mutable std::mutex voice_mutex;
    std::mutex operation_mutex;
  };

  struct AsyncTask {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
  };

  template <typename Function>
  void launch_task(stoatpp::cluster &bot, const std::string &message_channel,
                   Function function) {
    std::lock_guard<std::mutex> lock(async_mutex_);
    for (auto it = async_tasks_.begin(); it != async_tasks_.end();) {
      if (it->finished->load()) {
        if (it->thread.joinable())
          it->thread.join();
        it = async_tasks_.erase(it);
      } else {
        ++it;
      }
    }
    if (async_tasks_.size() >= 16) {
      bot.send_message(message_channel,
                       "[Voice] Too many requests are already in progress.");
      return;
    }

    auto finished = std::make_shared<std::atomic<bool>>(false);
    async_tasks_.push_back(
        {std::thread([function = std::move(function), &bot, message_channel,
                      finished]() mutable {
           try {
             function();
           } catch (const std::exception &error) {
             std::cerr << "[bronx voice] background task failed: "
                       << error.what() << "\n";
             bot.send_message(message_channel,
                              "[Voice] Request failed: " +
                                  std::string(error.what()));
           }
           finished->store(true);
         }),
         std::move(finished)});
  }

  static std::string join_args(const std::vector<std::string> &args) {
    std::ostringstream result;
    for (size_t i = 0; i < args.size(); ++i) {
      if (i != 0)
        result << ' ';
      result << args[i];
    }
    return result.str();
  }

  std::string user_voice_channel(const std::string &user_id) const {
    std::lock_guard<std::mutex> lock(voice_state_mutex_);
    auto it = user_voice_channels_.find(user_id);
    return it == user_voice_channels_.end() ? "" : it->second;
  }

  std::shared_ptr<ServerVoiceSession>
  get_or_create_session(stoatpp::cluster &bot, const std::string &server_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto &session = sessions_[server_id];
    if (!session)
      session = std::make_shared<ServerVoiceSession>(bot);
    return session;
  }

  std::shared_ptr<ServerVoiceSession>
  find_session(const std::string &server_id) const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(server_id);
    return it == sessions_.end() ? nullptr : it->second;
  }

  std::shared_ptr<ServerVoiceSession>
  find_session_by_channel(const std::string &channel_id) const {
    if (channel_id.empty())
      return nullptr;
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (const auto &[srv_id, session] : sessions_) {
      if (!session)
        continue;
      std::lock_guard<std::mutex> vlock(session->voice_mutex);
      if (session->channel_id == channel_id)
        return session;
    }
    return nullptr;
  }

  std::string active_channel_id(const std::string &server_id) const {
    auto session = find_session(server_id);
    if (!session)
      return "";
    std::lock_guard<std::mutex> lock(session->voice_mutex);
    return session->channel_id;
  }

  bool voice_is_connected(const std::string &server_id) const {
    auto session = find_session(server_id);
    if (!session)
      return false;
    std::lock_guard<std::mutex> lock(session->voice_mutex);
    return session->voice.is_connected();
  }

  bool can_control_player(const stoatpp::events::Message &msg,
                          std::shared_ptr<ServerVoiceSession> &session,
                          std::string &error) const {
    if (msg.server_id.empty()) {
      error = "This command can only be used in a server.";
      return false;
    }
    session = find_session(msg.server_id);
    std::string channel_id;
    if (session) {
      std::lock_guard<std::mutex> lock(session->voice_mutex);
      channel_id = session->channel_id;
    }
    if (channel_id.empty()) {
      error = "I am not connected to a voice channel in this server.";
      return false;
    }
    if (user_voice_channel(msg.author.id) != channel_id) {
      error = "Join my voice channel to control playback.";
      return false;
    }
    return true;
  }

  static std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                   });
    return value;
  }

  bool resolve_voice_channel(stoatpp::cluster &bot,
                             const stoatpp::events::Message &msg,
                             const std::vector<std::string> &args,
                             std::string &channel_id,
                             std::string &channel_name,
                             std::string &error) const {
    std::string input = join_args(args);
    if (input.empty()) {
      input = user_voice_channel(msg.author.id);
      if (input.empty()) {
        error = "I can't detect your voice channel. Join one first or specify "
                "a voice channel.";
        return false;
      }
    }
    if (input.size() > 3 && input.rfind("<#", 0) == 0 &&
        input.back() == '>') {
      input = input.substr(2, input.size() - 3);
    }

    auto is_voice_channel = [](const stoatpp::models::Channel &channel) {
      return channel.channel_type == "VoiceChannel" ||
             (channel.channel_type == "TextChannel" &&
              channel.raw.contains("voice") && !channel.raw["voice"].is_null());
    };

    auto accept_channel = [&](const stoatpp::models::Channel &channel) {
      if (!is_voice_channel(channel) || !channel.server ||
          *channel.server != msg.server_id) {
        return false;
      }
      channel_id = channel.id;
      channel_name = channel.name.value_or("voice channel");
      return !channel_id.empty();
    };

    if (input.size() == 26) {
      auto cached_channel = bot.get_channel(input);
      if (cached_channel && accept_channel(*cached_channel))
        return true;

      auto response = bot.rest().get_channel(input);
      if (response.success() &&
          accept_channel(stoatpp::models::Channel::from_json(response.body)))
        return true;
      error = "I couldn't find an accessible voice channel with that ID in "
              "this server.";
      return false;
    }

    const std::string search = lowercase(input);
    for (const auto &channel : bot.get_channels()) {
      if (!channel.server || *channel.server != msg.server_id ||
          !is_voice_channel(channel) || !channel.name) {
        continue;
      }
      if (lowercase(*channel.name).find(search) != std::string::npos) {
        channel_id = channel.id;
        channel_name = *channel.name;
        return true;
      }
    }

    error = "Could not find a matching voice channel.";
    return false;
  }

  bool connect_to_voice(stoatpp::cluster &bot, const std::string &server_id,
                        const std::string &channel_id, std::string &error) {
    auto session = get_or_create_session(bot, server_id);
    std::lock_guard<std::mutex> operation_lock(session->operation_mutex);
    {
      std::lock_guard<std::mutex> lock(session->voice_mutex);
      if (session->channel_id == channel_id && session->voice.is_connected())
        return true;
    }

    session->music->stop();
    std::string old_channel;
    {
      std::lock_guard<std::mutex> lock(session->voice_mutex);
      old_channel = session->channel_id;
      session->voice.disconnect();
      session->channel_id.clear();
    }
    if (!old_channel.empty())
      bot.ws().leave_voice_channel(old_channel);

    auto node_info = bot.rest().get_node_info();
    if (!node_info.success() ||
        !node_info.body.contains("features") ||
        !node_info.body["features"].is_object() ||
        !node_info.body["features"].contains("livekit") ||
        !node_info.body["features"]["livekit"].is_object() ||
        !node_info.body["features"]["livekit"].contains("nodes") ||
        !node_info.body["features"]["livekit"]["nodes"].is_array()) {
      error = "The Stoat instance did not provide voice server information.";
      return false;
    }

    std::string voice_node;
    for (const auto &node : node_info.body["features"]["livekit"]["nodes"]) {
      if (node.is_object() && node.contains("name") &&
          node["name"].is_string()) {
        voice_node = node["name"].get<std::string>();
        if (!voice_node.empty())
          break;
      }
    }
    if (voice_node.empty()) {
      error = "The Stoat instance did not advertise an available voice server.";
      return false;
    }

    auto response = bot.rest().join_voice_call(channel_id, voice_node);
    if (!response.success()) {
      error = "Failed to join the voice call: " + response.error_message();
      return false;
    }

    const std::string url = response.body.value("url", "");
    const std::string token = response.body.value("token", "");
    if (url.empty() || token.empty()) {
      error = "The voice server returned an invalid connection response.";
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(session->voice_mutex);
      if (!session->voice.connect(url, token)) {
        session->voice.disconnect();
        error = "Could not establish the voice connection.";
        return false;
      }
    }

    bool connected = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(session->voice_mutex);
        connected = session->voice.is_connected();
      }
      if (connected)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!connected) {
      std::lock_guard<std::mutex> lock(session->voice_mutex);
      session->voice.disconnect();
      error = "The voice server did not finish connecting.";
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(session->voice_mutex);
      session->channel_id = channel_id;
    }
    bot.ws().join_voice_channel(channel_id);
    return true;
  }

  static bool supported_youtube_url(const std::string &url) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
      return false;
    const std::string scheme = lowercase(url.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https")
      return false;
    const size_t host_start = scheme_end + 3;
    const size_t host_end = url.find_first_of("/?#", host_start);
    std::string host = lowercase(
        url.substr(host_start, host_end == std::string::npos
                                   ? std::string::npos
                                   : host_end - host_start));
    if (host.find('@') != std::string::npos)
      return false;
    const size_t port = host.find(':');
    if (port != std::string::npos)
      host.resize(port);
    return host == "youtu.be" || host == "youtube.com" ||
           (host.size() > 12 &&
            host.compare(host.size() - 12, 12, ".youtube.com") == 0) ||
           host == "soundcloud.com" || host == "on.soundcloud.com" ||
           (host.size() > 15 &&
            host.compare(host.size() - 15, 15, ".soundcloud.com") == 0);
  }

  void play_query(stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                  const std::string &query, bool prefer_soundcloud) {
    if (msg.server_id.empty()) {
      bot.send_message(msg.channel_id,
                       "[Music] This command can only be used in a server.");
      return;
    }
    std::cout << "[bronx music] " << (prefer_soundcloud ? ".sc" : ".play")
              << " query=\"" << query << "\" user=" << msg.author.id
              << " channel=" << msg.channel_id << std::endl;
    launch_task(bot, msg.channel_id, [this, &bot, msg, query, prefer_soundcloud] {
      const std::string caller_channel = user_voice_channel(msg.author.id);
      std::string current_channel = active_channel_id(msg.server_id);
      std::string error;
      if (current_channel.empty()) {
        if (caller_channel.empty()) {
          bot.send_message(
              msg.channel_id,
              "[Music] Join a voice channel first, or use `.join <channel>`.");
          return;
        }
        if (!connect_to_voice(bot, msg.server_id, caller_channel, error)) {
          bot.send_message(msg.channel_id, "[Voice] " + error);
          return;
        }
        current_channel = caller_channel;
      } else if (caller_channel.empty() || caller_channel != current_channel) {
        bot.send_message(
            msg.channel_id,
            "[Music] Join my current voice channel to control playback.");
        return;
      }
      if (!voice_is_connected(msg.server_id) &&
          !connect_to_voice(bot, msg.server_id, current_channel, error)) {
        bot.send_message(msg.channel_id, "[Voice] " + error);
        return;
      }

      bot.send_message(msg.channel_id,
                       prefer_soundcloud ? "[Music] Searching SoundCloud..."
                                         : "[Music] Searching...");
      auto track = resolve_youtube(query, error, prefer_soundcloud);
      if (!track) {
        std::cerr << "[bronx music] resolve failed for \"" << query
                  << "\": " << error << std::endl;
        bot.send_message(msg.channel_id, "[Music] " + error);
        return;
      }
      std::cout << "[bronx music] resolved \"" << track->title << "\" -> "
                << track->url << " (duration=" << track->duration_seconds
                << "s)" << std::endl;
      if (active_channel_id(msg.server_id) != current_channel) {
        bot.send_message(
            msg.channel_id,
            "[Music] The voice connection changed before the track was queued.");
        return;
      }

      auto session = find_session(msg.server_id);
      if (!session) {
        bot.send_message(msg.channel_id,
                         "[Music] Voice session is no longer active.");
        return;
      }
      const std::string track_title = track->title;
      size_t position = 0;
      if (!session->music->enqueue(std::move(*track), msg.channel_id, position)) {
        bot.send_message(msg.channel_id,
                         "[Music] The queue is full or shutting down.");
        return;
      }
      bot.send_message(msg.channel_id,
                       "[Music] Added **" + track_title +
                           "** to the queue at position " +
                           std::to_string(position) + ".");
    });
  }

  static bool capture_ytdlp_json(const std::string &target,
                                std::string &output,
                                std::string &error) {
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0) {
      error = "Could not create the YouTube search process.";
      return false;
    }

    posix_spawn_file_actions_t actions;
    int spawn_error = posix_spawn_file_actions_init(&actions);
    const bool actions_initialized = spawn_error == 0;
    if (spawn_error == 0)
      spawn_error =
          posix_spawn_file_actions_adddup2(&actions, descriptors[1],
                                           STDOUT_FILENO);
    if (spawn_error == 0)
      spawn_error = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    if (spawn_error == 0)
      spawn_error = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
    pid_t child = -1;
    if (spawn_error == 0) {
      std::string cookies_path;
      if (const char *env_cookies = std::getenv("YTDLP_COOKIES_FILE")) {
        if (*env_cookies && access(env_cookies, R_OK) == 0)
          cookies_path = env_cookies;
      } else if (access("/app/cookies.txt", R_OK) == 0) {
        cookies_path = "/app/cookies.txt";
      }

      std::vector<char *> argv = {
          const_cast<char *>("yt-dlp"),
          const_cast<char *>("--ignore-config"),
          const_cast<char *>("--socket-timeout"), const_cast<char *>("15"),
          const_cast<char *>("--retries"), const_cast<char *>("1"),
          const_cast<char *>("--quiet"),
          const_cast<char *>("--no-warnings"),
          const_cast<char *>("--skip-download"),
          const_cast<char *>("--flat-playlist"),
          const_cast<char *>("--dump-single-json"),
          const_cast<char *>("--no-playlist")};
      if (!cookies_path.empty()) {
        argv.push_back(const_cast<char *>("--cookies"));
        argv.push_back(const_cast<char *>(cookies_path.c_str()));
      }
      argv.push_back(const_cast<char *>("--"));
      argv.push_back(const_cast<char *>(target.c_str()));
      argv.push_back(nullptr);
      spawn_error = posix_spawnp(&child, "yt-dlp", &actions, nullptr,
                                 argv.data(), environ);
    }
    if (actions_initialized) {
      posix_spawn_file_actions_destroy(&actions);
    }
    if (spawn_error != 0) {
      close(descriptors[0]);
      close(descriptors[1]);
      error = "Could not start yt-dlp: " + std::string(std::strerror(spawn_error));
      return false;
    }

    close(descriptors[1]);
    constexpr size_t max_output = 2 * 1024 * 1024;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(45);
    bool timed_out = false;
    std::array<char, 8192> buffer{};
    while (true) {
      pollfd descriptor{descriptors[0], POLLIN | POLLHUP, 0};
      const int result = poll(&descriptor, 1, 250);
      if (result < 0 && errno == EINTR)
        continue;
      if (result < 0) {
        error = "Could not read the YouTube search result.";
        break;
      }
      if (result > 0) {
        const ssize_t count = read(descriptors[0], buffer.data(), buffer.size());
        if (count == 0)
          break;
        if (count < 0) {
          if (errno == EINTR || errno == EAGAIN)
            continue;
          error = "Could not read the YouTube search result.";
          break;
        }
        if (output.size() + static_cast<size_t>(count) > max_output) {
          error = "The YouTube search result was unexpectedly large.";
          break;
        }
        output.append(buffer.data(), static_cast<size_t>(count));
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        error = "YouTube search timed out.";
        break;
      }
    }
    close(descriptors[0]);
    if (timed_out || !error.empty())
      kill(child, SIGTERM);

    int status = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
      const pid_t waited = waitpid(child, &status, WNOHANG);
      if (waited == child)
        break;
      if (waited < 0 && errno != EINTR)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (waitpid(child, &status, WNOHANG) == 0) {
      kill(child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
    }
    if (!error.empty())
      return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      error = "No playable YouTube result was found. Check the URL or search "
              "terms and try again.";
      return false;
    }
    return true;
  }

  static std::string url_encode(const std::string &value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : value) {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
        escaped << c;
      } else {
        escaped << std::uppercase << '%' << std::setw(2)
                << static_cast<int>(c) << std::nouppercase;
      }
    }
    return escaped.str();
  }

  static double parse_duration_text(const std::string &text) {
    if (text.empty())
      return 0.0;
    double total = 0.0;
    size_t start = 0;
    while (start < text.size()) {
      size_t colon = text.find(':', start);
      std::string part = (colon == std::string::npos)
                             ? text.substr(start)
                             : text.substr(start, colon - start);
      if (part.empty())
        return 0.0;
      for (unsigned char c : part) {
        if (!std::isdigit(c))
          return 0.0;
      }
      total = total * 60.0 + std::stod(part);
      if (colon == std::string::npos)
        break;
      start = colon + 1;
    }
    return total;
  }

  static const nlohmann::json *find_first_video_renderer(const nlohmann::json &node,
                                                         int depth = 0) {
    if (depth > 20)
      return nullptr;
    if (node.is_object()) {
      auto it = node.find("videoRenderer");
      if (it != node.end() && it->is_object())
        return &(*it);
      for (const auto &item : node.items()) {
        if (const auto *found = find_first_video_renderer(item.value(), depth + 1))
          return found;
      }
    } else if (node.is_array()) {
      for (const auto &elem : node) {
        if (const auto *found = find_first_video_renderer(elem, depth + 1))
          return found;
      }
    }
    return nullptr;
  }

  // Fast direct YouTube search (~300-700ms) avoiding yt-dlp Python startup.
  static bool search_youtube_direct(const std::string &query,
                                    VoiceMusicPlayer::Track &track) {
    httplib::Client cli("https://www.youtube.com");
    cli.set_connection_timeout(std::chrono::seconds(8));
    cli.set_read_timeout(std::chrono::seconds(8));
    cli.set_follow_location(true);

    httplib::Headers headers = {
        {"User-Agent",
         "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like "
         "Gecko) Chrome/120.0.0.0 Safari/537.36"},
        {"Accept-Language", "en-US,en;q=0.9"}};

    // sp=EgIQAQ%3D%3D filters results to videos only.
    const auto started = std::chrono::steady_clock::now();
    auto res = cli.Get("/results?search_query=" + url_encode(query) +
                           "&sp=EgIQAQ%3D%3D",
                       headers);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count();
    if (!res || res->status != 200 || res->body.empty()) {
      std::cerr << "[bronx music] direct search HTTP failed (" << elapsed_ms
                << "ms, status=" << (res ? res->status : -1) << "), falling back to yt-dlp"
                << std::endl;
      return false;
    }

    const std::string marker = "var ytInitialData = ";
    const size_t start = res->body.find(marker);
    if (start == std::string::npos) {
      std::cerr << "[bronx music] direct search missing ytInitialData ("
                << elapsed_ms << "ms), falling back to yt-dlp" << std::endl;
      return false;
    }
    const size_t json_start = start + marker.size();
    const size_t end = res->body.find(";</script>", json_start);
    if (end == std::string::npos) {
      std::cerr << "[bronx music] direct search truncated ytInitialData ("
                << elapsed_ms << "ms), falling back to yt-dlp" << std::endl;
      return false;
    }

    auto data = nlohmann::json::parse(
        res->body.substr(json_start, end - json_start), nullptr, false);
    if (data.is_discarded()) {
      std::cerr << "[bronx music] direct search JSON parse failed ("
                << elapsed_ms << "ms), falling back to yt-dlp" << std::endl;
      return false;
    }

    const nlohmann::json *vr = find_first_video_renderer(data);
    if (!vr || !vr->contains("videoId") || !(*vr)["videoId"].is_string()) {
      std::cerr << "[bronx music] direct search found no videoRenderer ("
                << elapsed_ms << "ms), falling back to yt-dlp" << std::endl;
      return false;
    }

    const std::string video_id = (*vr)["videoId"].get<std::string>();
    if (video_id.empty())
      return false;

    std::string title;
    if (vr->contains("title") && (*vr)["title"].is_object()) {
      const auto &t = (*vr)["title"];
      if (t.contains("runs") && t["runs"].is_array() && !t["runs"].empty() &&
          t["runs"][0].is_object() && t["runs"][0].contains("text") &&
          t["runs"][0]["text"].is_string()) {
        title = t["runs"][0]["text"].get<std::string>();
      } else if (t.contains("simpleText") && t["simpleText"].is_string()) {
        title = t["simpleText"].get<std::string>();
      }
    }
    if (title.empty())
      return false;

    track.title = title;
    track.url = "https://www.youtube.com/watch?v=" + video_id;
    if (vr->contains("lengthText") && (*vr)["lengthText"].is_object() &&
        (*vr)["lengthText"].contains("simpleText") &&
        (*vr)["lengthText"]["simpleText"].is_string()) {
      track.duration_seconds =
          parse_duration_text((*vr)["lengthText"]["simpleText"].get<std::string>());
    }
    return true;
  }

  static std::optional<VoiceMusicPlayer::Track>
  resolve_youtube(const std::string &query, std::string &error,
                  bool prefer_soundcloud = false) {
    std::string target = query;
    if (query.rfind("http://", 0) == 0 ||
        query.rfind("https://", 0) == 0) {
      if (!supported_youtube_url(query)) {
        error = "Only YouTube and SoundCloud URLs are supported right now.";
        return std::nullopt;
      }
    } else {
      if (query.find("://") != std::string::npos) {
        error = "Only YouTube/SoundCloud URLs or search terms are supported.";
        return std::nullopt;
      }
      if (prefer_soundcloud) {
        target = "scsearch1:" + query;
      } else if (query.rfind("sc:", 0) == 0 && query.size() > 3) {
        target = "scsearch1:" + query.substr(3);
      } else {
        VoiceMusicPlayer::Track direct_track;
        if (search_youtube_direct(query, direct_track)) {
          return direct_track;
        }
        target = "ytsearch1:" + query;
      }
    }

    std::string json_text;
    if (!capture_ytdlp_json(target, json_text, error))
      return std::nullopt;

    try {
      auto json = nlohmann::json::parse(json_text);
      if (json.contains("entries") && json["entries"].is_array()) {
        if (json["entries"].empty() || json["entries"][0].is_null()) {
          error = "No playable YouTube result was found.";
          return std::nullopt;
        }
        json = json["entries"][0];
      }
      if (!json.is_object() || !json.contains("title") ||
          !json["title"].is_string()) {
        error = "YouTube returned incomplete track information.";
        return std::nullopt;
      }

      VoiceMusicPlayer::Track track;
      track.title = json["title"].get<std::string>();
      if (json.contains("webpage_url") && json["webpage_url"].is_string())
        track.url = json["webpage_url"].get<std::string>();
      else if (json.contains("original_url") &&
               json["original_url"].is_string())
        track.url = json["original_url"].get<std::string>();
      else if (json.contains("url") && json["url"].is_string())
        track.url = json["url"].get<std::string>();
      else if (json.contains("id") && json["id"].is_string())
        track.url = "https://www.youtube.com/watch?v=" + json["id"].get<std::string>();
      if (track.title.empty() || !supported_youtube_url(track.url)) {
        error = "YouTube returned an invalid video URL.";
        return std::nullopt;
      }
      if (json.contains("duration") && json["duration"].is_number())
        track.duration_seconds = json["duration"].get<double>();
      return track;
    } catch (const nlohmann::json::exception &exception) {
      error = "Could not parse YouTube's response: " +
              std::string(exception.what());
      return std::nullopt;
    }
  }

  stoatpp::pulse_voice_client pulse_;
  std::mutex pulse_mutex_;
  mutable std::mutex sessions_mutex_;
  std::unordered_map<std::string, std::shared_ptr<ServerVoiceSession>> sessions_;
  mutable std::mutex voice_state_mutex_;
  std::unordered_map<std::string, std::string> user_voice_channels_;
  std::mutex async_mutex_;
  std::vector<AsyncTask> async_tasks_;
};

} // namespace bronx::cogs
