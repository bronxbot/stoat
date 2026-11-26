#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace bronx::cogs {

class VoiceTtsPlayer {
public:
  struct Utterance {
    std::string text;
    std::string voice;
  };

  using WriteAudio = std::function<bool(const int16_t *, size_t)>;
  using IsMusicBusy = std::function<bool()>;

  VoiceTtsPlayer(WriteAudio write_audio, IsMusicBusy is_music_busy)
      : write_audio_(std::move(write_audio)),
        is_music_busy_(std::move(is_music_busy)) {
    worker_ = std::thread([this] { run(); });
  }

  VoiceTtsPlayer(const VoiceTtsPlayer &) = delete;
  VoiceTtsPlayer &operator=(const VoiceTtsPlayer &) = delete;

  ~VoiceTtsPlayer() { shutdown(); }

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

  void skip() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!speaking_)
      return;
    cancel_current_ = true;
    terminate_active_locked();
    condition_.notify_all();
  }

  bool enqueue(std::string text, std::string voice) {
    if (text.empty())
      return false;
    if (voice.empty())
      voice = "en-US-AndrewMultilingualNeural";

    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_ || queue_.size() >= 25)
      return false;
    queue_.push_back({std::move(text), std::move(voice)});
    condition_.notify_all();
    return true;
  }

  bool is_speaking() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return speaking_;
  }

private:
  struct MediaProcess {
    pid_t synthesizer = -1;
    pid_t decoder = -1;
    int output = -1;
  };

  static void close_fd(int &fd) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }

  static bool start_tts_process(const Utterance &item, MediaProcess &media,
                                std::string &error) {
    int source_pipe[2] = {-1, -1};
    int audio_pipe[2] = {-1, -1};
    if (pipe(source_pipe) != 0 || pipe(audio_pipe) != 0) {
      error = "Could not create TTS audio pipes.";
      close_fd(source_pipe[0]);
      close_fd(source_pipe[1]);
      close_fd(audio_pipe[0]);
      close_fd(audio_pipe[1]);
      return false;
    }

    posix_spawn_file_actions_t decoder_actions;
    int spawn_error = posix_spawn_file_actions_init(&decoder_actions);
    const bool decoder_init = spawn_error == 0;
    if (spawn_error == 0)
      spawn_error = posix_spawn_file_actions_adddup2(&decoder_actions,
                                                     source_pipe[0], STDIN_FILENO);
    if (spawn_error == 0)
      spawn_error = posix_spawn_file_actions_adddup2(&decoder_actions,
                                                     audio_pipe[1], STDOUT_FILENO);
    for (int fd : {source_pipe[0], source_pipe[1], audio_pipe[0], audio_pipe[1]}) {
      if (spawn_error == 0)
        spawn_error = posix_spawn_file_actions_addclose(&decoder_actions, fd);
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
    if (decoder_init)
      posix_spawn_file_actions_destroy(&decoder_actions);
    if (spawn_error != 0) {
      error = "Could not start ffmpeg for TTS: " +
              std::string(std::strerror(spawn_error));
      close_fd(source_pipe[0]);
      close_fd(source_pipe[1]);
      close_fd(audio_pipe[0]);
      close_fd(audio_pipe[1]);
      return false;
    }

    posix_spawn_file_actions_t synth_actions;
    spawn_error = posix_spawn_file_actions_init(&synth_actions);
    const bool synth_init = spawn_error == 0;
    if (spawn_error == 0)
      spawn_error = posix_spawn_file_actions_adddup2(&synth_actions,
                                                     source_pipe[1], STDOUT_FILENO);
    if (spawn_error == 0)
      spawn_error = posix_spawn_file_actions_addopen(&synth_actions,
                                                     STDIN_FILENO, "/dev/null",
                                                     O_RDONLY, 0);
    for (int fd : {source_pipe[0], source_pipe[1], audio_pipe[0], audio_pipe[1]}) {
      if (spawn_error == 0)
        spawn_error = posix_spawn_file_actions_addclose(&synth_actions, fd);
    }

    pid_t synth = -1;
    if (spawn_error == 0) {
      std::string voice_arg = "--voice=" + item.voice;
      std::string text_arg = "--text=" + item.text;
      std::vector<char *> argv = {
          const_cast<char *>("edge-tts"),
          const_cast<char *>(voice_arg.c_str()),
          const_cast<char *>(text_arg.c_str()),
          const_cast<char *>("--write-media"),
          const_cast<char *>("-"),
          nullptr};
      spawn_error = posix_spawnp(&synth, "edge-tts", &synth_actions, nullptr,
                                 argv.data(), environ);
    }
    if (synth_init)
      posix_spawn_file_actions_destroy(&synth_actions);
    if (spawn_error != 0) {
      error = "Could not start edge-tts: " +
              std::string(std::strerror(spawn_error));
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
    media.synthesizer = synth;
    media.decoder = decoder;
    media.output = audio_pipe[0];
    return true;
  }

  static void reap_process(pid_t pid, bool terminate) {
    if (pid <= 0)
      return;
    if (terminate)
      kill(pid, SIGTERM);

    int status = 0;
    for (int attempt = 0; attempt < 30; ++attempt) {
      const pid_t result = waitpid(pid, &status, WNOHANG);
      if (result == pid || (result < 0 && errno != EINTR))
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }

  void terminate_active_locked() {
    if (synthesizer_pid_ > 0)
      kill(synthesizer_pid_, SIGTERM);
    if (decoder_pid_ > 0)
      kill(decoder_pid_, SIGTERM);
  }

  bool cancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutting_down_ || cancel_current_;
  }

  bool wait_for_audio(std::chrono::steady_clock::duration duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto remaining = duration;
    while (remaining > std::chrono::steady_clock::duration::zero()) {
      if (shutting_down_ || cancel_current_)
        return false;
      const auto started = std::chrono::steady_clock::now();
      condition_.wait_for(lock, remaining,
                          [this] { return shutting_down_ || cancel_current_; });
      const auto elapsed = std::chrono::steady_clock::now() - started;
      remaining -= std::min(elapsed, remaining);
    }
    return !cancelled();
  }

  void speak_utterance(const Utterance &item) {
    MediaProcess media;
    std::string error;
    if (!start_tts_process(item, media, error)) {
      std::cerr << "[bronx tts] " << error << std::endl;
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      synthesizer_pid_ = media.synthesizer;
      decoder_pid_ = media.decoder;
      if (shutting_down_ || cancel_current_)
        terminate_active_locked();
    }

    std::array<char, 4096> bytes{};
    std::vector<char> pending;
    bool cancelled_playback = false;

    while (true) {
      if (cancelled()) {
        cancelled_playback = true;
        break;
      }

      pollfd descriptor{media.output, POLLIN | POLLHUP, 0};
      const int poll_result = poll(&descriptor, 1, 100);
      if (poll_result < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (poll_result == 0)
        continue;

      const ssize_t count = read(media.output, bytes.data(), bytes.size());
      if (count <= 0) {
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
          continue;
        break;
      }

      pending.insert(pending.end(), bytes.begin(), bytes.begin() + count);
      const size_t usable_bytes = pending.size() - (pending.size() % 2);
      if (usable_bytes == 0)
        continue;

      std::vector<int16_t> samples(usable_bytes / sizeof(int16_t));
      std::memcpy(samples.data(), pending.data(), usable_bytes);
      pending.erase(pending.begin(), pending.begin() + usable_bytes);

      if (!write_audio_(samples.data(), samples.size()))
        break;

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
    reap_process(media.decoder, cancelled_playback);
    reap_process(media.synthesizer, cancelled_playback);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (synthesizer_pid_ == media.synthesizer)
        synthesizer_pid_ = -1;
      if (decoder_pid_ == media.decoder)
        decoder_pid_ = -1;
    }
  }

  void run() {
    while (true) {
      Utterance item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock,
                        [this] { return shutting_down_ || !queue_.empty(); });
        if (shutting_down_)
          return;
        item = std::move(queue_.front());
        queue_.pop_front();
        cancel_current_ = false;
      }

      // Wait until music playback finishes before speaking
      while (is_music_busy_ && is_music_busy_()) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (shutting_down_ || cancel_current_)
          break;
        condition_.wait_for(lock, std::chrono::milliseconds(100),
                            [this] { return shutting_down_ || cancel_current_; });
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_ || cancel_current_) {
          cancel_current_ = false;
          continue;
        }
        speaking_ = true;
      }

      speak_utterance(item);

      {
        std::lock_guard<std::mutex> lock(mutex_);
        speaking_ = false;
        cancel_current_ = false;
      }
      condition_.notify_all();
    }
  }

  WriteAudio write_audio_;
  IsMusicBusy is_music_busy_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Utterance> queue_;
  std::thread worker_;
  pid_t synthesizer_pid_ = -1;
  pid_t decoder_pid_ = -1;
  bool speaking_ = false;
  bool cancel_current_ = false;
  bool shutting_down_ = false;
};

} // namespace bronx::cogs
