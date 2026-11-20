#ifndef VESPERA_TEST_FILE_CONFIG_HPP
#define VESPERA_TEST_FILE_CONFIG_HPP

#include <vespera/dsp/convolution.hpp>
#include <vespera/kit/module.hpp>

namespace file_fixture {
namespace kit = vespera::kit;
namespace dsp = vespera::dsp;

struct Config {
  static constexpr const char *id = "test.file.convolution";
  static constexpr const char *name = "File convolution";
  static constexpr const char *version = "1.0.0";
  static constexpr uint32_t layouts = VS_LAYOUT_MONO | VS_LAYOUT_STEREO;
  static constexpr auto parameters = kit::parameters(kit::knob(1, "Level", 1));
  static constexpr auto files =
      kit::files(kit::file(1, "Response").types("wav").bundled("impulses").limit(32768));

  struct Result {
    std::shared_ptr<const dsp::PreparedResponse> response;
    dsp::Convolver convolver;
    Result(const dsp::ConvolverConfig &config, const kit::AudioData &audio)
        : response(make_response(config, audio)), convolver(config) {
      convolver.set_response(response.get());
    }
    static std::shared_ptr<const dsp::PreparedResponse>
    make_response(const dsp::ConvolverConfig &config, const kit::AudioData &audio) {
      std::array<std::span<const float>, 2> channels;
      for (size_t i = 0; i < audio.channels.size(); ++i) {
        channels[i] = audio.channels[i];
      }
      return std::make_shared<dsp::PreparedResponse>(
          config, std::span<const std::span<const float>>(channels.data(), audio.channels.size()));
    }
  };

  struct Processor {
    static constexpr uint32_t latency_frames = 0;
    static constexpr uint64_t tail_frames = 4096;
    kit::FileWork<Result> response;
    float level;
    Processor(const kit::Setup &setup, kit::Host &host)
        : response(host.file<Result>(
              1,
              [setup](const kit::File &file, kit::Job &job) {
                auto audio = kit::read_audio(file, job);
                if (audio.sample_rate != setup.sample_rate ||
                    audio.channels.front().size() > 4096) {
                  throw std::runtime_error("Response rate or length is unsupported");
                }
                return Result({setup.sample_rate, setup.channels, 4096, 0.01, {}}, audio);
              })),
          level(setup.values[0]) {}
    void reset() noexcept {
      response.reset([](Result &value) { value.convolver.reset(); });
    }
    void set_parameter(size_t, float value) noexcept { level = value; }
    void process(kit::Block &block) noexcept {
      kit::crossfade(response, block, [](Result &value, kit::Block &span) {
        value.convolver.process({span.inputs.data(), span.channels},
                                {span.outputs.data(), span.channels}, span.frames);
      });
      for (uint32_t c = 0; c < block.channels; ++c) {
        for (uint32_t i = 0; i < block.frames; ++i) {
          block.outputs[c][i] *= level;
        }
      }
    }
  };
};
}

#endif
