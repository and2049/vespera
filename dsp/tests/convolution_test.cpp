#include "support.hpp"

#include <vespera/dsp/convolution.hpp>
#include <vespera/dsp/utility.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace test_support;
using vespera::dsp::ConvolutionLayout;
using vespera::dsp::Convolver;
using vespera::dsp::ConvolverConfig;
using vespera::dsp::ConvolverSwitch;
using vespera::dsp::PreparedResponse;
using vespera::dsp::ResponseExchange;

using Signal = std::vector<float>;

constexpr double epsilon = std::numeric_limits<float>::epsilon();

Signal random_signal(std::size_t count, std::uint32_t seed, double amplitude = 1.0) {
  const std::vector<double> values = noise(count, seed, amplitude);
  return {values.begin(), values.end()};
}

Signal decaying_response(std::size_t count, std::uint32_t seed) {
  Signal response = random_signal(count, seed);
  for (std::size_t k = 0; k < count; ++k) {
    response[k] *=
        static_cast<float>(std::exp(-4.0 * static_cast<double>(k) / static_cast<double>(count)));
  }
  return response;
}

ConvolverConfig config_for(std::size_t channels, std::size_t frames, double fade = 0.002) {
  ConvolverConfig config;
  config.sample_rate = 48000.0;
  config.channels = channels;
  config.max_response_frames = frames;
  config.fade_seconds = fade;
  return config;
}

std::unique_ptr<PreparedResponse> prepare(const ConvolverConfig &config,
                                          std::initializer_list<const Signal *> channels) {
  std::vector<std::span<const float>> spans;
  for (const Signal *channel : channels) {
    spans.emplace_back(*channel);
  }
  return std::make_unique<PreparedResponse>(config, spans);
}

// Processes every channel in blocks whose sizes cycle through `blocks`; `before_block` runs with
// the frame position before each block.
template <typename BeforeBlock>
std::vector<Signal> render(Convolver &convolver, const std::vector<Signal> &input,
                           const std::vector<std::size_t> &blocks, BeforeBlock &&before_block) {
  const std::size_t frames = input[0].size();
  std::vector<Signal> output(input.size(), Signal(frames, 0.0f));
  std::size_t position = 0;
  std::size_t next = 0;
  while (position < frames) {
    const std::size_t count = std::min(blocks[next++ % blocks.size()], frames - position);
    before_block(position);
    std::array<const float *, 2> in{};
    std::array<float *, 2> out{};
    for (std::size_t c = 0; c < input.size(); ++c) {
      in[c] = input[c].data() + position;
      out[c] = output[c].data() + position;
    }
    convolver.process(std::span<const float *const>(in.data(), input.size()),
                      std::span<float *const>(out.data(), input.size()), count);
    position += count;
  }
  return output;
}

std::vector<Signal> render(Convolver &convolver, const std::vector<Signal> &input,
                           const std::vector<std::size_t> &blocks = {64}) {
  return render(convolver, input, blocks, [](std::size_t) {});
}

Signal render_mono(const ConvolverConfig &config, const PreparedResponse *response,
                   const Signal &input, const std::vector<std::size_t> &blocks = {64}) {
  Convolver convolver(config);
  convolver.set_response(response);
  return render(convolver, {input}, blocks)[0];
}

double reference_sample(const Signal &response, const Signal &input, std::size_t n) {
  double sum = 0.0;
  const std::size_t taps = std::min(response.size(), n + 1);
  for (std::size_t k = 0; k < taps; ++k) {
    sum += static_cast<double>(response[k]) * static_cast<double>(input[n - k]);
  }
  return sum;
}

double absolute_sum(const Signal &values) {
  double sum = 0.0;
  for (float value : values) {
    sum += std::abs(static_cast<double>(value));
  }
  return sum;
}

double peak(const Signal &values) {
  double result = 0.0;
  for (float value : values) {
    result = std::max(result, std::abs(static_cast<double>(value)));
  }
  return result;
}

// Single-precision error envelope relative to sum|h| * max|x|, which bounds |y|. The head's
// pairwise-reduced dot product contributes about (head / 16 + 4) epsilon of that scale, and each
// level's overlap-save path a forward transform, a complex product and an inverse transform, each
// within about 8 epsilon per radix-2 stage of the norm of its block. Measured errors are near
// 1 epsilon of the scale; this keeps a margin of more than ten times for noise-like signals while
// still catching any misplaced or missing partition, which errs by a whole tap's contribution.
double tolerance(const ConvolutionLayout &layout, const Signal &response, const Signal &input) {
  const double stages =
      std::log2(static_cast<double>(std::max<std::size_t>(2 * layout.largest_partition(), 2)));
  const double factor =
      static_cast<double>(layout.head_frames) / 16.0 + 4.0 + 16.0 * (stages + 2.0);
  return factor * epsilon * absolute_sum(response) * peak(input);
}

void check_against_reference(const char *label, const ConvolverConfig &config,
                             const Signal &response, const Signal &input,
                             const std::vector<std::size_t> &blocks, std::size_t stride = 1) {
  const auto prepared = prepare(config, {&response});
  const Signal output = render_mono(config, prepared.get(), input, blocks);
  const double limit = tolerance(prepared->layout(), response, input);
  double worst = 0.0;
  std::size_t worst_index = 0;
  for (std::size_t n = 0; n < input.size(); n += stride) {
    const double error =
        std::abs(static_cast<double>(output[n]) - reference_sample(response, input, n));
    if (error > worst) {
      worst = error;
      worst_index = n;
    }
  }
  if (!(worst <= limit)) {
    std::fprintf(stderr, "%s: %s: length %zu error %.3g at %zu exceeds %.3g\n", current_test, label,
                 response.size(), worst, worst_index, limit);
    ++failures;
  }
}

// Partition boundaries of a layout: head end, then every partition start.
std::vector<std::size_t> boundaries(const ConvolutionLayout &layout) {
  std::vector<std::size_t> result{layout.head_frames};
  std::size_t start = layout.head_frames;
  for (const auto &level : layout.active_levels()) {
    for (std::size_t p = 0; p < level.partitions; ++p) {
      start += level.partition_frames;
      result.push_back(start);
    }
  }
  return result;
}

void test_layout_plan() {
  for (std::size_t frames :
       {std::size_t{1}, std::size_t{2}, std::size_t{17}, std::size_t{100}, std::size_t{960},
        std::size_t{4800}, std::size_t{9600}, std::size_t{96000}, std::size_t{192000},
        vespera::dsp::maximum_response_frames}) {
    const ConvolutionLayout layout = ConvolutionLayout::plan(frames);
    CHECK(layout.capacity_frames() >= frames);
    CHECK(layout.head_frames % vespera::dsp::convolution_head_alignment == 0);
    std::size_t start = layout.head_frames;
    for (std::size_t i = 0; i < layout.level_count; ++i) {
      const auto &level = layout.levels[i];
      CHECK(start == 2 * level.partition_frames);
      CHECK(level.partitions >= 1);
      if (i + 1 < layout.level_count) {
        CHECK(level.partitions == 2);
        CHECK(layout.levels[i + 1].partition_frames == 2 * level.partition_frames);
      }
      start += level.partitions * level.partition_frames;
    }
    CHECK(layout.largest_partition() <= ConvolutionLayout::largest_partition_limit);
    CHECK(layout.capacity_frames() - frames <
          std::max<std::size_t>(16, layout.largest_partition()));
  }
  CHECK(ConvolutionLayout::plan(2).level_count == 0);
  CHECK(ConvolutionLayout::plan(192000).level_count > 3);
  CHECK(throws_invalid([] { (void)ConvolutionLayout::plan(0); }));
  CHECK(throws_invalid(
      [] { (void)ConvolutionLayout::plan(vespera::dsp::maximum_response_frames + 1); }));
  CHECK(throws_invalid([] { (void)ConvolutionLayout::doubling(100, 12, 64); }));
  CHECK(throws_invalid([] { (void)ConvolutionLayout::doubling(100, 64, 32); }));
  CHECK(throws_invalid([] { (void)ConvolutionLayout::doubling(100, 16, 16384); }));
}

void test_rejects_invalid_configuration() {
  const auto rejects = [](auto mutate) {
    ConvolverConfig config = config_for(2, 1000);
    mutate(config);
    return throws_invalid([&] { Convolver convolver(config); }) && throws_invalid([&] {
             const Signal response(10, 0.5f);
             (void)prepare(config, {&response});
           });
  };
  CHECK(rejects([](ConvolverConfig &c) { c.sample_rate = 7999.0; }));
  CHECK(rejects([](ConvolverConfig &c) { c.sample_rate = qnan; }));
  CHECK(rejects([](ConvolverConfig &c) { c.sample_rate = 768001.0; }));
  CHECK(rejects([](ConvolverConfig &c) { c.channels = 0; }));
  CHECK(rejects([](ConvolverConfig &c) { c.channels = 3; }));
  CHECK(rejects([](ConvolverConfig &c) { c.max_response_frames = 0; }));
  CHECK(rejects([](ConvolverConfig &c) {
    c.max_response_frames = vespera::dsp::maximum_response_frames + 1;
  }));
  CHECK(rejects([](ConvolverConfig &c) { c.fade_seconds = -0.001; }));
  CHECK(rejects([](ConvolverConfig &c) { c.fade_seconds = 10.5; }));
  CHECK(rejects([](ConvolverConfig &c) { c.fade_seconds = inf; }));
  CHECK(rejects([](ConvolverConfig &c) { c.layout = ConvolutionLayout::direct(1100); }));
  CHECK(rejects([](ConvolverConfig &c) { c.layout = ConvolutionLayout::doubling(2000, 16, 64); }));
  CHECK(rejects([](ConvolverConfig &c) {
    ConvolutionLayout layout = ConvolutionLayout::doubling(1000, 16, 64);
    layout.levels[0].partitions = 3;
    c.layout = layout;
  }));
  ConvolverConfig accepted = config_for(1, 1000);
  accepted.layout = ConvolutionLayout::doubling(1000, 32, 128);
  Convolver convolver(accepted);
  CHECK(convolver.layout() == *accepted.layout);
  CHECK(Convolver::latency_frames() == 0);
}

void test_rejects_invalid_responses() {
  const ConvolverConfig config = config_for(2, 1000);
  const Signal ok(1000, 0.1f);
  const Signal too_long(1001, 0.1f);
  const Signal empty;
  Signal non_finite(10, 0.1f);
  non_finite[3] = std::numeric_limits<float>::infinity();
  CHECK(throws_invalid([&] { (void)prepare(config, {&too_long}); }));
  CHECK(throws_invalid([&] { (void)prepare(config, {&ok, &too_long}); }));
  CHECK(throws_invalid([&] { (void)prepare(config, {&empty}); }));
  CHECK(throws_invalid([&] { (void)prepare(config, {&non_finite}); }));
  CHECK(throws_invalid([&] { (void)prepare(config, {}); }));
  CHECK(throws_invalid([&] { (void)prepare(config, {&ok, &ok, &ok}); }));
  const auto stereo = prepare(config, {&ok, &ok});
  CHECK(stereo->channels() == 2 && stereo->frames() == 1000);

  Convolver mono(config_for(1, 1000));
  CHECK(!mono.accepts(stereo.get()));
  CHECK(throws_invalid([&] { mono.set_response(stereo.get()); }));
  CHECK(!mono.crossfade_to(stereo.get()));
  ConvolverConfig other_rate = config_for(1, 1000);
  other_rate.sample_rate = 44100.0;
  const auto wrong_rate = prepare(other_rate, {&ok});
  CHECK(!mono.accepts(wrong_rate.get()));
  const auto wrong_layout = prepare(config_for(1, 4000), {&ok});
  CHECK(!mono.accepts(wrong_layout.get()));
  CHECK(!mono.crossfade_to(wrong_layout.get()));
  CHECK(mono.accepts(nullptr));
  CHECK(mono.response() == nullptr && mono.take_retired() == nullptr);
}

void test_matches_direct_convolution() {
  const std::vector<std::size_t> blocks = {64};
  for (std::size_t length : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{31},
                             std::size_t{100}, std::size_t{1000}, std::size_t{4801}}) {
    const Signal response = decaying_response(length, static_cast<std::uint32_t>(length));
    const Signal input = random_signal(length + 3000, 99u);
    check_against_reference("planned", config_for(1, length), response, input, blocks);
  }
  for (double scale : {1e-20, 1e20}) {
    const Signal response = decaying_response(3000, 4u);
    Signal input = random_signal(8000, 5u);
    for (float &sample : input) {
      sample = static_cast<float>(sample * scale);
    }
    check_against_reference("scaled input", config_for(1, 3000), response, input, blocks);
  }
}

// Every response length one below, at and above each partition boundary of a fixed layout, and a
// single tap at each of those positions, which exposes a misplaced partition exactly.
void test_partition_boundaries() {
  const std::size_t capacity = 2000;
  ConvolverConfig config = config_for(1, capacity);
  config.layout = ConvolutionLayout::doubling(capacity, 16, 128);
  const Signal input = random_signal(capacity + 1200, 21u);
  for (std::size_t boundary : boundaries(*config.layout)) {
    for (std::size_t length : {boundary - 1, boundary, boundary + 1}) {
      if (length == 0 || length > capacity) {
        continue;
      }
      const Signal response = random_signal(length, static_cast<std::uint32_t>(length));
      check_against_reference("dense", config, response, input, {37});
      Signal tap(length, 0.0f);
      tap[length - 1] = 0.75f;
      const auto prepared = prepare(config, {&tap});
      const Signal output = render_mono(config, prepared.get(), input, {37});
      double worst = 0.0;
      for (std::size_t n = 0; n < input.size(); ++n) {
        const double expected = n + 1 >= length ? 0.75 * input[n + 1 - length] : 0.0;
        worst = std::max(worst, std::abs(output[n] - expected));
      }
      if (!(worst <= 64.0 * epsilon)) {
        std::fprintf(stderr, "%s: single tap at %zu errs by %.3g\n", current_test, length - 1,
                     worst);
        ++failures;
      }
    }
  }
}

void test_long_response() {
  for (std::size_t length : {std::size_t{96000}, std::size_t{192000}}) {
    const Signal response = decaying_response(length, 8u);
    const Signal input = random_signal(length + 9000, 9u);
    check_against_reference("long", config_for(1, length), response, input, {512}, 997);
  }
}

void test_impulse_and_latency() {
  const Signal response = decaying_response(5000, 31u);
  const ConvolverConfig config = config_for(1, 5000);
  const auto prepared = prepare(config, {&response});
  Signal input(7000, 0.0f);
  input[0] = 1.0f;
  const Signal output = render_mono(config, prepared.get(), input, {1, 13, 64});
  CHECK(output[0] == response[0]);
  const double limit = tolerance(prepared->layout(), response, input);
  for (std::size_t n = 0; n < output.size(); ++n) {
    CHECK_NEAR(output[n], n < response.size() ? response[n] : 0.0, limit);
  }
  Signal delayed(7000, 0.0f);
  delayed[123] = 1.0f;
  const Signal shifted = render_mono(config, prepared.get(), delayed);
  for (std::size_t n = 0; n < 123; ++n) {
    CHECK(shifted[n] == 0.0f);
  }
  CHECK(shifted[123] == response[0]);
}

void test_linearity() {
  const Signal response = decaying_response(3000, 41u);
  const ConvolverConfig config = config_for(1, 3000);
  const auto prepared = prepare(config, {&response});
  const Signal a = random_signal(9000, 42u);
  const Signal b = random_signal(9000, 43u);
  Signal mix(9000);
  for (std::size_t n = 0; n < mix.size(); ++n) {
    mix[n] = 0.5f * a[n] - 0.25f * b[n];
  }
  const Signal ya = render_mono(config, prepared.get(), a);
  const Signal yb = render_mono(config, prepared.get(), b);
  const Signal ymix = render_mono(config, prepared.get(), mix);
  const double limit = 2.0 * tolerance(prepared->layout(), response, a);
  for (std::size_t n = 0; n < mix.size(); ++n) {
    CHECK_NEAR(ymix[n], 0.5 * ya[n] - 0.25 * yb[n], limit);
  }
}

void test_mono_and_stereo() {
  const Signal left_response = decaying_response(2500, 51u);
  const Signal right_response = decaying_response(1800, 52u);
  const Signal left = random_signal(6000, 53u);
  const Signal right = random_signal(6000, 54u);
  const ConvolverConfig mono_config = config_for(1, 2500);
  const ConvolverConfig stereo_config = config_for(2, 2500);
  const auto left_mono = prepare(mono_config, {&left_response});
  const auto right_mono = prepare(mono_config, {&right_response});
  const auto pair = prepare(stereo_config, {&left_response, &right_response});
  const auto shared = prepare(stereo_config, {&left_response});
  CHECK(shared->channels() == 1);

  Convolver stereo(stereo_config);
  stereo.set_response(pair.get());
  const auto both = render(stereo, {left, right}, {50});
  CHECK(count_mismatches(both[0], render_mono(mono_config, left_mono.get(), left), "left") == 0);
  CHECK(count_mismatches(both[1], render_mono(mono_config, right_mono.get(), right), "right") == 0);

  Convolver shared_stereo(stereo_config);
  shared_stereo.set_response(shared.get());
  const auto shared_out = render(shared_stereo, {left, right}, {50});
  CHECK(count_mismatches(shared_out[0], render_mono(mono_config, left_mono.get(), left),
                         "shared left") == 0);
  CHECK(count_mismatches(shared_out[1], render_mono(mono_config, left_mono.get(), right),
                         "shared right") == 0);
}

void test_block_size_independence() {
  for (std::size_t length : {std::size_t{20}, std::size_t{700}, std::size_t{9600}}) {
    const ConvolverConfig config = config_for(2, length);
    const Signal response = decaying_response(length, 61u);
    const auto prepared = prepare(config, {&response});
    const std::vector<Signal> input = {random_signal(70000, 62u), random_signal(70000, 63u)};
    Convolver reference(config);
    reference.set_response(prepared.get());
    const auto expected = render(reference, input, {1});
    for (const std::vector<std::size_t> &blocks : {std::vector<std::size_t>{2},
                                                   {3},
                                                   {16},
                                                   {63},
                                                   {64},
                                                   {65},
                                                   {511},
                                                   {4096},
                                                   {65536},
                                                   {1, 1000, 7, 64, 3, 4097, 32, 65536}}) {
      Convolver convolver(config);
      convolver.set_response(prepared.get());
      const auto output = render(convolver, input, blocks);
      CHECK(count_mismatches(output[0], expected[0], "left blocks") == 0);
      CHECK(count_mismatches(output[1], expected[1], "right blocks") == 0);
    }
  }
}

void test_in_place() {
  const ConvolverConfig config = config_for(2, 3000);
  const Signal response = decaying_response(3000, 64u);
  const auto prepared = prepare(config, {&response});
  const std::vector<Signal> input = {random_signal(9000, 65u), random_signal(9000, 66u)};
  Convolver separate(config);
  separate.set_response(prepared.get());
  const auto expected = render(separate, input, {100});
  Convolver in_place(config);
  in_place.set_response(prepared.get());
  std::vector<Signal> buffers = input;
  for (std::size_t position = 0; position < 9000; position += 100) {
    std::array<const float *, 2> in{buffers[0].data() + position, buffers[1].data() + position};
    std::array<float *, 2> out{buffers[0].data() + position, buffers[1].data() + position};
    in_place.process(in, out, 100);
  }
  CHECK(count_mismatches(buffers[0], expected[0], "in place left") == 0);
  CHECK(count_mismatches(buffers[1], expected[1], "in place right") == 0);
}

void test_reset() {
  const ConvolverConfig config = config_for(1, 3000);
  const Signal response = decaying_response(3000, 71u);
  const auto prepared = prepare(config, {&response});
  const Signal first = random_signal(5000, 72u);
  const Signal second = random_signal(5000, 73u);
  Convolver convolver(config);
  convolver.set_response(prepared.get());
  (void)render(convolver, {first}, {77});
  convolver.reset();
  const Signal after = render(convolver, {second}, {77})[0];
  CHECK(count_mismatches(after, render_mono(config, prepared.get(), second), "after reset") == 0);
  CHECK(convolver.response() == prepared.get());
}

void test_non_finite_and_extreme_input() {
  const std::size_t length = 1500;
  const ConvolverConfig config = config_for(1, length);
  const Signal response = decaying_response(length, 81u);
  const auto prepared = prepare(config, {&response});
  Signal input = random_signal(12000, 82u);
  Signal cleaned = input;
  for (std::size_t n : {std::size_t{10}, std::size_t{700}, std::size_t{701}, std::size_t{4000}}) {
    input[n] = n % 2 == 0 ? std::numeric_limits<float>::quiet_NaN()
                          : -std::numeric_limits<float>::infinity();
    cleaned[n] = 0.0f;
  }
  CHECK(count_mismatches(render_mono(config, prepared.get(), input),
                         render_mono(config, prepared.get(), cleaned), "non-finite input") == 0);

  Signal extreme(20000, 0.0f);
  for (std::size_t n = 100; n < 400; ++n) {
    extreme[n] = n % 2 == 0 ? 3.0e38f : -3.0e38f;
  }
  const Signal output = render_mono(config, prepared.get(), extreme);
  const std::size_t settled =
      400 + prepared->layout().capacity_frames() + 2 * prepared->layout().largest_partition();
  for (std::size_t n = settled; n < output.size(); ++n) {
    CHECK(output[n] == 0.0f);
  }
}

void test_subnormal_input() {
  const std::size_t length = 2000;
  const ConvolverConfig config = config_for(1, length);
  const Signal response = decaying_response(length, 91u);
  const auto prepared = prepare(config, {&response});
  Signal input(30000, 0.0f);
  for (std::size_t n = 0; n < 5000; ++n) {
    input[n] = (n % 3 == 0 ? 1.0f : -1.0f) * 1.0e-41f;
  }
  const std::size_t settled =
      5000 + prepared->layout().capacity_frames() + 2 * prepared->layout().largest_partition();
  const double bound = absolute_sum(response) * 1.0e-41 * 1.01 + 1e-44;
  const auto check = [&](const Signal &output) {
    for (std::size_t n = 0; n < output.size(); ++n) {
      CHECK(std::isfinite(output[n]) && std::abs(output[n]) <= bound);
      if (n >= settled) {
        CHECK(output[n] == 0.0f);
      }
    }
  };
  check(render_mono(config, prepared.get(), input));
  vespera::dsp::DenormalGuard guard;
  check(render_mono(config, prepared.get(), input));
}

struct SwitchFixture {
  ConvolverConfig config = config_for(2, 3000, 0.01);
  Signal a_response = decaying_response(3000, 101u);
  Signal b_response = decaying_response(2200, 102u);
  Signal c_response = decaying_response(900, 103u);
  std::unique_ptr<PreparedResponse> a = prepare(config, {&a_response});
  std::unique_ptr<PreparedResponse> b = prepare(config, {&b_response});
  std::unique_ptr<PreparedResponse> c = prepare(config, {&c_response});
  std::vector<Signal> input = {random_signal(40000, 104u), random_signal(40000, 105u)};

  std::vector<Signal> steady(const PreparedResponse *response) const {
    Convolver convolver(config);
    convolver.set_response(response);
    return render(convolver, input, {64});
  }

  std::uint64_t fade_start(std::uint64_t request, const Convolver &convolver) const {
    const std::uint64_t largest = convolver.layout().largest_partition();
    return ((request + largest - 1) / largest + 1) * largest;
  }
};

void check_fade(const std::vector<Signal> &output, const std::vector<Signal> &from,
                const std::vector<Signal> &to, std::uint64_t start, std::uint64_t frames,
                std::uint64_t first, std::uint64_t last, const char *label) {
  std::size_t mismatches = 0;
  for (std::size_t c = 0; c < output.size(); ++c) {
    for (std::uint64_t n = first; n < last; ++n) {
      float expected = 0.0f;
      if (n < start) {
        expected = from[c][n];
      } else if (n >= start + frames) {
        expected = to[c][n];
      } else {
        expected = vespera::dsp::crossfade(
            from[c][n], to[c][n], static_cast<double>(n - start + 1) / static_cast<double>(frames));
      }
      if (!same_bits(output[c][n], expected)) {
        if (mismatches == 0) {
          std::fprintf(stderr, "%s: %s: channel %zu frame %llu: %.9g vs %.9g\n", current_test,
                       label, c, static_cast<unsigned long long>(n), output[c][n], expected);
        }
        ++mismatches;
      }
    }
  }
  CHECK(mismatches == 0);
}

void test_crossfade() {
  SwitchFixture f;
  const auto from = f.steady(f.a.get());
  const auto to = f.steady(f.b.get());
  for (std::size_t request : {std::size_t{0}, std::size_t{1000}, std::size_t{4096},
                              std::size_t{4097}, std::size_t{12345}}) {
    for (const std::vector<std::size_t> &blocks :
         {std::vector<std::size_t>{1}, {64}, {333}, {4096}}) {
      Convolver convolver(f.config);
      convolver.set_response(f.a.get());
      std::uint64_t start = 0;
      bool requested = false;
      const PreparedResponse *retired = nullptr;
      std::size_t retired_count = 0;
      const auto output = render(convolver, f.input, blocks, [&](std::size_t position) {
        if (!requested && position >= request) {
          requested = true;
          CHECK(convolver.crossfade_to(f.b.get()));
          CHECK(convolver.switch_state() == ConvolverSwitch::priming);
          start = f.fade_start(position, convolver);
        }
        while (const PreparedResponse *old = convolver.take_retired()) {
          retired = old;
          ++retired_count;
          CHECK(position >= start + convolver.fade_frames());
        }
      });
      CHECK(convolver.fade_frames() == 480);
      check_fade(output, from, to, start, convolver.fade_frames(), 0, f.input[0].size(), "a to b");
      while (const PreparedResponse *old = convolver.take_retired()) {
        retired = old;
        ++retired_count;
      }
      CHECK(retired == f.a.get() && retired_count == 1);
      CHECK(convolver.switch_state() == ConvolverSwitch::steady);
      CHECK(convolver.response() == f.b.get());
    }
  }
}

void test_retired_response_is_fully_released() {
  SwitchFixture f;
  const auto to = f.steady(f.b.get());
  Convolver convolver(f.config);
  convolver.set_response(f.a.get());
  bool released = false;
  const auto output = render(convolver, f.input, {128}, [&](std::size_t position) {
    if (position == 1024) {
      CHECK(convolver.crossfade_to(f.b.get()));
    }
    if (const PreparedResponse *old = convolver.take_retired()) {
      CHECK(old == f.a.get());
      f.a.reset();
      released = true;
    }
  });
  CHECK(released);
  for (std::size_t c = 0; c < 2; ++c) {
    CHECK(count_mismatches(Signal(output[c].end() - 10000, output[c].end()),
                           Signal(to[c].end() - 10000, to[c].end()), "after release") == 0);
  }
}

void test_switch_during_fade_is_queued() {
  SwitchFixture f;
  const auto ya = f.steady(f.a.get());
  const auto yb = f.steady(f.b.get());
  const auto yc = f.steady(f.c.get());
  Convolver convolver(f.config);
  convolver.set_response(f.a.get());
  const std::uint64_t first_start = f.fade_start(1000, convolver);
  const std::uint64_t first_end = first_start + convolver.fade_frames();
  const std::uint64_t second_start = f.fade_start(first_end, convolver);
  std::vector<const PreparedResponse *> retired;
  bool queued = false;
  const auto output = render(convolver, f.input, {100}, [&](std::size_t position) {
    if (position == 1000) {
      CHECK(convolver.crossfade_to(f.b.get()));
    }
    if (!queued && position >= first_start + 100) {
      queued = true;
      CHECK(convolver.switch_state() == ConvolverSwitch::fading);
      CHECK(convolver.crossfade_to(f.c.get()));
      CHECK(convolver.switch_state() == ConvolverSwitch::fading);
    }
    while (const PreparedResponse *old = convolver.take_retired()) {
      retired.push_back(old);
    }
  });
  check_fade(output, ya, yb, first_start, convolver.fade_frames(), 0, first_end, "first fade");
  check_fade(output, yb, yc, second_start, convolver.fade_frames(), first_end, f.input[0].size(),
             "queued fade");
  CHECK(retired.size() == 2 && retired[0] == f.a.get() && retired[1] == f.b.get());
}

void test_queue_replacement_and_reversal() {
  SwitchFixture f;
  Signal d_response = decaying_response(400, 106u);
  const auto d = prepare(f.config, {&d_response});
  Convolver convolver(f.config);
  convolver.set_response(f.a.get());
  CHECK(convolver.crossfade_to(f.b.get()));
  CHECK(convolver.crossfade_to(f.b.get()));
  CHECK(convolver.take_retired() == nullptr);
  CHECK(convolver.crossfade_to(f.c.get()));
  CHECK(convolver.crossfade_to(d.get()));
  CHECK(convolver.take_retired() == f.c.get());
  CHECK(convolver.crossfade_to(f.b.get()));
  CHECK(convolver.take_retired() == d.get());
  CHECK(convolver.crossfade_to(f.a.get()));
  CHECK(convolver.take_retired() == nullptr);
  (void)render(convolver, f.input, {256});
  CHECK(convolver.response() == f.a.get());
  CHECK(convolver.take_retired() == f.b.get());
  CHECK(convolver.take_retired() == nullptr);

  const auto ya = f.steady(f.a.get());
  Convolver back(f.config);
  back.set_response(f.a.get());
  const auto output = render(back, f.input, {64}, [&](std::size_t position) {
    if (position == 64) {
      CHECK(back.crossfade_to(f.b.get()));
      CHECK(back.crossfade_to(f.a.get()));
    }
  });
  CHECK(count_mismatches(Signal(output[0].end() - 5000, output[0].end()),
                         Signal(ya[0].end() - 5000, ya[0].end()), "switched back") == 0);
}

void test_fade_to_and_from_silence_and_reset() {
  SwitchFixture f;
  const auto yb = f.steady(f.b.get());
  const std::vector<Signal> silence(2, Signal(f.input[0].size(), 0.0f));
  Convolver convolver(f.config);
  const auto rising = render(convolver, f.input, {64}, [&](std::size_t position) {
    if (position == 512) {
      CHECK(convolver.crossfade_to(f.b.get()));
    }
  });
  check_fade(rising, silence, yb, f.fade_start(512, convolver), convolver.fade_frames(), 0,
             f.input[0].size(), "from silence");

  Convolver falling(f.config);
  falling.set_response(f.b.get());
  const auto out = render(falling, f.input, {64}, [&](std::size_t position) {
    if (position == 2048) {
      CHECK(falling.crossfade_to(nullptr));
    }
  });
  check_fade(out, yb, silence, f.fade_start(2048, falling), falling.fade_frames(), 0,
             f.input[0].size(), "to silence");
  CHECK(falling.take_retired() == f.b.get());
  CHECK(falling.response() == nullptr);

  Convolver resetting(f.config);
  resetting.set_response(f.a.get());
  CHECK(resetting.crossfade_to(f.b.get()));
  CHECK(resetting.crossfade_to(f.c.get()));
  resetting.reset();
  CHECK(resetting.switch_state() == ConvolverSwitch::steady);
  CHECK(resetting.response() == f.c.get());
  CHECK(resetting.take_retired() == f.a.get());
  CHECK(resetting.take_retired() == f.b.get());
  CHECK(resetting.take_retired() == nullptr);
}

void test_zero_fade_and_direct_layout() {
  ConvolverConfig config = config_for(1, 20, 0.0);
  const Signal a_response = decaying_response(20, 111u);
  const Signal b_response = decaying_response(20, 112u);
  const auto a = prepare(config, {&a_response});
  const auto b = prepare(config, {&b_response});
  const Signal input = random_signal(3000, 113u);
  const Signal ya = render_mono(config, a.get(), input);
  const Signal yb = render_mono(config, b.get(), input);
  Convolver convolver(config);
  CHECK(convolver.layout().level_count == 0);
  convolver.set_response(a.get());
  const auto output = render(convolver, {input}, {50}, [&](std::size_t position) {
    if (position == 1000) {
      CHECK(convolver.crossfade_to(b.get()));
      CHECK(convolver.switch_state() == ConvolverSwitch::steady);
      CHECK(convolver.response() == b.get());
    }
  });
  check_fade(output, {ya}, {yb}, 1000, 0, 0, input.size(), "hard switch");
}

void test_interrupted_switch_back_retirement() {
  const auto config = config_for(1, 64);
  const Signal impulse{1};
  for (bool replace : {false, true}) {
    for (std::size_t backlog : {std::size_t{0}, Convolver::retired_capacity - 3}) {
      std::vector<std::unique_ptr<PreparedResponse>> responses;
      for (std::size_t i = 0; i < backlog + 3; ++i) {
        responses.push_back(prepare(config, {&impulse}));
      }
      Convolver convolver(config);
      for (std::size_t i = 0; i <= backlog; ++i) {
        convolver.set_response(responses[i].get());
      }
      const auto *a = responses[backlog].get();
      const auto *b = responses[backlog + 1].get();
      const auto *c = responses[backlog + 2].get();
      CHECK(convolver.crossfade_to(b));
      (void)render(convolver, {Signal(64)}, {64});
      CHECK(convolver.switch_state() != ConvolverSwitch::steady);
      CHECK(convolver.crossfade_to(a));
      CHECK(convolver.crossfade_to(a));
      convolver.set_response(replace ? c : nullptr);
      std::vector<const PreparedResponse *> retired;
      while (const auto *old = convolver.take_retired()) {
        retired.push_back(old);
      }
      CHECK(retired.size() == backlog + 2);
      for (std::size_t i = 0; i < backlog + 2; ++i) {
        CHECK(std::count(retired.begin(), retired.end(), responses[i].get()) == 1);
      }
      CHECK(convolver.response() == (replace ? c : nullptr));
      convolver.set_response(nullptr);
      CHECK(convolver.take_retired() == (replace ? c : nullptr));
      CHECK(convolver.take_retired() == nullptr);
    }
  }
}

void test_oversized_layout_counts() {
  const Signal impulse{1};
  for (std::size_t count : {ConvolutionLayout::max_levels + 1, std::size_t{1024},
                            std::numeric_limits<std::size_t>::max()}) {
    auto config = config_for(1, 64);
    config.layout = ConvolutionLayout::plan(64);
    config.layout->level_count = count;
    bool rejected = false;
    try {
      Convolver bad(config);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    CHECK(rejected);
    rejected = false;
    try {
      (void)prepare(config, {&impulse});
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(config.layout->level_count == count);
  }
}

// Mutations of a valid layout's partition and capacity arithmetic: no validation path may index,
// multiply or sum the supplied counts before the layout has been checked against the one the
// factories would build for this capacity.
void test_malformed_layout_arithmetic() {
  const Signal impulse{1};
  const auto rejects_layout = [&impulse](auto mutate) {
    ConvolverConfig config = config_for(1, 2000);
    config.layout = ConvolutionLayout::doubling(2000, 64, 256);
    mutate(*config.layout);
    const ConvolutionLayout mutated = *config.layout;
    CHECK(throws_invalid([&] { Convolver convolver(config); }));
    CHECK(throws_invalid([&] { (void)prepare(config, {&impulse}); }));
    CHECK(*config.layout == mutated);
  };
  rejects_layout([](ConvolutionLayout &l) { l.levels[0].partition_frames = 0; });
  rejects_layout([](ConvolutionLayout &l) { l.levels[0].partition_frames = 48; });
  rejects_layout([](ConvolutionLayout &l) { l.levels[0].partition_frames = 8; });
  rejects_layout([](ConvolutionLayout &l) { l.levels[0].partition_frames = 512; });
  rejects_layout([](ConvolutionLayout &l) { l.levels[0].partition_frames = 16384; });
  rejects_layout([](ConvolutionLayout &l) { l.levels[0].partitions = 0; });
  rejects_layout([](ConvolutionLayout &l) { l.levels[0].partitions = 1; });
  rejects_layout([](ConvolutionLayout &l) {
    l.levels[2].partitions = std::numeric_limits<std::size_t>::max();
  });
  rejects_layout([](ConvolutionLayout &l) { l.levels[2].partitions *= 2; });
  rejects_layout([](ConvolutionLayout &l) { l.head_frames = 0; });
  rejects_layout([](ConvolutionLayout &l) { l.head_frames = 127; });
  rejects_layout(
      [](ConvolutionLayout &l) { l.head_frames = std::numeric_limits<std::size_t>::max(); });
  rejects_layout([](ConvolutionLayout &l) { l.level_count = 1; });
  rejects_layout([](ConvolutionLayout &l) { l.level_count = ConvolutionLayout::max_levels; });
  ConvolverConfig direct = config_for(1, 2000);
  direct.layout = ConvolutionLayout::direct(2000);
  for (std::size_t head : {std::size_t{0}, std::size_t{125}, std::size_t{4096},
                           std::numeric_limits<std::size_t>::max()}) {
    ConvolverConfig mutated = direct;
    mutated.layout->head_frames = head;
    CHECK(throws_invalid([&] { Convolver convolver(mutated); }));
    CHECK(throws_invalid([&] { (void)prepare(mutated, {&impulse}); }));
  }
  CHECK(throws_invalid(
      [] { (void)ConvolutionLayout::direct(vespera::dsp::maximum_response_frames + 1); }));
  CHECK(throws_invalid([] { (void)ConvolutionLayout::doubling(100, 16, 16384); }));
  CHECK(throws_invalid([] { (void)ConvolutionLayout::doubling(100, 12, 64); }));
  CHECK(throws_invalid([] { (void)ConvolutionLayout::doubling(100, 4, 64); }));
  CHECK(throws_invalid(
      [] { (void)ConvolutionLayout::doubling(100, std::numeric_limits<std::size_t>::max(), 64); }));
  CHECK(throws_invalid(
      [] { (void)ConvolutionLayout::doubling(std::numeric_limits<std::size_t>::max(), 16, 64); }));
  CHECK(throws_invalid([] { (void)ConvolutionLayout::plan(0); }));

  // A rejected layout leaves an existing convolver untouched: construct the valid twin after.
  ConvolverConfig mutated = config_for(1, 2000);
  mutated.layout = ConvolutionLayout::doubling(2000, 64, 256);
  mutated.layout->levels[2].partitions = 3;
  CHECK(throws_invalid([&] { Convolver convolver(mutated); }));
  Convolver valid(config_for(1, 2000));
  valid.set_response(nullptr);
  CHECK(valid.response() == nullptr && valid.switch_state() == ConvolverSwitch::steady);
}

// Refused requests leave the active response, switch state and pending retirements exactly as
// they were, for both refusal reasons: an incompatible response and a full retirement ring.
void test_failed_requests_preserve_state() {
  SwitchFixture f;
  ConvolverConfig other = f.config;
  other.max_response_frames = 4000;
  const Signal other_response(1000, 0.2f);
  const auto incompatible = prepare(other, {&other_response});
  CHECK(!f.config.layout.has_value());

  Convolver convolver(f.config);
  convolver.set_response(f.a.get());
  CHECK(convolver.crossfade_to(f.b.get()));
  const ConvolverSwitch mid_switch = convolver.switch_state();
  CHECK(!convolver.accepts(incompatible.get()));
  CHECK(!convolver.crossfade_to(incompatible.get()));
  CHECK(convolver.switch_state() == mid_switch && convolver.response() == f.a.get());
  CHECK(throws_invalid([&] { convolver.set_response(incompatible.get()); }));
  CHECK(convolver.switch_state() == mid_switch && convolver.response() == f.a.get());

  std::vector<std::unique_ptr<PreparedResponse>> extra;
  for (std::size_t i = 0; i < Convolver::retired_capacity + 2; ++i) {
    extra.push_back(prepare(f.config, {&f.c_response}));
  }
  Convolver loaded(f.config);
  loaded.set_response(extra[0].get());
  std::size_t accepted = 1;
  while (accepted < extra.size() && loaded.crossfade_to(extra[accepted].get())) {
    ++accepted;
  }
  CHECK(accepted == Convolver::retired_capacity);
  CHECK(!loaded.crossfade_to(extra[accepted].get()));
  bool length_error = false;
  try {
    loaded.set_response(extra[accepted].get());
  } catch (const std::length_error &) {
    length_error = true;
  }
  CHECK(length_error);
  CHECK(loaded.response() == extra[0].get());
  CHECK(loaded.switch_state() == ConvolverSwitch::priming);
  for (std::size_t i = 2; i + 1 < accepted; ++i) {
    CHECK(loaded.take_retired() == extra[i].get());
  }
  CHECK(loaded.take_retired() == nullptr);

  CHECK(!convolver.crossfade_to(incompatible.get()));
  CHECK(throws_invalid([&] { convolver.set_response(incompatible.get()); }));
  CHECK(convolver.switch_state() == mid_switch && convolver.response() == f.a.get());
}

// Numerical boundary rates of the documented 8-768 kHz range, checked against double direct
// convolution rather than only the default-rate layouts.
void test_boundary_sample_rates() {
  for (double rate : {8000.0, 192000.0, 768000.0}) {
    ConvolverConfig config;
    config.sample_rate = rate;
    config.channels = 1;
    config.max_response_frames = 1500;
    config.fade_seconds = 0.002;
    const Signal response = decaying_response(1500, 141u);
    const Signal input = random_signal(6000, 142u);
    const auto prepared = prepare(config, {&response});
    Convolver convolver(config);
    convolver.set_response(prepared.get());
    const Signal output = render(convolver, {input}, {37})[0];
    const double limit = tolerance(prepared->layout(), response, input);
    double worst = 0.0;
    for (std::size_t n = 0; n < input.size(); ++n) {
      worst = std::max(
          worst, std::abs(static_cast<double>(output[n]) - reference_sample(response, input, n)));
    }
    if (!(worst <= limit)) {
      std::fprintf(stderr, "%s: rate %.0f: error %.3g exceeds %.3g\n", current_test, rate, worst,
                   limit);
      ++failures;
    }
  }
}

void test_retired_capacity() {
  SwitchFixture f;
  std::vector<std::unique_ptr<PreparedResponse>> responses;
  for (std::uint32_t i = 0; i < 12; ++i) {
    responses.push_back(prepare(f.config, {&f.c_response}));
  }
  Convolver convolver(f.config);
  convolver.set_response(responses[0].get());
  CHECK(convolver.crossfade_to(responses[1].get()));
  std::size_t accepted = 2;
  while (accepted < responses.size() && convolver.crossfade_to(responses[accepted].get())) {
    ++accepted;
  }
  CHECK(accepted == Convolver::retired_capacity);
  CHECK(!convolver.crossfade_to(responses[accepted].get()));
  bool length_error = false;
  try {
    convolver.set_response(responses[accepted].get());
  } catch (const std::length_error &) {
    length_error = true;
  }
  CHECK(length_error);
  std::size_t taken = 0;
  while (convolver.take_retired() != nullptr) {
    ++taken;
  }
  CHECK(taken == Convolver::retired_capacity - 3);
  CHECK(convolver.crossfade_to(responses[accepted].get()));
}

void test_exchange_single_thread() {
  SwitchFixture f;
  ResponseExchange exchange;
  std::unique_ptr<PreparedResponse> first = prepare(f.config, {&f.a_response});
  const PreparedResponse *first_raw = first.get();
  CHECK(exchange.submit(first));
  CHECK(!first);
  std::unique_ptr<PreparedResponse> second = prepare(f.config, {&f.b_response});
  const PreparedResponse *second_raw = second.get();
  CHECK(exchange.submit(second));
  CHECK(exchange.take() == second_raw);
  CHECK(exchange.take() == nullptr);
  (void)first_raw;
  std::unique_ptr<PreparedResponse> empty;
  CHECK(!exchange.submit(empty));

  Convolver convolver(f.config);
  convolver.set_response(second_raw);
  for (std::size_t i = 0; i < 20; ++i) {
    std::unique_ptr<PreparedResponse> next = prepare(f.config, {&f.c_response});
    CHECK(exchange.submit(next));
    const PreparedResponse *taken = exchange.take();
    if (!convolver.crossfade_to(taken)) {
      exchange.retire(taken);
    }
    (void)render(convolver, f.input, {4096});
    while (const PreparedResponse *old = convolver.take_retired()) {
      exchange.retire(old);
    }
  }
  CHECK(exchange.collect() == 1);
  std::size_t owned = 0;
  std::vector<std::unique_ptr<PreparedResponse>> extra;
  for (std::size_t i = 0; i < ResponseExchange::capacity + 1; ++i) {
    extra.push_back(prepare(f.config, {&f.c_response}));
    if (exchange.submit(extra.back())) {
      ++owned;
      (void)exchange.take();
    }
  }
  CHECK(owned == ResponseExchange::capacity - 1);
  CHECK(extra.back() != nullptr);
}

void test_exchange_two_threads() {
  const ConvolverConfig config = config_for(2, 2000, 0.001);
  const Signal response = decaying_response(2000, 121u);
  ResponseExchange exchange;
  std::atomic<bool> running{true};
  std::thread audio([&] {
    Convolver convolver(config);
    const std::vector<Signal> input = {random_signal(256, 122u), random_signal(256, 123u)};
    std::vector<Signal> output(2, Signal(256));
    while (running.load(std::memory_order_acquire)) {
      if (const PreparedResponse *next = exchange.take()) {
        if (!convolver.crossfade_to(next)) {
          exchange.retire(next);
        }
      }
      std::array<const float *, 2> in{input[0].data(), input[1].data()};
      std::array<float *, 2> out{output[0].data(), output[1].data()};
      convolver.process(in, out, 256);
      while (const PreparedResponse *old = convolver.take_retired()) {
        exchange.retire(old);
      }
    }
    if (!convolver.crossfade_to(nullptr)) {
      std::abort();
    }
    convolver.reset();
    while (const PreparedResponse *old = convolver.take_retired()) {
      exchange.retire(old);
    }
    exchange.retire(exchange.take());
  });
  std::size_t submitted = 0;
  for (std::size_t i = 0; i < 300; ++i) {
    std::unique_ptr<PreparedResponse> next = prepare(config, {&response});
    if (exchange.submit(next)) {
      ++submitted;
    }
  }
  running.store(false, std::memory_order_release);
  audio.join();
  exchange.collect();
  CHECK(submitted > 0);
}

}

int main() {
  run("layout plan", test_layout_plan);
  run("rejects invalid configuration", test_rejects_invalid_configuration);
  run("rejects invalid responses", test_rejects_invalid_responses);
  run("matches direct convolution", test_matches_direct_convolution);
  run("partition boundaries", test_partition_boundaries);
  run("long response", test_long_response);
  run("impulse and latency", test_impulse_and_latency);
  run("linearity", test_linearity);
  run("mono and stereo", test_mono_and_stereo);
  run("block-size independence", test_block_size_independence);
  run("in place", test_in_place);
  run("reset", test_reset);
  run("non-finite and extreme input", test_non_finite_and_extreme_input);
  run("subnormal input", test_subnormal_input);
  run("crossfade", test_crossfade);
  run("retired response fully released", test_retired_response_is_fully_released);
  run("switch during fade is queued", test_switch_during_fade_is_queued);
  run("queue replacement and reversal", test_queue_replacement_and_reversal);
  run("fade to and from silence and reset", test_fade_to_and_from_silence_and_reset);
  run("zero fade and direct layout", test_zero_fade_and_direct_layout);
  run("retired capacity", test_retired_capacity);
  run("interrupted switch-back retirement", test_interrupted_switch_back_retirement);
  run("oversized layout counts", test_oversized_layout_counts);
  run("malformed layout arithmetic", test_malformed_layout_arithmetic);
  run("failed requests preserve state", test_failed_requests_preserve_state);
  run("boundary sample rates", test_boundary_sample_rates);
  run("exchange single thread", test_exchange_single_thread);
  run("exchange two threads", test_exchange_two_threads);
  return finish("convolution");
}
