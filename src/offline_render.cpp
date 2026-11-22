#include "offline_render.hpp"

#include "samples.hpp"

#include <vespera/tempo.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace vespera {

namespace {

bool finite(const float *const *channels, uint32_t count, uint32_t frames) noexcept {
  for (uint32_t c = 0; c < count; ++c) {
    for (uint32_t i = 0; i < frames; ++i) {
      if (!std::isfinite(channels[c][i])) {
        return false;
      }
    }
  }
  return true;
}

uint64_t saturating_add(uint64_t a, uint64_t b) noexcept {
  return a > std::numeric_limits<uint64_t>::max() - b ? std::numeric_limits<uint64_t>::max()
                                                      : a + b;
}

}

OfflineRenderer::OfflineRenderer(Graph &graph, const OfflineRenderSettings &settings)
    : graph_(graph), settings_(settings) {
  TempoSettings tempo;
  tempo.bpm = settings.bpm;
  if (!validate_tempo_settings(tempo)) {
    throw std::invalid_argument("Invalid offline render tempo");
  }
  if (!std::isfinite(settings.sample_rate) || settings.sample_rate < 8000.0 ||
      settings.block_size == 0 || settings.block_size > offline_render_max_block_size ||
      settings.input_channels < 1 || settings.input_channels > 2 || settings.graph_channels < 1 ||
      settings.graph_channels > 2 || settings.input_channels > settings.graph_channels) {
    throw std::invalid_argument("Invalid offline render settings");
  }
  if (settings.core_io) {
    core_io_.emplace();
    core_io_->prepare(settings.sample_rate, settings.block_size,
                      settings.core_io->transpose_semitones == 0.0f ? TransposeLatency::none
                                                                    : TransposeLatency::fixed,
                      *settings.core_io);
    conditioned_.assign(static_cast<size_t>(settings.block_size) * settings.input_channels, 0.0f);
  }
  sanitised_.assign(static_cast<size_t>(settings.block_size) * settings.input_channels, 0.0f);
  adapter_.emplace(graph, settings.block_size, settings.graph_channels);
  graph.prepare(Format{settings.sample_rate, settings.block_size, settings.graph_channels});
}

OfflineRenderer::~OfflineRenderer() {
  try {
    graph_.release();
  } catch (...) {
  }
}

uint32_t OfflineRenderer::output_channels() const noexcept {
  return core_io_ ? 2 : settings_.graph_channels;
}

uint64_t OfflineRenderer::latency_frames() const noexcept {
  return static_cast<uint64_t>(graph_.latency_frames()) +
         (core_io_ ? core_io_->input_latency_frames() : 0);
}

uint64_t OfflineRenderer::tail_frames() const noexcept {
  const auto limit =
      static_cast<uint64_t>(std::llround(settings_.sample_rate * offline_render_max_tail_seconds));
  uint64_t tail = graph_.tail_frames();
  if (core_io_) {
    const auto &io = *settings_.core_io;
    if (io.doubler_enabled) {
      tail = saturating_add(
          tail, std::max<uint64_t>(1, static_cast<uint64_t>(std::lround(
                                          settings_.sample_rate * io.doubler_delay_ms / 1000.0))));
    }
    if (io.transpose_semitones != 0.0f) {
      tail = saturating_add(tail, core_io_->input_latency_frames());
    }
  }
  return std::min(tail, limit);
}

void OfflineRenderer::process(const float *const *inputs, float *const *outputs, uint32_t frames) {
  if (frames == 0 || frames > settings_.block_size) {
    throw std::invalid_argument("Offline render block exceeds the block size");
  }
  const uint32_t flags =
      VS_PROCESS_OFFLINE | (position_ == 0 ? uint32_t{VS_PROCESS_DISCONTINUITY} : 0u);
  const vs_tempo_context timing{
      sizeof(vs_tempo_context), VS_TEMPO_VALID | (position_ == 0 ? VS_TEMPO_DISCONTINUITY : 0u),
      settings_.bpm,
      static_cast<double>(position_) * settings_.bpm / (60.0 * settings_.sample_rate)};
  const auto at_frame = [&] { return std::to_string(position_); };
  const uint32_t input_channels = settings_.input_channels;
  const auto failed = [&](vs_result result) {
    return std::runtime_error("Offline render failed at frame " + at_frame() + " (" +
                              std::to_string(result) + ")");
  };
  std::array<float *, 2> sanitised{};
  if (!finite(inputs, input_channels, frames)) {
    for (uint32_t c = 0; c < input_channels; ++c) {
      sanitised[c] = sanitised_.data() + static_cast<size_t>(c) * settings_.block_size;
    }
    detail::copy_zeroing_non_finite(inputs, sanitised.data(), input_channels, frames);
    inputs = sanitised.data();
  }
  if (!core_io_) {
    if (const auto result = adapter_->process(std::span<const float *const>(inputs, input_channels),
                                              frames, position_, flags, {}, &timing);
        result != VS_OK) {
      throw failed(result);
    }
    for (uint32_t c = 0; c < settings_.graph_channels; ++c) {
      std::copy_n(adapter_->output(c), frames, outputs[c]);
    }
    position_ += frames;
    return;
  }
  if (position_ == 0) {
    core_io_->reset();
  }
  std::array<float *, 2> conditioned{};
  for (uint32_t c = 0; c < input_channels; ++c) {
    conditioned[c] = conditioned_.data() + static_cast<size_t>(c) * settings_.block_size;
  }
  if (!core_io_->process_input(inputs, conditioned.data(), input_channels, frames) ||
      !finite(conditioned.data(), input_channels, frames)) {
    throw std::runtime_error("Offline render input conditioning failed at frame " + at_frame());
  }
  const std::array<const float *, 2> graph_input{conditioned[0], conditioned[1]};
  const auto result =
      adapter_->process(std::span<const float *const>(graph_input.data(), input_channels), frames,
                        position_, flags, {}, &timing);
  if (result != VS_OK) {
    throw failed(result);
  }
  const std::array<const float *, 2> graph_output{adapter_->output(0), adapter_->output(1)};
  if (!core_io_->process_output(graph_output.data(), settings_.graph_channels, outputs, 2,
                                frames) ||
      !finite(outputs, 2, frames)) {
    throw std::runtime_error("Offline render output conditioning failed at frame " + at_frame());
  }
  position_ += frames;
}

OfflineRenderOutcome render_aligned(OfflineRenderer &renderer, const OfflineStream &stream) {
  if (renderer.position() != 0) {
    throw std::logic_error("An aligned render needs a renderer that has not processed audio");
  }
  const auto &settings = renderer.settings();
  const uint32_t block = settings.block_size;
  const uint32_t input_channels = settings.input_channels;
  const uint32_t output_channels = renderer.output_channels();
  std::vector<float> input(static_cast<size_t>(block) * input_channels);
  std::vector<float> output(static_cast<size_t>(block) * output_channels);
  std::array<float *, 2> input_channel{};
  std::array<float *, 2> output_channel{};
  for (uint32_t c = 0; c < input_channels; ++c) {
    input_channel[c] = input.data() + static_cast<size_t>(c) * block;
  }
  for (uint32_t c = 0; c < output_channels; ++c) {
    output_channel[c] = output.data() + static_cast<size_t>(c) * block;
  }
  const uint64_t latency = renderer.latency_frames();
  const uint64_t tail = renderer.tail_frames();
  const auto max_tail =
      static_cast<uint64_t>(std::llround(settings.sample_rate * offline_render_max_tail_seconds));
  const auto hold =
      static_cast<uint64_t>(std::llround(settings.sample_rate * offline_render_tail_hold_seconds));
  // input: every frame is written. declared_tail: frames up to the declared end are written.
  // extension: inaudible frames are held back until an audible one follows them, and the render
  // ends once `hold` of them are held or the maximum tail is reached.
  enum class Phase { input, declared_tail, extension };
  Phase phase = Phase::input;
  uint64_t input_frames = 0;
  uint64_t declared_end = 0;
  uint64_t limit = 0;
  uint64_t processed = 0;
  uint64_t written = 0;
  std::vector<std::vector<float>> held(output_channels);
  const auto emit = [&](const float *const *channels, uint64_t frames) {
    if (frames == 0) {
      return;
    }
    stream.write(channels, frames);
    written += frames;
    if (stream.progress) {
      stream.progress(written);
    }
  };
  const auto release_held = [&] {
    std::array<const float *, 2> frames{};
    for (uint32_t c = 0; c < output_channels; ++c) {
      frames[c] = held[c].data();
    }
    emit(frames.data(), held[0].size());
    for (auto &channel : held) {
      channel.clear();
    }
  };
  for (;;) {
    if (stream.cancelled && stream.cancelled()) {
      return OfflineRenderOutcome::cancelled;
    }
    uint64_t read = 0;
    if (phase == Phase::input) {
      read = stream.read(input_channel.data(), block);
      if (read > block) {
        throw std::logic_error("Offline render input returned more frames than requested");
      }
      input_frames += read;
      if (read < block) {
        phase = Phase::declared_tail;
        declared_end = input_frames + tail;
        limit = input_frames + std::max(tail, max_tail);
      }
    }
    for (uint32_t c = 0; c < input_channels; ++c) {
      std::fill(input_channel[c] + read, input_channel[c] + block, 0.0f);
    }
    renderer.process(input_channel.data(), output_channel.data(), block);
    const uint64_t begin = processed;
    processed += block;
    if (processed <= latency) {
      continue;
    }
    const uint64_t skip = begin < latency ? latency - begin : 0;
    const uint64_t count = block - skip;
    std::array<const float *, 2> chunk{};
    for (uint32_t c = 0; c < output_channels; ++c) {
      chunk[c] = output_channel[c] + skip;
    }
    if (phase == Phase::input) {
      emit(chunk.data(), count);
      continue;
    }
    uint64_t next = 0;
    if (phase == Phase::declared_tail) {
      next = std::min(count, declared_end > written ? declared_end - written : 0);
      emit(chunk.data(), next);
      if (written < declared_end) {
        continue;
      }
      phase = Phase::extension;
    }
    uint64_t unwritten = next;
    for (uint64_t i = next; i < count; ++i) {
      if (written + held[0].size() + (i - unwritten) >= limit) {
        return OfflineRenderOutcome::finished;
      }
      bool audible = false;
      for (uint32_t c = 0; c < output_channels; ++c) {
        audible = audible || std::abs(chunk[c][i]) > offline_render_audible_level;
      }
      if (audible) {
        release_held();
        std::array<const float *, 2> run{};
        for (uint32_t c = 0; c < output_channels; ++c) {
          run[c] = chunk[c] + unwritten;
        }
        emit(run.data(), i + 1 - unwritten);
        unwritten = i + 1;
      } else if (held[0].size() + (i + 1 - unwritten) >= hold) {
        return OfflineRenderOutcome::finished;
      }
    }
    for (uint32_t c = 0; c < output_channels; ++c) {
      held[c].insert(held[c].end(), chunk[c] + unwritten, chunk[c] + count);
    }
  }
}

}
