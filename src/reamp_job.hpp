#ifndef VESPERA_REAMP_JOB_HPP
#define VESPERA_REAMP_JOB_HPP

#include "offline_render.hpp"
#include "wav_file.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vespera {

// Renders one recorded input WAV file through a freshly built graph on a worker thread and
// writes the stereo binary32 result, sample-aligned with the input, to a WAV file at the input's
// sample rate. The graph is stereo, as the live session's is. Output goes to a temporary file
// beside the destination that replaces the destination only once the render succeeds; a failed
// or cancelled render removes it.
//
// States move idle -> rendering in start(), and rendering -> done, failed or cancelled only on
// the worker, as its very last action. A finished state persists until the next start().
//
// The worker builds, prepares, releases and destroys the graph itself, so modules whose
// lifecycle runs on the main thread need the main run loop serviced while a render starts or
// ends, including during shutdown().
class ReampJob {
public:
  enum class State { idle, rendering, done, failed, cancelled };

  struct Request {
    std::filesystem::path input;
    std::filesystem::path output;
    CoreIoSettings core_io;
    double bpm = 120.0;
    uint32_t block_size = 128;
    // Adds the modules to an editing graph; runs on the worker.
    std::function<void(Graph &)> populate;
  };

  struct Snapshot {
    State state = State::idle;
    // Written frames over the input plus declared tail, monotonic, below 1 until the render is
    // done; output continuing past the declared tail holds it there.
    double progress = 0.0;
    std::filesystem::path output;
    float peak = 0.0f;
    std::string error;
  };

  ReampJob() = default;
  ~ReampJob();
  ReampJob(const ReampJob &) = delete;
  ReampJob &operator=(const ReampJob &) = delete;

  // Opens the input and creates the temporary output before returning, so missing or
  // unsupported input and an unwritable destination fail here rather than in the render. Throws
  // std::logic_error while rendering, std::invalid_argument for an invalid request and
  // std::runtime_error for files that cannot be used; the previous state then remains.
  void start(Request request);
  // Asks a running render to stop; it reaches the cancelled state once its files are removed.
  void cancel() noexcept;
  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] bool rendering() const;
  // Cancels and joins the worker.
  void shutdown();

private:
  void run(Request request, std::unique_ptr<WavReader> reader, std::unique_ptr<WavWriter> writer,
           std::filesystem::path temporary);
  void finish(State state, std::string error);

  mutable std::mutex mutex_;
  State state_ = State::idle;
  std::filesystem::path output_;
  std::string error_;
  std::atomic<double> progress_{0.0};
  std::atomic<float> peak_{0.0f};
  std::atomic<bool> cancel_{false};
  std::mutex join_mutex_;
  std::thread worker_;
};

const char *to_string(ReampJob::State state) noexcept;

}

#endif
