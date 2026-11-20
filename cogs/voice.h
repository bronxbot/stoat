#pragma once
#include "helpers.h"
#include "stoatpp/bot_module.h"
#include "stoatpp/pulse_voice.h"
#include "stoatpp/voice_connection.h"
#include "voice_music.h"
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
    if (music_)
      music_->shutdown();
    {
      std::lock_guard<std::mutex> lock(voice_mutex_);
      voice_.disconnect();
    }
    {
      std::lock_guard<std::mutex> lock(pulse_mutex_);
      pulse_.shutdown();
    }
  }

  void register_handlers(stoatpp::cluster &bot) override {
    music_ = std::make_unique<VoiceMusicPlayer>(
        [this](const int16_t *samples, size_t count) {
          std::lock_guard<std::mutex> lock(voice_mutex_);
          return voice_.is_connected() && voice_.write_pcm(samples, count);
        },
        [&bot](const std::string &channel_id, const std::string &message) {
          bot.send_message(channel_id, message);
        });

    // Hook PulseAudio to WebRTC voice stream
    pulse_.set_audio_monitor([this](const int16_t* samples, size_t count) {
      std::lock_guard<std::mutex> lock(voice_mutex_);
      if (voice_.is_connected()) {
        voice_.write_pcm(samples, count);
      }
    });

    // 0. Listen to voice state updates to play join/leave chimes and log active status
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
        std::lock_guard<std::mutex> operation_lock(voice_operation_mutex_);
        bool left_current_channel = false;
        {
          std::lock_guard<std::mutex> lock(voice_mutex_);
          left_current_channel =
              !current_channel_id_.empty() &&
              (ev.channel_id.empty() || current_channel_id_ == ev.channel_id);
        }
        if (left_current_channel) {
          music_->stop();
          {
            std::lock_guard<std::mutex> lock(voice_mutex_);
            voice_.disconnect();
            current_channel_id_.clear();
          }
          std::lock_guard<std::mutex> lock(pulse_mutex_);
          pulse_.shutdown();
        }
      }

      bool should_play_chime = false;
      {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        should_play_chime = !current_channel_id_.empty() &&
                            ev.channel_id == current_channel_id_;
      }
      if (should_play_chime) {
        std::lock_guard<std::mutex> lock(pulse_mutex_);
        pulse_.play_chime(ev.joined);
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
             if (!connect_to_voice(bot, channel_id, error)) {
               bot.send_message(msg.channel_id, "[Voice] " + error);
               return;
             }
             bot.send_message(msg.channel_id,
                              "[Voice] Joined " + channel_name + ".");
           });
         },
         "Voice"});

    // 2. Leave voice channel command
    bot.register_command(
        {"leave",
         {"part", "disconnect", "stop"},
         "leave the current voice channel.",
         "leave",
         {},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
            const std::vector<std::string> &args) {
           if (active_channel_id().empty()) {
             bot.send_message(msg.channel_id, "" + bronx::emj_warning + " I am not currently connected to any voice channel.");
             return;
           }

           launch_task(bot, msg.channel_id, [this, &bot, msg] {
             std::lock_guard<std::mutex> operation_lock(voice_operation_mutex_);
             music_->stop();
             std::string channel_id;
             {
               std::lock_guard<std::mutex> lock(voice_mutex_);
               channel_id = current_channel_id_;
             }
             if (channel_id.empty()) {
               bot.send_message(msg.channel_id,
                                "I am not currently connected to a voice channel.");
               return;
             }
             bot.ws().leave_voice_channel(channel_id);
             {
               std::lock_guard<std::mutex> lock(voice_mutex_);
               voice_.disconnect();
               current_channel_id_.clear();
             }
             {
               std::lock_guard<std::mutex> lock(pulse_mutex_);
               pulse_.shutdown();
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

    // 5. Resolve a YouTube URL or search and enqueue it for playback.
    bot.register_command(
        {"play",
         {"p", "playyt", "yt", "youtube"},
         "play or queue a YouTube video by URL or search query.",
         "play <youtube_url_or_search>",
         {"<youtube_url_or_search>"},
         [this](stoatpp::cluster &bot, const stoatpp::events::Message &msg,
                const std::vector<std::string> &args) {
           if (args.empty()) {
             bot.send_message(msg.channel_id,
                              "Usage: `.play <YouTube URL or search query>`");
             return;
           }
           if (msg.server_id.empty()) {
             bot.send_message(msg.channel_id,
                              "[Music] This command can only be used in a server.");
             return;
           }
           const std::string query = join_args(args);
           launch_task(bot, msg.channel_id, [this, &bot, msg, query] {
             const std::string caller_channel =
                 user_voice_channel(msg.author.id);
             std::string current_channel = active_channel_id();
             std::string error;
             if (current_channel.empty()) {
               if (caller_channel.empty()) {
                 bot.send_message(
                     msg.channel_id,
                     "[Music] Join a voice channel first, or use `.join <channel>`.");
                 return;
               }
               if (!connect_to_voice(bot, caller_channel, error)) {
                 bot.send_message(msg.channel_id, "[Voice] " + error);
                 return;
               }
               current_channel = caller_channel;
             } else if (caller_channel.empty() ||
                        caller_channel != current_channel) {
               bot.send_message(
                   msg.channel_id,
                   "[Music] Join my current voice channel to control playback.");
               return;
             }
             if (!voice_is_connected() &&
                 !connect_to_voice(bot, current_channel, error)) {
               bot.send_message(msg.channel_id, "[Voice] " + error);
               return;
             }

             bot.send_message(msg.channel_id, "[Music] Searching YouTube...");
             auto track = resolve_youtube(query, error);
             if (!track) {
               bot.send_message(msg.channel_id, "[Music] " + error);
               return;
             }
             if (active_channel_id() != current_channel) {
               bot.send_message(
                   msg.channel_id,
                   "[Music] The voice connection changed before the track was queued.");
               return;
             }

             size_t position = 0;
             if (!music_->enqueue(std::move(*track), msg.channel_id, position)) {
               bot.send_message(msg.channel_id,
                                "[Music] The queue is full or shutting down.");
               return;
             }
             bot.send_message(msg.channel_id,
                              "[Music] Added to the queue at position " +
                                  std::to_string(position) + ".");
           });
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
           std::string error;
           if (!can_control_player(msg, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           bot.send_message(msg.channel_id, music_->queue_text());
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
           std::string error;
           if (!can_control_player(msg, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           bot.send_message(msg.channel_id, music_->skip());
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
           std::string error;
           if (!can_control_player(msg, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           bot.send_message(msg.channel_id, music_->pause());
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
           std::string error;
           if (!can_control_player(msg, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           bot.send_message(msg.channel_id, music_->resume());
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
           std::string error;
           if (!can_control_player(msg, error)) {
             bot.send_message(msg.channel_id, "[Music] " + error);
             return;
           }
           bot.send_message(msg.channel_id, music_->clear_queue());
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
           std::string error;
           if (!can_control_player(msg, error)) {
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
                 music_->set_volume(static_cast<double>(volume) / 100.0));
           } catch (const std::invalid_argument &) {
             bot.send_message(msg.channel_id,
                              "Volume must be a whole number from 0 to 200.");
           } catch (const std::out_of_range &) {
             bot.send_message(msg.channel_id,
                              "Volume must be a whole number from 0 to 200.");
           }
         },
         "Voice"});
  }

private:
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
    if (async_tasks_.size() >= 4) {
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

  std::string active_channel_id() const {
    std::lock_guard<std::mutex> lock(voice_mutex_);
    return current_channel_id_;
  }

  bool voice_is_connected() const {
    std::lock_guard<std::mutex> lock(voice_mutex_);
    return voice_.is_connected();
  }

  bool can_control_player(const stoatpp::events::Message &msg,
                          std::string &error) const {
    const std::string channel_id = active_channel_id();
    if (channel_id.empty()) {
      error = "I am not connected to a voice channel.";
      return false;
    }
    if (msg.server_id.empty() ||
        user_voice_channel(msg.author.id) != channel_id) {
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

  bool connect_to_voice(stoatpp::cluster &bot, const std::string &channel_id,
                        std::string &error) {
    std::lock_guard<std::mutex> operation_lock(voice_operation_mutex_);
    {
      std::lock_guard<std::mutex> lock(voice_mutex_);
      if (current_channel_id_ == channel_id && voice_.is_connected())
        return true;
    }

    music_->stop();
    std::string old_channel;
    {
      std::lock_guard<std::mutex> lock(voice_mutex_);
      old_channel = current_channel_id_;
      voice_.disconnect();
      current_channel_id_.clear();
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
      std::lock_guard<std::mutex> lock(voice_mutex_);
      if (!voice_.connect(url, token)) {
        voice_.disconnect();
        error = "Could not establish the voice connection.";
        return false;
      }
    }

    bool connected = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        connected = voice_.is_connected();
      }
      if (connected)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!connected) {
      std::lock_guard<std::mutex> lock(voice_mutex_);
      voice_.disconnect();
      error = "The voice server did not finish connecting.";
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(voice_mutex_);
      current_channel_id_ = channel_id;
    }
    bot.ws().join_voice_channel(channel_id);

    {
      std::lock_guard<std::mutex> lock(pulse_mutex_);
      if (pulse_.is_initialized())
        pulse_.shutdown();
      pulse_.init("bronx-bot", "voice-stream");
      pulse_.play_chime(true);
    }
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
            host.compare(host.size() - 12, 12, ".youtube.com") == 0);
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
      spawn_error = posix_spawn_file_actions_addopen(
          &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (spawn_error == 0)
      spawn_error = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    if (spawn_error == 0)
      spawn_error = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
    pid_t child = -1;
    if (spawn_error == 0) {
      std::vector<char *> argv = {
          const_cast<char *>("yt-dlp"),
          const_cast<char *>("--ignore-config"),
          const_cast<char *>("--socket-timeout"), const_cast<char *>("15"),
          const_cast<char *>("--retries"), const_cast<char *>("1"),
          const_cast<char *>("--quiet"),
          const_cast<char *>("--no-warnings"),
          const_cast<char *>("--skip-download"),
          const_cast<char *>("--dump-single-json"),
          const_cast<char *>("--no-playlist"), const_cast<char *>("--"),
          const_cast<char *>(target.c_str()), nullptr};
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

  static std::optional<VoiceMusicPlayer::Track>
  resolve_youtube(const std::string &query, std::string &error) {
    std::string target = query;
    if (query.rfind("http://", 0) == 0 ||
        query.rfind("https://", 0) == 0) {
      if (!supported_youtube_url(query)) {
        error = "Only YouTube video URLs are supported right now.";
        return std::nullopt;
      }
    } else {
      if (query.find("://") != std::string::npos) {
        error = "Only YouTube video URLs or search terms are supported.";
        return std::nullopt;
      }
      target = "ytsearch1:" + query;
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
  stoatpp::voice_connection voice_;
  std::unique_ptr<VoiceMusicPlayer> music_;
  mutable std::mutex voice_mutex_;
  std::mutex voice_operation_mutex_;
  std::mutex pulse_mutex_;
  mutable std::mutex voice_state_mutex_;
  std::unordered_map<std::string, std::string> user_voice_channels_;
  std::string current_channel_id_;
  std::mutex async_mutex_;
  std::vector<AsyncTask> async_tasks_;
};

} // namespace bronx::cogs
