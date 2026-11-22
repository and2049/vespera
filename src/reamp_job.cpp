#include "reamp_job.hpp"

#include <vespera/tempo.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <system_error>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace vespera {

namespace {

// Output can continue past the declared length while it stays audible, so progress against that
// length waits just short of completion, which only finish() reports.
constexpr double rendering_progress_limit = 0.999;

std::filesystem::path temporary_path(const std::filesystem::path &output) {
  return output.parent_path() / ("." + output.filename().string() + ".partial");
}

}

const char *to_string(ReampJob::State state) noexcept {
  switch (state) {
  case ReampJob::State::idle:
    return "idle";
  case ReampJob::State::rendering:
    return "rendering";
  case ReampJob::State::done:
    return "done";
  case ReampJob::State::failed:
    return "failed";
  case ReampJob::State::cancelled:
    return "cancelled";
  }
  return "unknown";
}

ReampJob::~ReampJob() { shutdown(); }

void ReampJob::start(Request request) {
  std::lock_guard lock(mutex_);
  if (state_ == State::rendering) {
    throw std::logic_error("A reamp render is already running");
  }
  {
    std::lock_guard join(join_mutex_);
    if (worker_.joinable()) {
      worker_.join();
    }
  }
  if (!request.input.is_absolute() || !request.output.is_absolute() ||
      !request.output.has_filename()) {
    throw std::invalid_argument("Reamp input and output must be absolute file paths");
  }
  TempoSettings tempo;
  tempo.bpm = request.bpm;
  if (!validate_tempo_settings(tempo)) {
    throw std::invalid_argument("Invalid reamp tempo");
  }
  if (request.block_size == 0 || request.block_size > offline_render_max_block_size) {
    throw std::invalid_argument("Invalid reamp block size");
  }
  if (!validate_core_io_settings(request.core_io) || !request.populate) {
    throw std::invalid_argument("Invalid reamp request");
  }
  auto reader = std::make_unique<WavReader>(request.input);
  if (reader->format().frames == 0) {
    throw std::invalid_argument("Reamp input has no audio");
  }
  std::error_code same_error;
  if (std::filesystem::equivalent(request.input, request.output, same_error)) {
    throw std::invalid_argument("Reamp output must differ from its input");
  }
  const auto temporary = temporary_path(request.output);
  std::unique_ptr<WavWriter> writer;
  try {
    writer = std::make_unique<WavWriter>(temporary, reader->format().sample_rate, 2);
  } catch (const std::runtime_error &) {
    throw std::runtime_error("Reamp output cannot be created in " +
                             request.output.parent_path().string());
  }
  const auto previous_state = state_;
  auto previous_output = output_;
  auto previous_error = error_;
  const auto previous_progress = progress_.load();
  const auto previous_peak = peak_.load();
  state_ = State::rendering;
  output_ = request.output;
  error_.clear();
  progress_.store(0.0);
  peak_.store(0.0f);
  cancel_.store(false);
  try {
    std::lock_guard join(join_mutex_);
    worker_ = std::thread(&ReampJob::run, this, std::move(request), std::move(reader),
                          std::move(writer), temporary);
  } catch (...) {
    state_ = previous_state;
    output_ = std::move(previous_output);
    error_ = std::move(previous_error);
    progress_.store(previous_progress);
    peak_.store(previous_peak);
    writer.reset();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

void ReampJob::cancel() noexcept { cancel_.store(true); }

ReampJob::Snapshot ReampJob::snapshot() const {
  std::lock_guard lock(mutex_);
  return {state_, progress_.load(), output_, peak_.load(), error_};
}

bool ReampJob::rendering() const {
  std::lock_guard lock(mutex_);
  return state_ == State::rendering;
}

void ReampJob::shutdown() {
  cancel();
  std::lock_guard join(join_mutex_);
  if (worker_.joinable()) {
    worker_.join();
  }
}

void ReampJob::finish(State state, std::string error) {
  std::lock_guard lock(mutex_);
  state_ = state;
  error_ = std::move(error);
  if (state == State::done) {
    progress_.store(1.0);
  }
}

void ReampJob::run(Request request, std::unique_ptr<WavReader> reader,
                   std::unique_ptr<WavWriter> writer, std::filesystem::path temporary) {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
  State outcome = State::done;
  std::string error;
  try {
    const auto format = reader->format();
    auto graph = std::make_unique<Graph>();
    request.populate(*graph);
    if (cancel_.load()) {
      outcome = State::cancelled;
    } else {
      OfflineRenderer renderer(*graph, {static_cast<double>(format.sample_rate), request.block_size,
                                        format.channels, 2, request.bpm, request.core_io});
      for (const auto &node : graph->nodes()) {
        if (const auto status = graph->module_status(node.instance_id);
            status && status->kind == VS_STATUS_ERROR) {
          throw std::runtime_error(node.module.name + ": " + status->message);
        }
      }
      const double total = static_cast<double>(format.frames + renderer.tail_frames());
      OfflineStream stream;
      stream.read = [&](float *const *channels, uint64_t frames) {
        return reader->read(channels, frames);
      };
      stream.write = [&](const float *const *channels, uint64_t frames) {
        float peak = peak_.load(std::memory_order_relaxed);
        for (uint32_t c = 0; c < 2; ++c) {
          for (uint64_t i = 0; i < frames; ++i) {
            peak = std::max(peak, std::abs(channels[c][i]));
          }
        }
        writer->write(channels, frames);
        peak_.store(peak, std::memory_order_relaxed);
      };
      stream.cancelled = [&] { return cancel_.load(std::memory_order_relaxed); };
      stream.progress = [&](uint64_t written) {
        progress_.store(std::min(rendering_progress_limit, static_cast<double>(written) / total),
                        std::memory_order_relaxed);
      };
      if (render_aligned(renderer, stream) == OfflineRenderOutcome::cancelled) {
        outcome = State::cancelled;
      }
    }
    graph.reset();
    if (outcome == State::done) {
      writer->close();
      writer.reset();
      std::filesystem::rename(temporary, request.output);
    }
  } catch (const std::exception &failure) {
    outcome = State::failed;
    error = failure.what();
  } catch (...) {
    outcome = State::failed;
    error = "Reamp render failed";
  }
  if (outcome != State::done) {
    writer.reset();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
  }
  reader.reset();
  request.populate = nullptr;
  finish(outcome, std::move(error));
}

}
