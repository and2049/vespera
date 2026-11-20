#include <vespera/dsp/delay.hpp>
#include <vespera/dsp/dynamics.hpp>
#include <vespera/dsp/filters.hpp>
#include <vespera/dsp/oscillator.hpp>
#include <vespera/dsp/oversampling.hpp>
#include <vespera/dsp/smoothing.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace {
template <typename Processor, typename Operation>
void rejected_preserves_sample(Processor &processor, Operation operation) {
  auto reference = processor;
  try {
    operation();
    std::abort();
  } catch (const std::invalid_argument &) {
  }
  if (processor.process(0.125f) != reference.process(0.125f)) {
    std::abort();
  }
}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 8) {
    return 0;
  }
  using namespace vespera::dsp;
  const double rate = 8000.0 + static_cast<double>(data[0]) * 2000.0;
  const size_t capacity = 1 + data[1];
  FractionalDelay delay({rate, capacity, static_cast<double>(capacity) * data[2] / 255.0});
  EnvelopeFollower envelope({rate, data[3] / 1000.0, data[4] / 1000.0});
  Lfo lfo({rate, data[5] * rate / 510.0, static_cast<LfoWaveform>(data[6] % 4), 0.0});
  LinearSmoother linear;
  ExponentialSmoother exponential;
  linear.configure(rate, data[7] / 1000.0);
  exponential.configure(rate, data[7] / 1000.0);
  const double frequency = rate * (0.00001 + 0.48998 * data[2] / 255.0);
  const double q = 0.05 + 49.95 * data[3] / 255.0;
  const double gain = -48.0 + 96.0 * data[4] / 255.0;
  Biquad filter(BiquadCoefficients::peak(rate, frequency, q, gain));
  FirstOrderFilter blocker(FirstOrderCoefficients::dc_blocker(rate));
  Oversampler oversampler(data[5] % 2 == 0 ? OversamplingFactor::two : OversamplingFactor::four);
  for (size_t i = 8; i < size; ++i) {
    const float input = (static_cast<float>(data[i]) - 128.0f) / 128.0f;
    if (data[i] % 17 == 0) {
      linear.set_target(input);
      exponential.set_target(input);
      if (!delay.try_set_delay_samples(static_cast<double>(capacity) * data[i] / 255.0)) {
        std::abort();
      }
      const double cutoff = rate * (0.001 + 0.48 * data[i] / 255.0);
      BiquadCoefficients coefficients;
      switch ((i + data[i]) % 7) {
      case 0:
        coefficients = BiquadCoefficients::lowpass(rate, cutoff, q);
        break;
      case 1:
        coefficients = BiquadCoefficients::highpass(rate, cutoff, q);
        break;
      case 2:
        coefficients = BiquadCoefficients::bandpass(rate, cutoff, q);
        break;
      case 3:
        coefficients = BiquadCoefficients::notch(rate, cutoff, q);
        break;
      case 4:
        coefficients = BiquadCoefficients::peak(rate, cutoff, q, gain);
        break;
      case 5:
        coefficients = BiquadCoefficients::low_shelf(rate, cutoff, q, gain);
        break;
      default:
        coefficients = BiquadCoefficients::high_shelf(rate, cutoff, q, gain);
        break;
      }
      filter.set_coefficients(coefficients);
      filter.reset();
      envelope.configure({rate, data[i] / 1000.0, data[4] / 1000.0});
      lfo.configure({rate, data[i] * rate / 510.0, static_cast<LfoWaveform>(data[i] % 4), 0.0});
    }
    if (data[i] % 31 == 0) {
      const double invalid = std::numeric_limits<double>::quiet_NaN();
      rejected_preserves_sample(delay, [&] { delay.prepare({rate, capacity, invalid}); });
      rejected_preserves_sample(envelope, [&] { envelope.configure({rate, invalid, 0.1}); });
      rejected_preserves_sample(
          filter, [&] { filter.set_coefficients(BiquadCoefficients::lowpass(rate, invalid, q)); });
      const auto reference = oversampler;
      try {
        oversampler.prepare(static_cast<OversamplingFactor>(3));
        std::abort();
      } catch (const std::invalid_argument &) {
      }
      auto expected = reference;
      const auto identity = [](double x) noexcept { return x; };
      if (oversampler.process(0.125, identity) != expected.process(0.125, identity)) {
        std::abort();
      }
    }
    if (data[i] == 255) {
      delay.prepare({rate, capacity, 0.5});
      oversampler.prepare(data[6] % 2 == 0 ? OversamplingFactor::two : OversamplingFactor::four);
    }
    const float delayed = delay.process(input);
    const float level = envelope.process(delayed);
    const float modulation = lfo.process();
    const double ramp = linear.process();
    const double smooth = exponential.process();
    const double filtered = filter.process(blocker.process(static_cast<double>(input)));
    const double oversampled =
        oversampler.process(input, [](double x) noexcept { return std::tanh(x); });
    if (!std::isfinite(delayed) || std::abs(delayed) > 1.0f || !std::isfinite(level) ||
        level < 0.0f || level > 1.0f || !std::isfinite(modulation) || std::abs(modulation) > 1.0f ||
        !std::isfinite(ramp) || std::abs(ramp) > 1.0 || !std::isfinite(smooth) ||
        std::abs(smooth) > 1.0 || !std::isfinite(filtered) || !std::isfinite(oversampled)) {
      std::abort();
    }
    if (data[i] == 0) {
      delay.reset();
      envelope.reset();
      lfo.reset();
      linear.reset();
      exponential.reset();
      filter.reset();
      blocker.reset();
      oversampler.reset();
    }
  }
  return 0;
}
