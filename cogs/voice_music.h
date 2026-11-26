#pragma once

#include "stoatpp/voice_connection.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace bronx::cogs {

class VoiceMusicPlayer {
public:
  struct Track {
    std::string title;
    std::string url;
    double duration_seconds = 0.0;
  };

  using WriteAudio = std::function<bool(const int16_t *, size_t)>;
  using SendMessage =
      std::function<void(const std::string &, const std::string &)>;
  using IsTtsSpeaking = std::function<bool()>;

  VoiceMusicPlayer(WriteAudio write_audio, SendMessage send_message,
                   IsTtsSpeaking is_tts_speaking = nullptr)
      : write_audio_(std::move(write_audio)),
        send_message_(std::move(send_message)),
        is_tts_speaking_(std::move(is_tts_speaking)) {
    worker_ = std::thread([this] { run(); });
  }

  VoiceMusicPlayer(const VoiceMusicPlayer &) = delete;
  VoiceMusicPlayer &operator=(const VoiceMusicPlayer &) = delete;

  ~VoiceMusicPlayer() { shutdown(); }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutting_down_)
        return;
      shutting_down_ = true;
      queue_.clear();
      cancel_current_ = true;
      terminate_active_locked();
    }
    condition_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
      worker_.join();
  }

  void stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    cancel_current_ = true;
    terminate_active_locked();
    condition_.notify_all();
  }

  bool enqueue(Track track, const std::string &message_channel,
               size_t &position) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_ || queue_.size() >= 100)
      return false;
    queue_.push_back({std::move(track), message_channel});
    position = queue_.size() + (current_ ? 1 : 0);
    condition_.notify_all();
    return true;
  }

  std::string skip() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_)
      return "Nothing is playing right now.";
    cancel_current_ = true;
    terminate_active_locked();
    condition_.notify_all();
    return "Skipped the current track.";
  }

  std::string pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_)
      return "Nothing is playing right now.";
    if (paused_)
      return "Playback is already paused.";
    paused_ = true;
    condition_.notify_all();
    return "Playback paused.";
  }

  std::string resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_)
      return "Nothing is playing right now.";
    if (!paused_)
      return "Playback is not paused.";
    paused_ = false;
    condition_.notify_all();
    return "Playback resumed.";
  }

  std::string clear_queue() {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t removed = queue_.size();
    queue_.clear();
    return "Cleared " + std::to_string(removed) + " queued track(s).";
  }

  std::string set_volume(double volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    volume_ = volume;
    return "Volume set to " + std::to_string(static_cast<int>(volume * 100)) +
           "%.";
  }

  bool is_playing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_.has_value();
  }

  std::string queue_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string result;
    if (current_) {
      result = "Now playing: " + current_->title;
      if (paused_)
        result += " (paused)";
      result += "\n";
    }
    if (queue_.empty()) {
      if (result.empty())
        return "The queue is empty.";
      return result + "The queue is empty.";
    }

    const size_t shown = std::min<size_t>(queue_.size(), 20);
    for (size_t i = 0; i < shown; ++i) {
      result += std::to_string(i + 1) + ". " + queue_[i].track.title + "\n";
    }
    if (queue_.size() > shown) {
      result += "... and " + std::to_string(queue_.size() - shown) +
                " more track(s).";
    }
    return result;
  }

  static std::string format_duration(double duration) {
    if (duration <= 0.0)
      return "live";
    const auto seconds = static_cast<unsigned long long>(duration);
    const auto minutes = seconds / 60;
    const auto remainder = seconds % 60;
    if (minutes >= 60) {
      return std::to_string(minutes / 60) + ":" +
             (minutes % 60 < 10 ? "0" : "") +
             std::to_string(minutes % 60) + ":" +
             (remainder < 10 ? "0" : "") + std::to_string(remainder);
    }
    return std::to_string(minutes) + ":" +
           (remainder < 10 ? "0" : "") + std::to_string(remainder);
  }

private:
  struct QueuedTrack {
    Track track;
    std::string message_channel;
  };

  enum class TrackResult { Finished, Cancelled, Failed };

  struct MediaProcess {
    pid_t extractor = -1;
    pid_t decoder = -1;
    int output = -1;
  };

  static bool make_pipe(int descriptors[2]) {
    return pipe(descriptors) == 0;
  }

  static void close_fd(int &fd) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }

  static bool start_media_process(const std::string &url, MediaProcess &media,
                                  std::string &error) {
    int source_pipe[2] = {-1, -1};
    int audio_pipe[2] = {-1, -1};
    if (!make_pipe(source_pipe) || !make_pipe(audio_pipe)) {
      error = "Could not create the audio streaming pipes.";
      close_fd(source_pipe[0]);
      close_fd(source_pipe[1]);
      close_fd(audio_pipe[0]);
      close_fd(audio_pipe[1]);
      return false;
    }

    posix_spawn_file_actions_t decoder_actions;
    int spawn_error = posix_spawn_file_actions_init(&decoder_actions);
    const bool decoder_actions_initialized = spawn_error == 0;
    if (spawn_error == 0)
      spawn_error =
          posix_spawn_file_actions_adddup2(&decoder_actions, source_pipe[0],
                                           STDIN_FILENO);
    if (spawn_error == 0)
      spawn_error =
          posix_spawn_file_actions_adddup2(&decoder_actions, audio_pipe[1],
                                           STDOUT_FILENO);
    for (int fd : {source_pipe[0], source_pipe[1], audio_pipe[0],
                   audio_pipe[1]}) {
      if (spawn_error == 0)
        spawn_error =
            posix_spawn_file_actions_addclose(&decoder_actions, fd);
    }
    pid_t decoder = -1;
    if (spawn_error == 0) {
      std::vector<char *> argv = {
          const_cast<char *>("ffmpeg"), const_cast<char *>("-hide_banner"),
          const_cast<char *>("-loglevel"), const_cast<char *>("error"),
          const_cast<char *>("-nostdin"), const_cast<char *>("-i"),
          const_cast<char *>("pipe:0"), const_cast<char *>("-vn"),
          const_cast<char *>("-ac"), const_cast<char *>("1"),
          const_cast<char *>("-ar"), const_cast<char *>("48000"),
          const_cast<char *>("-f"), const_cast<char *>("s16le"),
          const_cast<char *>("pipe:1"), nullptr};
      spawn_error = posix_spawnp(&decoder, "ffmpeg", &decoder_actions, nullptr,
                                 argv.data(), environ);
    }
    if (decoder_actions_initialized)
      posix_spawn_file_actions_destroy(&decoder_actions);
    if (spawn_error != 0) {
      error = "Could not start ffmpeg: " + std::string(std::strerror(spawn_error));
      close_fd(source_pipe[0]);
      close_fd(source_pipe[1]);
      close_fd(audio_pipe[0]);
      close_fd(audio_pipe[1]);
      return false;
    }

    posix_spawn_file_actions_t extractor_actions;
    spawn_error = posix_spawn_file_actions_init(&extractor_actions);
    const bool extractor_actions_initialized = spawn_error == 0;
    if (spawn_error == 0)
      spawn_error = posix_spawn_file_actions_adddup2(
          &extractor_actions, source_pipe[1], STDOUT_FILENO);
    if (spawn_error == 0)
      spawn_error = posix_spawn_file_actions_addopen(
          &extractor_actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    for (int fd : {source_pipe[0], source_pipe[1], audio_pipe[0],
                   audio_pipe[1]}) {
      if (spawn_error == 0)
        spawn_error =
            posix_spawn_file_actions_addclose(&extractor_actions, fd);
    }
    pid_t extractor = -1;
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
          const_cast<char *>("--fragment-retries"), const_cast<char *>("1"),
          const_cast<char *>("--quiet"),
          const_cast<char *>("--no-warnings"),
          const_cast<char *>("--no-playlist")};
      if (!cookies_path.empty()) {
        argv.push_back(const_cast<char *>("--cookies"));
        argv.push_back(const_cast<char *>(cookies_path.c_str()));
      }
      argv.push_back(const_cast<char *>("-f"));
      argv.push_back(const_cast<char *>("bestaudio/best"));
      argv.push_back(const_cast<char *>("-o"));
      argv.push_back(const_cast<char *>("-"));
      argv.push_back(const_cast<char *>("--"));
      argv.push_back(const_cast<char *>(url.c_str()));
      argv.push_back(nullptr);
      spawn_error =
          posix_spawnp(&extractor, "yt-dlp", &extractor_actions, nullptr,
                       argv.data(), environ);
    }
    if (extractor_actions_initialized)
      posix_spawn_file_actions_destroy(&extractor_actions);
    if (spawn_error != 0) {
      error = "Could not start yt-dlp: " + std::string(std::strerror(spawn_error));
      kill(decoder, SIGTERM);
      close_fd(source_pipe[0]);
      close_fd(source_pipe[1]);
      close_fd(audio_pipe[0]);
      close_fd(audio_pipe[1]);
      waitpid(decoder, nullptr, 0);
      return false;
    }

    close_fd(source_pipe[0]);
    close_fd(source_pipe[1]);
    close_fd(audio_pipe[1]);
    media.extractor = extractor;
    media.decoder = decoder;
    media.output = audio_pipe[0];
    return true;
  }

  static bool reap_process(pid_t pid, bool terminate) {
    if (pid <= 0)
      return true;
    if (terminate)
      kill(pid, SIGTERM);

    int status = 0;
    for (int attempt = 0; attempt < 40; ++attempt) {
      const pid_t result = waitpid(pid, &status, WNOHANG);
      if (result == pid)
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
      if (result < 0 && errno != EINTR)
        return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  void terminate_active_locked() {
    if (extractor_pid_ > 0)
      kill(extractor_pid_, SIGTERM);
    if (decoder_pid_ > 0)
      kill(decoder_pid_, SIGTERM);
  }

  bool cancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutting_down_ || cancel_current_;
  }

  void notify(const std::string &channel_id, const std::string &message) {
    try {
      send_message_(channel_id, message);
    } catch (const std::exception &error) {
      std::cerr << "[bronx music] could not send playback message: "
                << error.what() << "\n";
    }
  }

  bool wait_for_audio(std::chrono::steady_clock::duration duration) {
    auto remaining = duration;
    while (remaining > std::chrono::steady_clock::duration::zero()) {
      std::unique_lock<std::mutex> lock(mutex_);
      if (shutting_down_ || cancel_current_)
        return false;
      if (paused_) {
        condition_.wait(lock, [this] {
          return shutting_down_ || cancel_current_ || !paused_;
        });
        continue;
      }

      const auto started = std::chrono::steady_clock::now();
      condition_.wait_for(lock, remaining, [this] {
        return shutting_down_ || cancel_current_ || paused_;
      });
      const auto elapsed = std::chrono::steady_clock::now() - started;
      remaining -= std::min(elapsed, remaining);
    }
    return !cancelled();
  }

  TrackResult play_track(const QueuedTrack &queued, std::string &error) {
    const auto started = std::chrono::steady_clock::now();
    std::cout << "[bronx music] starting stream: \"" << queued.track.title
              << "\" (" << queued.track.url << ")" << std::endl;
    MediaProcess media;
    if (!start_media_process(queued.track.url, media, error)) {
      std::cerr << "[bronx music] start_media_process failed: " << error
                << std::endl;
      return TrackResult::Failed;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      extractor_pid_ = media.extractor;
      decoder_pid_ = media.decoder;
      if (shutting_down_ || cancel_current_)
        terminate_active_locked();
    }

    std::array<char, 4096> bytes{};
    std::vector<char> pending;
    bool received_audio = false;
    bool cancelled_playback = false;

    while (true) {
      if (cancelled()) {
        cancelled_playback = true;
        break;
      }

      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (paused_) {
          condition_.wait(lock, [this] {
            return shutting_down_ || cancel_current_ || !paused_;
          });
          continue;
        }
      }

      pollfd descriptor{media.output, POLLIN | POLLHUP, 0};
      const int poll_result = poll(&descriptor, 1, 100);
      if (poll_result < 0) {
        if (errno == EINTR)
          continue;
        error = "Audio stream read failed.";
        break;
      }
      if (poll_result == 0)
        continue;

      const ssize_t count = read(media.output, bytes.data(), bytes.size());
      if (count == 0)
        break;
      if (count < 0) {
        if (errno == EINTR || errno == EAGAIN)
          continue;
        error = "Audio stream read failed.";
        break;
      }

      pending.insert(pending.end(), bytes.begin(), bytes.begin() + count);
      const size_t usable_bytes = pending.size() - (pending.size() % 2);
      if (usable_bytes == 0)
        continue;

      std::vector<int16_t> samples(usable_bytes / sizeof(int16_t));
      std::memcpy(samples.data(), pending.data(), usable_bytes);
      pending.erase(pending.begin(), pending.begin() + usable_bytes);

      double volume = 1.0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        volume = volume_;
      }
      if (volume != 1.0) {
        for (auto &sample : samples) {
          const auto scaled = static_cast<long>(sample * volume);
          sample = static_cast<int16_t>(
              std::clamp<long>(scaled, -32768, 32767));
        }
      }

      if (!write_audio_(samples.data(), samples.size())) {
        error = "The voice connection is no longer available.";
        break;
      }
      if (!received_audio) {
        const auto first_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
        std::cout << "[bronx music] first PCM audio chunk after " << first_ms
                  << "ms" << std::endl;
      }
      received_audio = true;

      const auto frame_duration = std::chrono::duration_cast<
          std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(
              static_cast<double>(samples.size()) / 48000.0));
      if (!wait_for_audio(frame_duration)) {
        cancelled_playback = true;
        break;
      }
    }

    close_fd(media.output);
    const bool decoder_ok = reap_process(media.decoder, cancelled_playback);
    const bool extractor_ok = reap_process(media.extractor, cancelled_playback);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (extractor_pid_ == media.extractor)
        extractor_pid_ = -1;
      if (decoder_pid_ == media.decoder)
        decoder_pid_ = -1;
      cancelled_playback = cancelled_playback || shutting_down_ ||
                           cancel_current_;
    }

    if (cancelled_playback) {
      std::cout << "[bronx music] track cancelled: \"" << queued.track.title
                << "\"" << std::endl;
      return TrackResult::Cancelled;
    }
    if (!received_audio || !decoder_ok || !extractor_ok) {
      std::cerr << "[bronx music] track failed: \"" << queued.track.title
                << "\" (received_audio=" << received_audio
                << " decoder_ok=" << decoder_ok
                << " extractor_ok=" << extractor_ok
                << " error=\"" << error << "\")" << std::endl;
      if (error.empty())
        error =
            "Could not play this track. Check that yt-dlp and ffmpeg are installed "
            "and the video is available.";
      return TrackResult::Failed;
    }
    std::cout << "[bronx music] track finished: \"" << queued.track.title
              << "\"" << std::endl;
    return TrackResult::Finished;
  }

  void run() {
    while (true) {
      QueuedTrack queued;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock,
                        [this] { return shutting_down_ || !queue_.empty(); });
        if (shutting_down_)
          return;
        queued = std::move(queue_.front());
        queue_.pop_front();
        current_ = queued.track;
        cancel_current_ = false;
        paused_ = false;
      }

      notify(queued.message_channel,
             "🎵 Now playing **" + queued.track.title + "** (" +
                 format_duration(queued.track.duration_seconds) + ").");

      while (is_tts_speaking_ && is_tts_speaking_()) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutting_down_ || cancel_current_)
          break;
        condition_.wait_for(lock, std::chrono::milliseconds(50), [this] {
          return shutting_down_ || cancel_current_;
        });
      }

      std::string error;
      const TrackResult result = play_track(queued, error);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        current_.reset();
        cancel_current_ = false;
        paused_ = false;
      }
      condition_.notify_all();

      if (result == TrackResult::Failed)
        notify(queued.message_channel, "[Music] " + error);
    }
  }

  WriteAudio write_audio_;
  SendMessage send_message_;
  IsTtsSpeaking is_tts_speaking_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<QueuedTrack> queue_;
  std::optional<Track> current_;
  std::thread worker_;
  pid_t extractor_pid_ = -1;
  pid_t decoder_pid_ = -1;
  double volume_ = 1.0;
  bool paused_ = false;
  bool cancel_current_ = false;
  bool shutting_down_ = false;
};

} // namespace bronx::cogs
