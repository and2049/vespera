#include <vespera/dsp/dsp.hpp>

#include <span>

int main() {
  vespera::dsp::LinearSmoother smoother;
  smoother.configure(48000.0, 0.0);
  smoother.set_target(0.5);

  vespera::dsp::ConvolverConfig config;
  config.channels = 1;
  config.max_response_frames = 1;
  const float tap = 0.5f;
  const std::span<const float> response(&tap, 1);
  const vespera::dsp::PreparedResponse prepared(
      config, std::span<const std::span<const float>>(&response, 1));
  vespera::dsp::Convolver convolver(config);
  convolver.set_response(&prepared);
  const float input = 2.0f;
  float output = 0.0f;
  convolver.process(std::span<const float>(&input, 1), std::span<float>(&output, 1));
  vespera::dsp::StateVariableFilter svf(48000.0, 1000.0, 0.7071067811865476);
  vespera::dsp::LinkwitzRileyCrossover crossover(48000.0, 1000.0);
  const bool filters_run =
      svf.try_set_cutoff(2000.0) && svf.process(1.0).low > 0.0 && crossover.process(1.0).high > 0.0;
  return smoother.process() == 0.5 && output == 1.0f && filters_run ? 0 : 1;
}
