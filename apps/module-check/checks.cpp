#include "checks.hpp"

#include "allocation.hpp"
#include <vespera/file_content.hpp>
#include <vespera/parameter_display.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace vespera::module_check {
namespace {

constexpr double check_rate = 48000.0;
constexpr uint32_t ordinary_block = 512;

std::string layout_name(uint32_t channels) { return channels == 1 ? "mono" : "stereo"; }

std::string rate_name(double rate) { return number(rate / 1000.0) + " kHz"; }

uint32_t frames_for(double milliseconds, double rate) {
  return static_cast<uint32_t>(std::ceil(milliseconds * rate / 1000.0));
}

Audio slice(const Audio &audio, size_t start, size_t count) {
  Audio result;
  for (const auto &channel : audio) {
    result.emplace_back(channel.begin() + static_cast<std::ptrdiff_t>(start),
                        channel.begin() + static_cast<std::ptrdiff_t>(start + count));
  }
  return result;
}

void append(Audio &to, const Audio &from) {
  to.resize(from.size());
  for (size_t c = 0; c < from.size(); ++c) {
    to[c].insert(to[c].end(), from[c].begin(), from[c].end());
  }
}

Audio delayed(const Audio &audio, uint32_t latency) {
  Audio result = audio;
  for (size_t c = 0; c < audio.size(); ++c) {
    for (size_t i = 0; i < audio[c].size(); ++i) {
      result[c][i] = i >= latency ? audio[c][i - latency] : 0.0f;
    }
  }
  return result;
}

Settings with_value(Settings settings, uint32_t id, float value) {
  for (auto &setting : settings) {
    if (setting.first == id) {
      setting.second = value;
    }
  }
  return settings;
}

// Descriptor defaults, with the bypass parameter, if any, set to process.
Settings processing_defaults(const Binary &binary) {
  Settings settings;
  for (const Descriptor &d : binary.descriptors) {
    const bool route = binary.serial_route && d.id == binary.serial_route->parameter;
    settings.emplace_back(d.id, binary.bypass && d.id == *binary.bypass ? 1.0f
                                : route                                 ? binary.serial_route->value
                                                                        : d.default_value);
  }
  return settings;
}

std::vector<Automation> make_automation(const Binary &binary, Random &random, size_t frames,
                                        uint32_t count) {
  std::vector<Automation> automation;
  for (uint32_t i = 0; i < count && !binary.descriptors.empty(); ++i) {
    const Descriptor &d =
        binary.descriptors[random.below(static_cast<uint32_t>(binary.descriptors.size()))];
    if (binary.serial_route && d.id == binary.serial_route->parameter) {
      continue;
    }
    float value = 0.0f;
    if (binary.bypass && d.id == *binary.bypass) {
      value = random.below(2) ? 1.0f : 0.0f;
    } else {
      const double position = random.uniform();
      value = std::clamp(
          static_cast<float>(d.minimum + position * (static_cast<double>(d.maximum) - d.minimum)),
          d.minimum, d.maximum);
    }
    const bool same_frame = !automation.empty() && random.below(8) == 0;
    const uint64_t frame =
        same_frame ? automation.back().frame : random.below(static_cast<uint32_t>(frames));
    automation.push_back({frame, d.id, value});
  }
  std::stable_sort(automation.begin(), automation.end(),
                   [](const Automation &a, const Automation &b) { return a.frame < b.frame; });
  return automation;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool same_bits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

// Generic modules may retain the request or canonicalize it. Branch mixes have a stricter rule.
bool matches_request(const Binary &binary, uint32_t id, float stored, float requested) {
  const ParameterDisplay *display = binary.display(id);
  return same_bits(stored, requested) ||
         (display && same_bits(stored, display->nearest(requested)));
}

Settings stored_settings(const Binary &binary, const Instance &instance, const Settings &requests) {
  Settings stored;
  for (const auto &[id, requested] : requests) {
    const auto value = instance.value(id);
    if (!value || !matches_request(binary, id, *value, requested)) {
      throw CheckError("get_value(" + std::to_string(id) + ") reports " +
                       (value ? number(*value) : std::string("an error")) + " after setting " +
                       number(requested));
    }
    stored.emplace_back(id, *value);
  }
  return stored;
}

// Owns input and output storage for hand-built blocks, sized for up to three channels so that
// blocks claiming more channels or frames than prepared stay within valid memory.
struct RawBlock {
  std::vector<std::vector<float>> inputs;
  std::vector<std::vector<float>> outputs;
  std::vector<const float *> input_pointers;
  std::vector<float *> output_pointers;
  vs_process_block block{};

  RawBlock(uint32_t channels, uint32_t frames, uint32_t capacity)
      : inputs(3, std::vector<float>(capacity, 0.0f)),
        outputs(3, std::vector<float>(capacity, 0.0f)) {
    const Audio signal = test_signal(3, capacity, check_rate, 7);
    for (size_t c = 0; c < 3; ++c) {
      inputs[c] = signal[c];
      input_pointers.push_back(inputs[c].data());
      output_pointers.push_back(outputs[c].data());
    }
    block.struct_size = sizeof(vs_process_block);
    block.inputs = input_pointers.data();
    block.outputs = output_pointers.data();
    block.channels = channels;
    block.frames = frames;
  }
  RawBlock(const RawBlock &) = delete;
  RawBlock &operator=(const RawBlock &) = delete;
};

std::vector<uint8_t> save_state(const Binary &binary, void *handle) {
  uint64_t bytes = 0;
  vs_result result = binary.state->size(handle, &bytes);
  if (result != VS_OK) {
    throw CheckError("state size returned " + result_name(result));
  }
  if (bytes > (64u << 20)) {
    throw CheckError("state size reports " + std::to_string(bytes) + " bytes");
  }
  std::vector<uint8_t> state(static_cast<size_t>(bytes));
  uint64_t written = 0;
  result = binary.state->save(handle, state.data(), bytes, &written);
  if (result != VS_OK || written != bytes) {
    throw CheckError("state save returned " + result_name(result) + " after writing " +
                     std::to_string(written) + " of " + std::to_string(bytes) + " bytes");
  }
  return state;
}

// Whether state begins with each parameter's value as binary32 in descriptor order, the layout of
// the module kit's schema 1 and of modules sharing it.
bool parameter_table(const Binary &binary, const std::vector<uint8_t> &state,
                     const Settings &settings) {
  if (settings.empty() || state.size() < settings.size() * sizeof(float)) {
    return false;
  }
  for (size_t i = 0; i < settings.size(); ++i) {
    float stored = 0.0f;
    std::memcpy(&stored, state.data() + i * sizeof(float), sizeof(float));
    const auto &[id, value] = settings[i];
    if (!(binary.parameters ? same_bits(stored, value)
                            : matches_request(binary, id, stored, value))) {
      return false;
    }
  }
  return true;
}

std::string list(const std::vector<std::string> &items, const char *separator = ", ") {
  std::string result;
  for (const auto &item : items) {
    result += (result.empty() ? "" : separator) + item;
  }
  return result;
}

}

Result Findings::summarize(const std::string &target, const std::string &check,
                           const std::string &passed) const {
  if (!failures_.empty()) {
    const std::vector<std::string> shown(
        failures_.begin(), failures_.begin() + std::min<std::ptrdiff_t>(3, std::ssize(failures_)));
    std::string detail = list(shown, "; ");
    if (failures_.size() > shown.size()) {
      detail += " (and " + std::to_string(failures_.size() - shown.size()) + " more)";
    }
    return {target, check, Status::fail, detail};
  }
  if (!warnings_.empty()) {
    return {target, check, Status::warn, passed + "; " + list(warnings_, "; ")};
  }
  return {target, check, Status::pass, passed};
}

void check_lifecycle(const Context &context) {
  const Binary &binary = context.binary;
  const Options &options = context.options;
  const uint32_t max = options.max_frames;
  const uint32_t channels = binary.channel_counts.front();
  Findings findings;
  uint32_t rejected = 0;
  const auto expect_rejected = [&](const std::string &what, vs_result result) {
    if (result == VS_OK) {
      findings.fail(what + " returned VS_OK");
    } else {
      ++rejected;
    }
  };

  findings.attempt("create", [&] {
    expect_rejected("create(NULL)", binary.api->create(nullptr));
    Instance instance(binary);
    for (const Descriptor &d : binary.descriptors) {
      if (!binary.parameters) {
        break;
      }
      const auto value = instance.value(d.id);
      if (!value) {
        findings.fail("get_value(" + std::to_string(d.id) + ") fails after create");
      } else if (!same_bits(*value, d.default_value)) {
        findings.fail("parameter " + std::to_string(d.id) + " starts at " + number(*value) +
                      ", its descriptor default is " + number(d.default_value));
      }
    }
  });

  findings.attempt("repeated prepare and release", [&] {
    Instance instance(binary);
    instance.release();
    const double rates[] = {check_rate, 44100.0, 96000.0};
    for (uint32_t cycle = 0; cycle < 3; ++cycle) {
      for (uint32_t count : binary.channel_counts) {
        const uint32_t frames = cycle == 2 ? 1 : max;
        instance.prepare(rates[cycle], count, frames);
        instance.render(test_signal(count, 256, rates[cycle], options.seed),
                        fixed_blocks(256, std::min<uint32_t>(frames, 64)));
        instance.release();
      }
    }
    instance.release();
  });

  findings.attempt("invalid prepare", [&] {
    Instance instance(binary);
    struct Case {
      std::string name;
      vs_prepare_info info;
      bool null_info = false;
      bool null_timing = false;
      uint32_t timing_size = sizeof(vs_module_timing);
    };
    const vs_prepare_info valid{sizeof(vs_prepare_info), check_rate, max, channels};
    const auto with = [&](auto edit) {
      vs_prepare_info info = valid;
      edit(info);
      return info;
    };
    std::vector<Case> cases{
        {"a null vs_prepare_info", valid, true},
        {"a null vs_module_timing", valid, false, true},
        {"a short vs_prepare_info.struct_size",
         with([](vs_prepare_info &i) { i.struct_size = sizeof(vs_prepare_info) - 1; })},
        {"a short vs_module_timing.struct_size", valid, false, false, sizeof(vs_module_timing) - 1},
        {"0 channels", with([](vs_prepare_info &i) { i.channels = 0; })},
        {"3 channels", with([](vs_prepare_info &i) { i.channels = 3; })},
        {"max_frames 0", with([](vs_prepare_info &i) { i.max_frames = 0; })},
        {"max_frames 65537", with([](vs_prepare_info &i) { i.max_frames = 65537; })},
        {"a NaN sample rate", with([](vs_prepare_info &i) {
           i.sample_rate = std::numeric_limits<double>::quiet_NaN();
         })},
        {"an infinite sample rate",
         with([](vs_prepare_info &i) { i.sample_rate = std::numeric_limits<double>::infinity(); })},
        {"sample rate 0", with([](vs_prepare_info &i) { i.sample_rate = 0.0; })},
        {"a negative sample rate", with([](vs_prepare_info &i) { i.sample_rate = -check_rate; })},
    };
    if (binary.channel_counts.size() == 1) {
      const uint32_t other = channels == 1 ? 2 : 1;
      cases.push_back({"an undeclared " + layout_name(other) + " layout",
                       with([&](vs_prepare_info &i) { i.channels = other; })});
    }
    for (const Case &c : cases) {
      vs_module_timing timing{c.timing_size, 0, 0};
      const vs_result result =
          instance.try_prepare(c.null_info ? nullptr : &c.info, c.null_timing ? nullptr : &timing);
      expect_rejected("prepare with " + c.name, result);
      instance.release();
    }
    instance.prepare(check_rate, channels, max);
    instance.render(test_signal(channels, 256, check_rate, options.seed), fixed_blocks(256, 64));
  });

  findings.attempt("prepare while prepared", [&] {
    Instance instance(binary);
    instance.prepare(check_rate, channels, max);
    const vs_prepare_info info{sizeof(vs_prepare_info), check_rate, max, channels};
    vs_module_timing timing{sizeof(vs_module_timing), 0, 0};
    expect_rejected("a second prepare without release",
                    binary.api->prepare(instance.handle(), &info, &timing));
    instance.render(test_signal(channels, 256, check_rate, options.seed), fixed_blocks(256, 64));
  });

  findings.attempt("process when not prepared", [&] {
    Instance instance(binary);
    RawBlock raw(channels, 64, max + 1);
    expect_rejected("process before prepare", instance.process(raw.block));
    instance.prepare(check_rate, channels, max);
    instance.release();
    expect_rejected("process after release", instance.process(raw.block));
  });

  findings.attempt("invalid process", [&] {
    Instance instance(binary);
    instance.prepare(check_rate, channels, max);
    expect_rejected("process with a null block", binary.api->process(instance.handle(), nullptr));
    const auto attempt_block = [&](const std::string &what, auto edit) {
      RawBlock raw(channels, 64, max + 1);
      edit(raw);
      expect_rejected("process with " + what, instance.process(raw.block));
    };
    attempt_block("a short struct_size", [](RawBlock &r) {
      r.block.struct_size = static_cast<uint32_t>(VS_PROCESS_BLOCK_V1_SIZE - 1);
    });
    attempt_block(std::to_string(channels + 1) + " channels after preparing " +
                      std::to_string(channels),
                  [&](RawBlock &r) { r.block.channels = channels + 1; });
    attempt_block("0 channels", [](RawBlock &r) { r.block.channels = 0; });
    attempt_block("0 frames", [](RawBlock &r) { r.block.frames = 0; });
    attempt_block(std::to_string(max + 1) + " frames after preparing " + std::to_string(max),
                  [&](RawBlock &r) { r.block.frames = max + 1; });
    attempt_block("null inputs", [](RawBlock &r) { r.block.inputs = nullptr; });
    attempt_block("null outputs", [](RawBlock &r) { r.block.outputs = nullptr; });
    attempt_block("a null input channel",
                  [&](RawBlock &r) { r.input_pointers[channels - 1] = nullptr; });
    attempt_block("a null output channel",
                  [&](RawBlock &r) { r.output_pointers[channels - 1] = nullptr; });
    attempt_block("null events and event_count 1", [](RawBlock &r) { r.block.event_count = 1; });
    instance.render(test_signal(channels, 256, check_rate, options.seed), fixed_blocks(256, 64));
  });

  findings.attempt("reset", [&] {
    Random random(options.seed + 1);
    const Settings settings = binary.parameters ? random_settings(binary, random) : Settings{};
    const Audio first = test_signal(channels, 4096, check_rate, options.seed + 1);
    const Audio second = test_signal(channels, 4096, check_rate, options.seed + 2);
    Instance used(binary);
    used.configure(settings);
    used.prepare(check_rate, channels, max);
    used.render(first, fixed_blocks(4096, 256));
    used.reset();
    const Audio after = used.render(second, fixed_blocks(4096, 256));
    Instance fresh(binary);
    fresh.configure(settings);
    fresh.prepare(check_rate, channels, max);
    const Difference difference = compare(after, fresh.render(second, fixed_blocks(4096, 256)));
    if (!within(difference, options.tolerance)) {
      findings.fail("output after reset() differs from a freshly prepared instance: " +
                    describe(difference));
    }
  });

  if (binary.parameters) {
    findings.attempt("values across release", [&] {
      Random random(options.seed + 3);
      const Settings settings = random_settings(binary, random);
      Instance instance(binary);
      instance.configure(settings);
      const Settings stored = stored_settings(binary, instance, settings);
      instance.prepare(check_rate, channels, max);
      instance.render(test_signal(channels, 256, check_rate, options.seed), fixed_blocks(256, 64));
      instance.release();
      instance.prepare(44100.0, channels, max);
      for (const auto &[id, value] : stored) {
        const auto current = instance.value(id);
        if (!current || !same_bits(*current, value)) {
          findings.warn("get_value(" + std::to_string(id) + ") reports " +
                        (current ? number(*current) : std::string("an error")) +
                        " after a new prepare; previously stored " + number(value));
        }
      }
    });
  }

  findings.attempt("independent instances", [&] {
    Random random(options.seed + 4);
    const Settings a_settings = random_settings(binary, random);
    const Settings b_settings = random_settings(binary, random);
    const Audio a_input = test_signal(channels, 8192, 96000.0, options.seed + 5);
    const Audio b_input = test_signal(channels, 8192, check_rate, options.seed + 6);
    Instance a(binary);
    Instance b(binary);
    a.configure(a_settings);
    b.configure(b_settings);
    a.prepare(96000.0, channels, max);
    b.prepare(check_rate, channels, max);
    Audio interleaved;
    for (size_t start = 0; start < 8192; start += 128) {
      a.render(slice(a_input, start, 128), {128});
      append(interleaved, b.render(slice(b_input, start, 128), {128}));
    }
    Instance alone(binary);
    alone.configure(b_settings);
    alone.prepare(check_rate, channels, max);
    const Difference difference =
        compare(interleaved, alone.render(b_input, fixed_blocks(8192, 128)));
    if (!difference.identical) {
      findings.fail("an instance's output changes when another instance processes in between: " +
                    describe(difference));
    }
  });

  findings.report(context, "lifecycle",
                  "legal call orders work; " + std::to_string(rejected) +
                      " misuse calls rejected; reset clears history; instances are independent");
}

void check_blocks(const Context &context) {
  const Binary &binary = context.binary;
  const Options &options = context.options;
  const uint32_t max = options.max_frames;
  Findings findings;
  double worst = 0.0;
  for (uint32_t channels : binary.channel_counts) {
    findings.attempt(layout_name(channels), [&] {
      Random random(options.seed + 10 + channels);
      const Settings settings = random_settings(binary, random);
      const size_t frames = 2 * size_t{max} + 333;
      const Audio input = test_signal(channels, frames, check_rate, options.seed + 11);
      const auto render = [&](const std::vector<uint32_t> &blocks) {
        Instance instance(binary);
        instance.configure(settings);
        instance.prepare(check_rate, channels, max);
        return instance.render(input, blocks);
      };
      const Audio reference = render(fixed_blocks(frames, 1));
      const std::pair<std::string, std::vector<uint32_t>> partitions[] = {
          {"7", fixed_blocks(frames, std::min<uint32_t>(7, max))},
          {"64", fixed_blocks(frames, std::min<uint32_t>(64, max))},
          {std::to_string(max), fixed_blocks(frames, max)},
          {"random sizes", random_blocks(frames, max, random)}};
      for (const auto &[name, blocks] : partitions) {
        const Difference difference = compare(reference, render(blocks));
        if (!within(difference, options.tolerance)) {
          findings.fail(layout_name(channels) + " blocks of " + name +
                        " differ from single-frame blocks: " + describe(difference));
        } else if (!difference.identical) {
          worst = std::max(worst, difference.maximum);
        }
      }
    });
  }
  const std::string partitions = "blocks of 1, 7, 64, " + std::to_string(max) + " and random sizes";
  findings.report(context, "blocks",
                  worst > 0.0
                      ? partitions + " within the requested tolerance " +
                            number(options.tolerance) + " (max difference " + number(worst) + ")"
                      : partitions + " give bit-identical output");
}

void check_automation(const Context &context) {
  const Binary &binary = context.binary;
  const Options &options = context.options;
  if (binary.descriptors.empty()) {
    context.add("automation", Status::skip, "declares no parameters");
    return;
  }
  const uint32_t max = options.max_frames;
  const uint32_t block = std::min(ordinary_block, max);
  Findings findings;
  double worst = 0.0;
  uint32_t rejections = 0;
  for (uint32_t channels : binary.channel_counts) {
    const std::string layout = layout_name(channels);
    findings.attempt(layout + " offsets", [&] {
      Random random(options.seed + 20 + channels);
      const Settings settings = random_settings(binary, random);
      const size_t frames = 16384;
      const std::vector<Automation> automation = make_automation(binary, random, frames, 96);
      const Audio input = test_signal(channels, frames, check_rate, options.seed + 21);
      Instance offsets(binary);
      offsets.configure(settings);
      offsets.prepare(check_rate, channels, max);
      const Audio a = offsets.render(input, fixed_blocks(frames, block), automation);
      Instance split(binary);
      split.configure(settings);
      split.prepare(check_rate, channels, max);
      const Audio b = split.render(input, split_blocks(frames, block, automation), automation);
      const Difference difference = compare(a, b);
      if (!within(difference, options.tolerance)) {
        findings.fail(layout + ": events at frame offsets differ from splitting blocks there: " +
                      describe(difference));
      } else if (!difference.identical) {
        worst = std::max(worst, difference.maximum);
      }
      for (const Descriptor &d : binary.descriptors) {
        const auto last = std::find_if(automation.rbegin(), automation.rend(),
                                       [&](const Automation &e) { return e.id == d.id; });
        const auto current = offsets.value(d.id);
        if (last != automation.rend() && binary.parameters &&
            (!current || !matches_request(binary, d.id, *current, last->value))) {
          findings.fail(layout + ": get_value(" + std::to_string(d.id) + ") reports " +
                        (current ? number(*current) : std::string("an error")) +
                        " after the last event set " + number(last->value));
        }
      }
    });

    findings.attempt(layout + " invalid events", [&] {
      Random random(options.seed + 22 + channels);
      const Settings settings = random_settings(binary, random);
      const Audio first = test_signal(channels, 2048, check_rate, options.seed + 23);
      const Audio second = test_signal(channels, 2048, check_rate, options.seed + 24);
      Instance rejecting(binary);
      Instance reference(binary);
      for (Instance *instance : {&rejecting, &reference}) {
        instance->configure(settings);
        instance->prepare(check_rate, channels, max);
        instance->render(first, fixed_blocks(2048, 256));
      }
      const auto controlled = std::find_if(
          binary.descriptors.begin(), binary.descriptors.end(),
          [&](const Descriptor &d) { return !binary.bypass || d.id != *binary.bypass; });
      const Descriptor &target =
          controlled != binary.descriptors.end() ? *controlled : binary.descriptors.front();
      const Settings before =
          binary.parameters ? stored_settings(binary, rejecting, settings) : Settings{};
      const Settings &initial = binary.parameters ? before : settings;
      const auto start = std::find_if(initial.begin(), initial.end(),
                                      [&](const auto &s) { return s.first == target.id; });
      const float current = start != initial.end() ? start->second : target.default_value;
      const float other = current != target.maximum ? target.maximum : target.minimum;
      uint32_t undeclared = 1;
      while (binary.find(undeclared)) {
        ++undeclared;
      }
      const uint32_t id = target.id;
      const float infinity = std::numeric_limits<float>::infinity();
      const std::pair<std::string, std::vector<vs_parameter_event>> cases[] = {
          {"unordered offsets", {{id, 10, other}, {id, 5, other}}},
          {"an offset equal to the frame count", {{id, 0, other}, {id, 64, other}}},
          {"an undeclared parameter " + std::to_string(undeclared),
           {{id, 0, other}, {undeclared, 1, 0.0f}}},
          {"a NaN value", {{id, 0, other}, {id, 1, std::numeric_limits<float>::quiet_NaN()}}},
          {"an infinite value", {{id, 0, other}, {id, 1, infinity}}},
          {"a value above the maximum",
           {{id, 0, other}, {id, 1, std::nextafter(target.maximum, infinity)}}},
          {"a value below the minimum",
           {{id, 0, other}, {id, 1, std::nextafter(target.minimum, -infinity)}}},
      };
      for (const auto &[name, events] : cases) {
        RawBlock raw(channels, 64, 64);
        raw.block.events = events.data();
        raw.block.event_count = static_cast<uint32_t>(events.size());
        if (rejecting.process(raw.block) == VS_OK) {
          findings.fail(layout + ": process accepted events with " + name);
        } else {
          ++rejections;
        }
      }
      for (const auto &[parameter, original] : before) {
        const auto value = rejecting.value(parameter);
        if (!value || !same_bits(*value, original)) {
          findings.fail(layout + ": rejected blocks changed readback for parameter " +
                        std::to_string(parameter));
        }
      }
      const Difference difference = compare(rejecting.render(second, fixed_blocks(2048, 256)),
                                            reference.render(second, fixed_blocks(2048, 256)));
      if (!difference.identical) {
        findings.fail(layout + ": rejected blocks changed later output: " + describe(difference));
      }
    });
  }
  findings.report(context, "automation",
                  std::string("events at frame offsets match blocks split at those offsets") +
                      (worst > 0.0
                           ? " within the requested tolerance " + number(options.tolerance) +
                                 " (max difference " + number(worst) + ")"
                           : " bit for bit") +
                      "; " + std::to_string(rejections) + " invalid event lists rejected");
}

void check_bypass(const Context &context) {
  const Binary &binary = context.binary;
  const Options &options = context.options;
  if (!(binary.api->capabilities & VS_MODULE_BYPASS_PARAMETER)) {
    context.add("bypass", Status::skip, "declares no bypass parameter");
    return;
  }
  if (binary.no_serial_route) {
    context.add("bypass", Status::skip,
                "the host realises the bypass parameter in every route; see the routing check");
    return;
  }
  if (!binary.bypass) {
    context.add("bypass", Status::fail,
                "bypass_parameter " + std::to_string(binary.api->bypass_parameter) +
                    " is not a declared parameter ranging from 0 to 1");
    return;
  }
  const uint32_t id = *binary.bypass;
  const uint32_t max = options.max_frames;
  const uint32_t settle = frames_for(options.bypass_settle_ms, check_rate);
  Findings findings;
  double worst = 0.0;
  uint32_t latency = 0;
  const auto judge = [&](const std::string &where, const Difference &difference) {
    if (!within(difference, options.bypass_tolerance)) {
      findings.fail(where + ": bypassed output is not the input delayed by " +
                    std::to_string(latency) + " frames: " + describe(difference));
    } else if (!difference.identical) {
      worst = std::max(worst, difference.maximum);
    }
  };
  for (uint32_t channels : binary.channel_counts) {
    const std::string layout = layout_name(channels);
    findings.attempt(layout + " settled", [&] {
      Random random(options.seed + 30 + channels);
      Instance instance(binary);
      instance.configure(with_value(random_settings(binary, random), id, 0.0f));
      latency = instance.prepare(check_rate, channels, max).latency_frames;
      const size_t start = size_t{settle} + latency;
      const size_t frames = start + 16384;
      Audio input = test_signal(channels, frames, check_rate, options.seed + 31);
      const float specials[] = {-0.0f,
                                std::numeric_limits<float>::denorm_min(),
                                -std::numeric_limits<float>::min() / 2.0f,
                                std::numeric_limits<float>::min(),
                                1.0e6f,
                                -1.5f};
      for (auto &channel : input) {
        for (size_t i = start; i < frames; i += 97) {
          channel[i] = specials[(i / 97) % std::size(specials)];
        }
      }
      const Audio output =
          instance.render(input, fixed_blocks(frames, std::min(ordinary_block, max)));
      judge(layout + " with the bypass parameter at 0",
            compare(output, delayed(input, latency), start));
    });
    findings.attempt(layout + " toggling", [&] {
      Random random(options.seed + 32 + channels);
      Instance instance(binary);
      instance.configure(random_settings(binary, random));
      latency = instance.prepare(check_rate, channels, max).latency_frames;
      const size_t toggles_end = 48000;
      const size_t start = toggles_end + settle + latency;
      const size_t frames = start + 8192;
      std::vector<Automation> automation = make_automation(binary, random, toggles_end, 24);
      float state = 1.0f;
      for (size_t frame = random.below(3000); frame < toggles_end;
           frame += 300 + random.below(2700)) {
        state = state == 1.0f ? 0.0f : 1.0f;
        automation.push_back({frame, id, state});
      }
      std::stable_sort(automation.begin(), automation.end(),
                       [](const Automation &a, const Automation &b) { return a.frame < b.frame; });
      automation.push_back({toggles_end, id, 0.0f});
      const Audio input = test_signal(channels, frames, check_rate, options.seed + 33);
      const Audio output = instance.render(input, random_blocks(frames, max, random), automation);
      size_t frame = 0;
      if (!finite(output, &frame)) {
        findings.fail(layout +
                      ": toggling the bypass parameter produced a non-finite sample at "
                      "frame " +
                      std::to_string(frame));
      }
      judge(layout + " after toggling back to bypass",
            compare(output, delayed(input, latency), start));
    });
  }
  const std::string latency_text = "the input delayed by the reported latency (" +
                                   std::to_string(latency) + " frames) after " +
                                   number(options.bypass_settle_ms) + " ms";
  findings.report(
      context, "bypass",
      worst > 0.0 ? "bypassed output matches " + latency_text + " within the requested tolerance " +
                        number(options.bypass_tolerance) + " (max difference " + number(worst) +
                        "); toggling stays finite"
                  : "bypassed output is bit-exactly " + latency_text + "; toggling stays finite");
}

void check_state(const Context &context) {
  const Binary &binary = context.binary;
  const Options &options = context.options;
  if (!binary.state_exported) {
    context.add("state", Status::skip, "no state extension");
    return;
  }
  if (!binary.state) {
    context.add("state", Status::fail, binary.state_error);
    return;
  }
  const vs_module_state_api &state = *binary.state;
  const uint32_t schema = state.schema_version;
  const uint32_t channels = binary.channel_counts.front();
  const uint32_t max = options.max_frames;
  Findings findings;
  Random random(options.seed + 40);
  const Settings settings = random_settings(binary, random);
  // Without the parameter value extension, settings reach the module as events of a first block.
  const auto apply = [&](Instance &instance, const Settings &values) {
    instance.configure(values);
    if (!binary.parameters) {
      instance.prepare(check_rate, channels, max);
      instance.render(silence(channels, 1), {1});
      instance.release();
    }
  };
  std::vector<uint8_t> saved;
  Settings stored;
  bool table = false;
  uint32_t rejections = 0;

  findings.attempt("save", [&] {
    Instance source(binary);
    apply(source, settings);
    stored = binary.parameters ? stored_settings(binary, source, settings) : settings;
    saved = save_state(binary, source.handle());
    if (save_state(binary, source.handle()) != saved) {
      findings.fail("two saves of unchanged settings differ");
    }
    table = parameter_table(binary, saved, stored);
  });
  if (findings.failed()) {
    findings.report(context, "state", "");
    return;
  }

  findings.attempt("round trip", [&] {
    Instance restored(binary);
    const vs_result result = state.load(restored.handle(), schema, saved.data(), saved.size());
    if (result != VS_OK) {
      throw CheckError("loading saved state returned " + result_name(result));
    }
    if (save_state(binary, restored.handle()) != saved) {
      findings.fail("state saved after loading differs from the loaded bytes");
    }
    for (const auto &[id, value] : stored) {
      const auto current = restored.value(id);
      if (binary.parameters && (!current || !same_bits(*current, value))) {
        findings.fail("after loading, get_value(" + std::to_string(id) + ") reports " +
                      (current ? number(*current) : std::string("an error")) + " instead of " +
                      number(value));
      }
    }
    Instance expected(binary);
    apply(expected, settings);
    expected.prepare(check_rate, channels, max);
    restored.prepare(check_rate, channels, max);
    const Audio input = test_signal(channels, 8192, check_rate, options.seed + 41);
    const Difference difference = compare(restored.render(input, fixed_blocks(8192, 256)),
                                          expected.render(input, fixed_blocks(8192, 256)));
    if (!difference.identical) {
      findings.fail("output after loading state differs from an instance with the saved "
                    "settings: " +
                    describe(difference));
    }
  });

  findings.attempt("invalid state", [&] {
    Random other(options.seed + 42);
    const Settings baseline_settings = random_settings(binary, other);
    Instance target(binary);
    apply(target, baseline_settings);
    const Settings original =
        binary.parameters ? stored_settings(binary, target, baseline_settings) : baseline_settings;
    const std::vector<uint8_t> baseline = save_state(binary, target.handle());
    const auto reject = [&](const std::string &what, uint32_t version, const uint8_t *data,
                            uint64_t bytes) {
      if (state.load(target.handle(), version, data, bytes) == VS_OK) {
        findings.fail("load accepted " + what);
      } else {
        ++rejections;
      }
      if (save_state(binary, target.handle()) != baseline) {
        findings.fail("loading " + what + " changed the instance's state");
      }
      for (const auto &[id, value] : original) {
        const auto current = target.value(id);
        if (binary.parameters && (!current || !same_bits(*current, value))) {
          findings.fail("loading " + what + " changed parameter " + std::to_string(id));
        }
      }
    };
    if (!baseline.empty()) {
      reject("a state truncated by one byte", schema, baseline.data(), baseline.size() - 1);
      reject("a null buffer with a non-zero size", schema, nullptr, baseline.size());
    }
    std::vector<uint8_t> longer = baseline;
    longer.push_back(0);
    reject("a state one byte longer", schema, longer.data(), longer.size());
    reject("schema version 0", 0, baseline.data(), baseline.size());
    reject("schema version " + std::to_string(schema + 1), schema + 1, baseline.data(),
           baseline.size());
    table = table && parameter_table(binary, baseline, original);
    if (!table) {
      return;
    }
    const float infinity = std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < binary.descriptors.size(); ++i) {
      const Descriptor &d = binary.descriptors[i];
      const std::pair<const char *, float> values[] = {
          {"NaN", std::numeric_limits<float>::quiet_NaN()},
          {"infinity", infinity},
          {"a value above its maximum", std::nextafter(d.maximum, infinity)},
          {"a value below its minimum", std::nextafter(d.minimum, -infinity)}};
      for (const auto &[name, value] : values) {
        std::vector<uint8_t> mutated = baseline;
        std::memcpy(mutated.data() + i * sizeof(float), &value, sizeof(float));
        reject(std::string(name) + " for parameter " + std::to_string(d.id), schema, mutated.data(),
               mutated.size());
      }
    }
  });

  if (binary.files) {
    findings.attempt("file references stay outside state", [&] {
      Instance instance(binary);
      for (const auto &slot : binary.file_slots) {
        FileReference file{VS_FILE_MISSING, "", "first.wav", 0, {}};
        auto raw = file.descriptor();
        if (binary.files->set_file(instance.handle(), slot.id, &raw) != VS_OK) {
          continue;
        }
        const auto first = save_state(binary, instance.handle());
        file.name = "second.wav";
        file.sha256[0] = 1;
        raw = file.descriptor();
        if (binary.files->set_file(instance.handle(), slot.id, &raw) == VS_OK &&
            save_state(binary, instance.handle()) != first) {
          findings.fail("changing file slot " + std::to_string(slot.id) + " changed opaque state");
        }
      }
    });
  }
  findings.report(context, "state",
                  "save is deterministic; a round trip restores parameters and output; " +
                      std::to_string(rejections) + " invalid states rejected without changes" +
                      (table ? ", including non-finite and out-of-range values in its binary32 "
                               "parameter table"
                             : "; the state is not a binary32 parameter table, so value "
                               "mutations were not tried"));
}

void check_robustness(const Context &context) {
  const Binary &binary = context.binary;
  const Options &options = context.options;
  const uint32_t max = options.max_frames;
  const uint32_t block = std::min(ordinary_block, max);
  Findings findings;
  uint32_t configurations = 0;
  uint32_t passed_through = 0;
  const std::pair<const char *, float (*)(size_t, float)> families[] = {
      {"NaN",
       [](size_t i, float tone) {
         return i % 3 == 0 ? std::numeric_limits<float>::quiet_NaN() : tone;
       }},
      {"infinite",
       [](size_t i, float tone) {
         const float inf = std::numeric_limits<float>::infinity();
         return i % 5 == 0 ? inf : i % 5 == 1 ? -inf : tone;
       }},
      {"huge",
       [](size_t i, float) {
         const float values[] = {1.0e30f, 1.0e6f, -1.0e30f, -1.0e6f};
         return values[i % 4];
       }},
      {"subnormal",
       [](size_t i, float) {
         return (i % 2 ? -1.0f : 1.0f) * std::numeric_limits<float>::denorm_min() *
                static_cast<float>(1 + i % 1000);
       }},
      {"full-scale square", [](size_t i, float) { return (i / 32) % 2 ? 1.0f : -1.0f; }},
  };
  std::vector<std::pair<std::string, Settings>> setting_sets{
      {"defaults", processing_defaults(binary)},
      {"minimum", endpoint_settings(binary, false)},
      {"maximum", endpoint_settings(binary, true)}};
  Random random(options.seed + 50);
  setting_sets.emplace_back("random", random_settings(binary, random));
  if (binary.bypass) {
    setting_sets.emplace_back("bypassed",
                              with_value(processing_defaults(binary), *binary.bypass, 0.0f));
  }

  for (double rate : options.rates) {
    for (uint32_t channels : binary.channel_counts) {
      for (const auto &[setting_name, settings] : setting_sets) {
        const std::string where =
            rate_name(rate) + " " + layout_name(channels) + " " + setting_name;
        findings.attempt(where, [&] {
          Instance instance(binary);
          instance.configure(settings);
          const uint32_t latency = instance.prepare(rate, channels, max).latency_frames;
          ++configurations;
          Audio fed;
          Audio produced;
          bool passing_through = false;
          const auto run = [&](const Audio &input, const std::string &what) {
            const auto start = std::chrono::steady_clock::now();
            const Audio output = instance.render(input, {block});
            const double elapsed = seconds_since(start);
            append(fed, input);
            append(produced, output);
            size_t frame = 0;
            if (!finite(output, &frame)) {
              if (!compare(produced, delayed(fed, latency)).identical) {
                throw CheckError(what + " produced a non-finite sample at frame " +
                                 std::to_string(frame) +
                                 " without passing all input through unchanged");
              }
              passing_through = true;
            }
            return elapsed;
          };
          run(silence(channels, block), "silence right after prepare");
          const Audio tone = test_signal(channels, size_t{block} * 3, rate, options.seed + 51);
          double ordinary = std::numeric_limits<double>::infinity();
          for (size_t i = 0; i < 3; ++i) {
            ordinary = std::min(ordinary, run(slice(tone, i * block, block), "ordinary input"));
          }
          for (const auto &[name, generate] : families) {
            Audio input = slice(tone, 0, block);
            for (auto &channel : input) {
              for (size_t i = 0; i < channel.size(); ++i) {
                channel[i] = generate(i, channel[i]);
              }
            }
            double hostile = std::numeric_limits<double>::infinity();
            for (int repeat = 0; repeat < 3; ++repeat) {
              hostile = std::min(hostile, run(input, std::string(name) + " input"));
            }
            if (hostile > options.stall_factor * ordinary + 0.002) {
              throw CheckError(std::string(name) + " input takes " + number(hostile * 1000.0) +
                               " ms per block of " + std::to_string(block) + " frames, ordinary " +
                               "input " + number(ordinary * 1000.0) + " ms");
            }
          }
          run(silence(channels, block), "silence after hostile input");
          run(silence(channels, block), "silence after hostile input");
          passed_through += passing_through ? 1 : 0;
        });
      }
    }
  }

  for (uint32_t channels : binary.channel_counts) {
    const std::string layout = layout_name(channels);
    findings.attempt(layout + " discontinuity", [&] {
      Random local(options.seed + 52 + channels);
      const Settings settings = binary.parameters ? random_settings(binary, local) : Settings{};
      const Audio first = test_signal(channels, 4096, check_rate, options.seed + 53);
      const Audio second = test_signal(channels, 4096, check_rate, options.seed + 54);
      Instance flagged(binary);
      Instance reset(binary);
      for (Instance *instance : {&flagged, &reset}) {
        instance->configure(settings);
        instance->prepare(check_rate, channels, max);
        instance->render(first, fixed_blocks(4096, 256));
      }
      const Audio a = flagged.render(second, fixed_blocks(4096, 256), {}, VS_PROCESS_DISCONTINUITY);
      reset.reset();
      const Difference difference = compare(a, reset.render(second, fixed_blocks(4096, 256)));
      if (!within(difference, options.tolerance)) {
        findings.fail(layout +
                      ": output after VS_PROCESS_DISCONTINUITY differs from output "
                      "after reset(): " +
                      describe(difference));
      }
    });
  }

  std::string finite_text = std::to_string(configurations) +
                            " configurations stay finite for NaN, infinite, huge, subnormal and "
                            "full-scale input and silence";
  if (passed_through != 0) {
    finite_text += " (" + std::to_string(passed_through) +
                   " pass non-finite input through bit-exactly, delayed by the reported "
                   "latency, which is accepted)";
  }
  findings.report(context, "robustness",
                  finite_text + ", within " + number(options.stall_factor) +
                      "x ordinary block time; VS_PROCESS_DISCONTINUITY matches reset()");
}

void check_latency(const Context &context) {
  const Binary &binary = context.binary;
  const Options &options = context.options;
  const uint32_t max = options.max_frames;
  Findings findings;
  std::vector<std::string> reported;
  for (double rate : options.rates) {
    for (uint32_t channels : binary.channel_counts) {
      findings.attempt(rate_name(rate) + " " + layout_name(channels), [&] {
        Instance instance(binary);
        const vs_module_timing first = instance.prepare(rate, channels, max);
        instance.release();
        const vs_module_timing second = instance.prepare(rate, channels, max);
        if (first.latency_frames != second.latency_frames ||
            first.tail_frames != second.tail_frames) {
          findings.fail(rate_name(rate) + " " + layout_name(channels) +
                        ": repeated prepares report different latency or tail");
        }
        if (channels == binary.channel_counts.front()) {
          reported.push_back(rate_name(rate) + " latency " + std::to_string(first.latency_frames) +
                             ", tail " +
                             (first.tail_frames == UINT64_MAX ? std::string("unbounded")
                                                              : std::to_string(first.tail_frames)));
        }
      });
    }
  }

  std::string alignment = "dry-path alignment not measured: no usable bypass parameter";
  if (binary.own_bypass()) {
    for (uint32_t channels : binary.channel_counts) {
      findings.attempt(layout_name(channels) + " impulse", [&] {
        Instance instance(binary);
        instance.configure(with_value(processing_defaults(binary), *binary.bypass, 0.0f));
        const uint32_t latency = instance.prepare(check_rate, channels, max).latency_frames;
        const size_t impulse = size_t{frames_for(options.bypass_settle_ms, check_rate)} + 64;
        const size_t frames = impulse + latency + 4096;
        Audio input = silence(channels, frames);
        for (auto &channel : input) {
          channel[impulse] = 0.5f;
        }
        const Audio output =
            instance.render(input, fixed_blocks(frames, std::min(ordinary_block, max)));
        for (uint32_t c = 0; c < channels; ++c) {
          size_t peak = 0;
          for (size_t i = 0; i < frames; ++i) {
            if (std::abs(output[c][i]) > std::abs(output[c][peak])) {
              peak = i;
            }
          }
          if (output[c][peak] == 0.0f) {
            findings.fail(layout_name(channels) + ": a bypassed impulse produced no output");
            break;
          }
          if (peak != impulse + latency) {
            findings.fail(
                layout_name(channels) + ": a bypassed impulse is delayed by " +
                std::to_string(static_cast<long long>(peak) - static_cast<long long>(impulse)) +
                " frames, but prepare reports a latency of " + std::to_string(latency));
            break;
          }
        }
      });
    }
    alignment = "a bypassed impulse is delayed by exactly the reported latency";
  }

  findings.attempt("tail", [&] {
    const uint32_t channels = binary.channel_counts.front();
    Instance instance(binary);
    instance.configure(processing_defaults(binary));
    const vs_module_timing timing = instance.prepare(check_rate, channels, max);
    if (timing.tail_frames == UINT64_MAX) {
      findings.warn("declares an unbounded tail (UINT64_MAX)");
      return;
    }
    const uint64_t silent_after = 24000 + uint64_t{timing.latency_frames} + timing.tail_frames;
    if (silent_after > static_cast<uint64_t>(30.0 * check_rate)) {
      findings.warn("latency plus tail exceed 30 s at 48 kHz; decay not measured");
      return;
    }
    const size_t frames = static_cast<size_t>(silent_after) + 4096;
    Audio input = silence(channels, frames);
    const Audio material = test_signal(channels, 24000, check_rate, options.seed + 60);
    for (uint32_t c = 0; c < channels; ++c) {
      std::copy(material[c].begin(), material[c].end(), input[c].begin());
    }
    const Audio output =
        instance.render(input, fixed_blocks(frames, std::min(ordinary_block, max)));
    double peak = 0.0;
    for (const auto &channel : output) {
      for (size_t i = static_cast<size_t>(silent_after); i < channel.size(); ++i) {
        peak = std::max(peak, static_cast<double>(std::abs(channel[i])));
      }
    }
    if (!(peak <= 1.0e-4)) {
      findings.warn("output still reaches " + number(peak) +
                    " after the input has been silent for latency + tail frames");
    }
  });

  findings.report(context, "latency",
                  list(reported, "; ") + "; stable across repeated prepares; " + alignment +
                      "; output decays within latency + tail");
}

void check_realtime(const Context &context) {
  const Binary &binary = context.binary;
  const Options &options = context.options;
  const char *mechanism = allocation::mechanism();
  if (!mechanism) {
    context.add("realtime", Status::skip,
                "this build cannot observe heap allocation (it needs macOS libmalloc without a "
                "sanitizer replacing malloc)");
    return;
  }
  const uint32_t max = options.max_frames;
  const uint64_t earlier = process_allocations() - context.allocations_before;
  Findings findings;
  const auto phase = [&](const std::string &name, auto &&work) {
    const uint64_t before = process_allocations();
    work();
    const uint64_t count = process_allocations() - before;
    if (count != 0) {
      findings.fail(std::to_string(count) + " allocation or free events inside process during " +
                    name);
    }
  };
  uint64_t adopted = 0;
  for (uint32_t channels : binary.channel_counts) {
    const std::string layout = layout_name(channels);
    findings.attempt(layout, [&] {
      Random random(options.seed + 70 + channels);
      // Work runs between blocks, so results are adopted, and earlier ones handed back, inside
      // the counted process calls.
      Instance instance(binary, HostWorkMode::manual);
      const auto run_work = [&] {
        if (instance.services()) {
          instance.services()->services().run_pending();
        }
      };
      instance.configure(random_settings(binary, random));
      instance.prepare(check_rate, channels, max);
      const size_t frames = 48000;
      const Audio input = test_signal(channels, frames, check_rate, options.seed + 71);
      const uint32_t first = std::min<uint32_t>(64, max);
      phase(layout + " first block after prepare",
            [&] { instance.render(slice(input, 0, first), {first}); });
      const std::vector<Automation> automation =
          make_automation(binary, random, frames - first, 200);
      phase(layout + " automation in blocks of 1 to " + std::to_string(max) + " frames", [&] {
        instance.render(slice(input, first, frames - first),
                        random_blocks(frames - first, max, random), automation, 0, run_work);
      });
      phase(layout + " a discontinuity", [&] {
        instance.render(slice(input, 0, 4096), fixed_blocks(4096, 256), {},
                        VS_PROCESS_DISCONTINUITY);
      });
      instance.reset();
      phase(layout + " the first block after reset",
            [&] { instance.render(slice(input, 0, 256), {256}); });
      instance.release();
      instance.prepare(96000.0, channels, max);
      phase(layout + " the first block after preparing again",
            [&] { instance.render(slice(input, 0, 256), {256}); });
      if (instance.services()) {
        adopted += instance.services()->statistics().adopted;
      }
    });
  }
  if (earlier != 0) {
    findings.fail(std::to_string(earlier) +
                  " allocation or free events inside process during earlier checks");
  }
  findings.report(context, "realtime",
                  std::string("no allocation or free inside process") +
                      (adopted ? ", including " + std::to_string(adopted) +
                                     " blocks adopting results of background work"
                               : std::string()) +
                      "; observed with " + mechanism);
}

void check_cpu(const Context &context) {
  const Binary &binary = context.binary;
  const Options &options = context.options;
  const uint32_t block = options.cpu_block;
  Findings findings;
  std::vector<std::string> measured;
  for (double rate : options.rates) {
    for (uint32_t channels : binary.channel_counts) {
      const std::string where = rate_name(rate) + " " + layout_name(channels);
      findings.attempt(where, [&] {
        Instance instance(binary);
        instance.configure(processing_defaults(binary));
        instance.prepare(rate, channels, block);
        const size_t loop_blocks = 256;
        const Audio input = test_signal(channels, loop_blocks * block, rate, options.seed + 80);
        Audio output = silence(channels, block);
        instance.render(slice(input, 0, block), {block});
        const auto blocks = static_cast<uint64_t>(
            std::max(1.0, std::ceil(options.cpu_seconds * rate / static_cast<double>(block))));
        std::vector<const float *> inputs(channels);
        std::vector<float *> outputs(channels);
        for (uint32_t c = 0; c < channels; ++c) {
          outputs[c] = output[c].data();
        }
        vs_process_block raw{};
        raw.struct_size = sizeof(vs_process_block);
        raw.inputs = inputs.data();
        raw.outputs = outputs.data();
        raw.channels = channels;
        raw.frames = block;
        const auto start = std::chrono::steady_clock::now();
        for (uint64_t b = 0; b < blocks; ++b) {
          const size_t offset = static_cast<size_t>(b % loop_blocks) * block;
          for (uint32_t c = 0; c < channels; ++c) {
            inputs[c] = input[c].data() + offset;
          }
          raw.frame_position = (b + 1) * block;
          const vs_result result = instance.process(raw);
          if (result != VS_OK) {
            throw CheckError("process returned " + result_name(result));
          }
        }
        const double elapsed = seconds_since(start);
        const double percent = elapsed / (static_cast<double>(blocks * block) / rate) * 100.0;
        measured.push_back(where + " " + number(percent) + "%");
        if (percent > options.cpu_limit_percent) {
          findings.fail(where + " uses " + number(percent) + "% of real time, above the limit of " +
                        number(options.cpu_limit_percent) + "%");
        }
      });
    }
  }
  if (findings.failed()) {
    findings.report(context, "cpu", "");
    return;
  }
  context.add("cpu", Status::info,
              "real time used with blocks of " + std::to_string(block) + " frames: " +
                  list(measured) + " (limit " + number(options.cpu_limit_percent) + "%)");
}

namespace {

// What is wrong with a status report, or nothing.
std::optional<std::string> status_problem(const vs_module_status &status) {
  const size_t length = strnlen(status.message, sizeof(status.message));
  if (length == sizeof(status.message)) {
    return "the status message is not NUL-terminated within 256 bytes";
  }
  if (!vespera::display_detail::valid_utf8(std::string_view(status.message, length))) {
    return "the status message is not UTF-8";
  }
  if (status.kind > VS_STATUS_ERROR) {
    return "the status kind " + std::to_string(status.kind) + " is unknown";
  }
  if (std::isnan(status.progress) || status.progress > 1.0f) {
    return "the status progress " + number(status.progress) + " is outside [0, 1]";
  }
  return std::nullopt;
}

const char *no_services = "the module does not export the services extension";

// Settings, automation and channel count shared by the services checks.
struct WorkScenario {
  uint32_t channels;
  Settings settings;
  std::vector<Automation> automation;
  Audio input;
};

WorkScenario work_scenario(const Context &context, size_t frames, uint32_t events, uint64_t salt) {
  const Binary &binary = context.binary;
  Random random(context.options.seed + salt);
  WorkScenario scenario{binary.channel_counts.back(), random_settings(binary, random), {}, {}};
  scenario.automation = make_automation(binary, random, frames, events);
  scenario.input = test_signal(scenario.channels, frames, check_rate, context.options.seed + salt);
  return scenario;
}

}

void check_services(const Context &context) {
  const Binary &binary = context.binary;
  if (!binary.services_exported) {
    context.add("services", Status::skip, no_services);
    return;
  }
  if (!binary.services) {
    context.add("services", Status::fail, "cannot run: " + binary.services_error);
    return;
  }
  Findings findings;
  size_t reports = 0;
  const auto inspect = [&](const std::string &when, const std::optional<vs_module_status> &status) {
    ++reports;
    if (!status) {
      findings.fail("status failed " + when);
    } else if (auto problem = status_problem(*status)) {
      findings.fail(*problem + " " + when);
    } else if (status->file_slot != 0 &&
               std::none_of(binary.file_slots.begin(), binary.file_slots.end(),
                            [&](const auto &slot) { return slot.id == status->file_slot; })) {
      findings.fail("status names an undeclared file slot " + when);
    } else if (status->kind == VS_STATUS_READY && status->message[0] != '\0') {
      findings.warn("a ready status carries the message \"" + std::string(status->message) + "\" " +
                    when);
    }
  };
  const WorkScenario scenario = work_scenario(context, 9600, 24, 90);
  bool answered = false;
  bool held = false;
  findings.attempt("status", [&] {
    Instance instance(binary, HostWorkMode::manual);
    inspect("after attach", instance.status());
    instance.configure(scenario.settings);
    instance.prepare(check_rate, scenario.channels, ordinary_block);
    inspect("after prepare", instance.status());
    instance.render(scenario.input, fixed_blocks(9600, ordinary_block), scenario.automation);
    inspect("with work pending", instance.status());
    instance.services()->services().run_pending();
    instance.render(scenario.input, fixed_blocks(9600, ordinary_block));
    inspect("after work ran", instance.status());
    instance.release();
    inspect("after release", instance.status());
  });
  findings.attempt("status while processing", [&] {
    Instance instance(binary, HostWorkMode::manual);
    instance.configure(scenario.settings);
    instance.prepare(check_rate, scenario.channels, ordinary_block);
    std::atomic<bool> done{false};
    std::string failure;
    std::thread audio([&] {
      try {
        for (int pass = 0; pass < 4; ++pass) {
          instance.render(scenario.input, fixed_blocks(9600, ordinary_block), scenario.automation,
                          0, [&] { instance.services()->services().run_pending(); });
        }
      } catch (const std::exception &error) {
        failure = error.what();
      }
      done = true;
    });
    size_t calls = 0;
    while (!done) {
      const auto status = instance.status();
      if (++calls % 64 == 0 || !status || status_problem(*status)) {
        inspect("while another thread processed", status);
      }
    }
    audio.join();
    if (!failure.empty()) {
      throw CheckError(failure);
    }
    // Hold the processing thread inside process, at its next hand-off call, and ask for the
    // status from another thread: it must answer without waiting for process to return.
    if (instance.services()->statistics().opened == 0) {
      return;
    }
    instance.services()->hold_processing();
    std::thread blocked([&] {
      instance.render(scenario.input, fixed_blocks(2048, ordinary_block), scenario.automation);
    });
    held = instance.services()->wait_processing_held(std::chrono::steady_clock::now() +
                                                     std::chrono::seconds(5));
    if (held) {
      std::mutex mutex;
      std::condition_variable changed;
      bool returned = false;
      std::thread asking([&] {
        (void)instance.status();
        std::lock_guard lock(mutex);
        returned = true;
        changed.notify_all();
      });
      {
        std::unique_lock lock(mutex);
        answered = changed.wait_for(lock, std::chrono::seconds(1), [&] { return returned; });
      }
      instance.services()->release_hold();
      asking.join();
      if (!answered) {
        findings.fail("status waited for the processing thread to leave process");
      }
    }
    instance.services()->release_hold();
    blocked.join();
  });
  findings.report(context, "services",
                  std::to_string(reports) + " status reports are well formed" +
                      (answered ? "; status answers while process is in progress"
                       : held   ? std::string()
                                : "; the module does not use background work, so status "
                                  "was not checked while process was held"));
}

void check_pending(const Context &context) {
  const Binary &binary = context.binary;
  if (!binary.services) {
    context.add("pending", Status::skip, binary.services_exported ? "cannot run" : no_services);
    return;
  }
  const Options &options = context.options;
  const size_t frames = static_cast<size_t>(check_rate) * 2;
  const WorkScenario scenario = work_scenario(context, frames, 40, 91);
  Findings findings;
  uint64_t requested = 0;
  double slowest = 0.0;
  findings.attempt("pending", [&] {
    Instance instance(binary, HostWorkMode::manual);
    instance.configure(scenario.settings);
    instance.prepare(check_rate, scenario.channels, ordinary_block);
    HostServices &services = instance.services()->services();
    std::vector<double> durations;
    Audio output;
    std::string failure;
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    const uint64_t allocations = process_allocations();
    std::thread audio([&] {
      try {
        auto last = std::chrono::steady_clock::now();
        output = instance.render(
            scenario.input, fixed_blocks(frames, ordinary_block), scenario.automation, 0, [&] {
              const auto now = std::chrono::steady_clock::now();
              durations.push_back(std::chrono::duration<double>(now - last).count());
              last = now;
            });
      } catch (const std::exception &error) {
        failure = error.what();
      }
      std::lock_guard lock(mutex);
      done = true;
      changed.notify_all();
    });
    {
      std::unique_lock lock(mutex);
      if (!changed.wait_for(lock, std::chrono::seconds(20), [&] { return done; })) {
        findings.fail("process did not finish 2 s of audio within 20 s while work was pending");
        while (!changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return done; })) {
          lock.unlock();
          services.run_pending();
          lock.lock();
        }
      }
    }
    audio.join();
    if (!failure.empty()) {
      throw CheckError(failure + " while work was pending");
    }
    requested = services.statistics().requested;
    if (allocation::mechanism() && process_allocations() != allocations) {
      findings.fail(std::to_string(process_allocations() - allocations) +
                    " allocation or free events inside process while work was pending");
    }
    size_t frame = 0;
    if (!finite(output, &frame)) {
      findings.fail("non-finite output at frame " + std::to_string(frame) +
                    " while work was pending");
    }
    // One slow block can be the scheduler's doing; a module waiting for its work stalls every
    // block that follows a request.
    if (!durations.empty()) {
      std::vector<double> sorted = durations;
      std::sort(sorted.begin(), sorted.end());
      const double median = sorted[sorted.size() / 2];
      const double bound = std::max(median * options.stall_factor + 0.002, 0.01);
      slowest = sorted.back();
      const auto stalled = std::count_if(sorted.begin(), sorted.end(),
                                         [&](double duration) { return duration > bound; });
      if (stalled >= 3) {
        findings.fail(std::to_string(stalled) + " blocks took longer than " +
                      number(bound * 1000.0) + " ms while work was pending (slowest " +
                      number(slowest * 1000.0) + " ms, median " + number(median * 1000.0) +
                      " ms); process must not wait for work");
      }
    }
    services.run_pending();
    instance.render(slice(scenario.input, 0, 12000), fixed_blocks(12000, ordinary_block));
    const auto stats = services.statistics();
    if (stats.delivered > 0 && stats.adopted == 0) {
      findings.fail(std::to_string(stats.delivered) +
                    " results were delivered but none was adopted in the following blocks");
    }
  });
  if (!findings.failed() && requested == 0) {
    context.add("pending", Status::skip,
                "the module requested no background work under automation");
    return;
  }
  findings.report(context, "pending",
                  std::to_string(requested) +
                      " requests stayed pending for 2 s of processing without stalling it "
                      "(slowest block " +
                      number(slowest * 1000.0) + " ms); results were adopted once the work ran");
}

void check_cancel(const Context &context) {
  const Binary &binary = context.binary;
  if (!binary.services) {
    context.add("cancel", Status::skip, binary.services_exported ? "cannot run" : no_services);
    return;
  }
  const WorkScenario scenario = work_scenario(context, 4800, 12, 92);
  Findings findings;
  uint64_t requested = 0;
  double longest = 0.0;
  const auto balance = [&](const std::string &scenario_name, ServicesHost &host) {
    host.services().run_pending();
    const auto stats = host.statistics();
    requested += stats.requested;
    if (stats.left_open != 0) {
      findings.fail(scenario_name + ": " + std::to_string(stats.left_open) +
                    " works were still open when release returned");
    }
    if (stats.produced != stats.disposed) {
      findings.fail(scenario_name + ": " + std::to_string(stats.produced) +
                    " results were produced but " + std::to_string(stats.disposed) +
                    " disposed by the host; a result must be handed back exactly once");
    }
    if (host.calls_after_close() != 0) {
      findings.fail(scenario_name + ": module work ran or disposed after its work was closed");
    }
  };
  findings.attempt("release with work queued", [&] {
    Instance instance(binary, HostWorkMode::manual);
    instance.configure(scenario.settings);
    instance.prepare(check_rate, scenario.channels, ordinary_block);
    instance.render(scenario.input, fixed_blocks(4800, ordinary_block), scenario.automation);
    instance.release();
    balance("release with work queued", *instance.services());
  });
  findings.attempt("cancelling a run", [&] {
    Instance instance(binary, HostWorkMode::manual);
    instance.configure(scenario.settings);
    instance.prepare(check_rate, scenario.channels, ordinary_block);
    ServicesHost &host = *instance.services();
    for (int attempt = 0; attempt < 4; ++attempt) {
      instance.render(scenario.input, fixed_blocks(4800, ordinary_block), scenario.automation, 0,
                      [&] {
                        host.cancel_next_run();
                        host.services().run_one();
                      });
    }
    longest = host.typical_cancelled_return();
    if (longest > 0.05) {
      findings.fail("runs took " + number(longest * 1000.0) +
                    " ms to return after they were cancelled; check cancellation at least every "
                    "10 ms");
    }
    instance.release();
    balance("cancelling a run", host);
  });
  findings.attempt("release after adopting results", [&] {
    Instance instance(binary, HostWorkMode::manual);
    instance.configure(scenario.settings);
    instance.prepare(check_rate, scenario.channels, ordinary_block);
    ServicesHost &host = *instance.services();
    for (int pass = 0; pass < 4; ++pass) {
      instance.configure(endpoint_settings(binary, pass % 2 == 0));
      host.services().run_pending();
      instance.render(scenario.input, fixed_blocks(4800, ordinary_block));
      host.services().run_pending();
    }
    instance.release();
    balance("release after adopting results", host);
  });
  findings.attempt("release while a run is in progress", [&] {
    Instance instance(binary, HostWorkMode::manual);
    instance.configure(scenario.settings);
    instance.prepare(check_rate, scenario.channels, ordinary_block);
    ServicesHost &host = *instance.services();
    instance.render(scenario.input, fixed_blocks(4800, ordinary_block), scenario.automation);
    std::thread worker([&] { host.services().run_one(); });
    host.services().wait_running(std::chrono::steady_clock::now() + std::chrono::milliseconds(200));
    instance.release();
    worker.join();
    balance("release while a run is in progress", host);
  });
  if (!findings.failed() && requested == 0) {
    context.add("cancel", Status::skip, "the module requested no background work under automation");
    return;
  }
  findings.report(context, "cancel",
                  "release closes queued and running work; cancelled runs typically returned "
                  "within " +
                      number(longest * 1000.0) +
                      " ms; every result was handed back and disposed exactly once");
}

void check_offline(const Context &context) {
  const Binary &binary = context.binary;
  if (!binary.services) {
    context.add("offline", Status::skip, binary.services_exported ? "cannot run" : no_services);
    return;
  }
  const Options &options = context.options;
  const size_t frames = 16384;
  const WorkScenario scenario = work_scenario(context, frames, 96, 93);
  Findings findings;
  uint64_t requested = 0;
  findings.attempt("offline", [&] {
    Random random(options.seed + 94);
    const std::vector<std::pair<std::string, std::vector<uint32_t>>> partitions{
        {"1", fixed_blocks(frames, 1)},
        {"7", fixed_blocks(frames, 7)},
        {"64", fixed_blocks(frames, 64)},
        {std::to_string(options.max_frames), fixed_blocks(frames, options.max_frames)},
        {"random", random_blocks(frames, options.max_frames, random)}};
    std::optional<Audio> reference;
    for (const auto &[name, blocks] : partitions) {
      Instance instance(binary);
      instance.configure(scenario.settings);
      instance.prepare(check_rate, scenario.channels, options.max_frames);
      const Audio output = instance.render(scenario.input, blocks, scenario.automation);
      requested = std::max(requested, instance.services()->statistics().requested);
      if (!reference) {
        reference = output;
        continue;
      }
      const Difference difference = compare(*reference, output);
      if (!within(difference, options.tolerance)) {
        findings.fail("with work run inline, blocks of " + name +
                      " frames differ from blocks of 1: " + describe(difference));
      }
    }
  });
  if (!findings.failed() && requested == 0) {
    context.add("offline", Status::skip,
                "the module requested no background work under automation");
    return;
  }
  findings.report(context, "offline",
                  "with work run inline at the frames that requested it (" +
                      std::to_string(requested) + " requests), blocks of 1, 7, 64, " +
                      std::to_string(options.max_frames) + " and random sizes give " +
                      (options.tolerance > 0 ? "matching" : "bit-identical") + " output");
}

void check_files(const Context &context) {
  const auto &binary = context.binary;
  if (!binary.files || binary.file_slots.empty()) {
    context.add("files", Status::skip, "no file slots");
    return;
  }
  Findings findings;
  std::string root =
      (std::filesystem::temp_directory_path() / "vespera-check-files-XXXXXX").string();
  if (!::mkdtemp(root.data())) {
    throw CheckError("cannot create file fixture directory");
  }
  struct Cleanup {
    std::string root;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
  } cleanup{root};
  for (const auto &slot : binary.file_slots) {
    for (uint32_t state : {uint32_t{VS_FILE_NONE}, uint32_t{VS_FILE_MISSING}}) {
      findings.attempt(slot.name + (state == VS_FILE_NONE ? " empty" : " missing"), [&] {
        Instance instance(binary);
        FileReference reference;
        reference.state = state;
        reference.name = state == VS_FILE_MISSING ? "missing.wav" : "";
        auto raw = reference.descriptor();
        if (binary.files->set_file(instance.handle(), slot.id, &raw) != VS_OK) {
          throw CheckError("set_file rejected a missing or empty reference");
        }
        instance.prepare(check_rate, binary.channel_counts.front(), ordinary_block);
        const auto status = instance.status();
        if (status && (state == VS_FILE_MISSING || !slot.optional) &&
            status->kind != VS_STATUS_ERROR) {
          findings.fail("missing/required-empty file did not report ERROR");
        }
        instance.render(Audio(binary.channel_counts.front(), std::vector<float>(1024, 0.25f)),
                        fixed_blocks(1024, 64));
      });
    }
    if (("," + slot.types + ",").find(",wav,") == std::string::npos) {
      continue;
    }
    for (bool valid : {false, true}) {
      findings.attempt(slot.name + (valid ? " WAV" : " corrupt"), [&] {
        std::vector<uint8_t> bytes;
        const auto word = [&](uint32_t value, uint32_t count) {
          for (uint32_t i = 0; i < count; ++i) {
            bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
          }
        };
        const auto tag = [&](const char *text) { bytes.insert(bytes.end(), text, text + 4); };
        if (valid) {
          tag("RIFF");
          word(38, 4);
          tag("WAVE");
          tag("fmt ");
          word(16, 4);
          word(1, 2);
          word(1, 2);
          word(48000, 4);
          word(96000, 4);
          word(2, 2);
          word(16, 2);
          tag("data");
          word(2, 4);
          word(16384, 2);
        } else {
          bytes = {0, 1, 2, 3};
        }
        if (bytes.size() > slot.max_bytes) {
          return;
        }
        const auto path = std::filesystem::path(root) / "response.wav";
        {
          std::ofstream file(path, std::ios::binary);
          file.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        }
        Instance instance(binary);
        FileReference reference{VS_FILE_PRESENT, path.string(), "response.wav", bytes.size(),
                                content_sha256(bytes)};
        auto raw = reference.descriptor();
        if (binary.files->set_file(instance.handle(), slot.id, &raw) != VS_OK) {
          throw CheckError("set_file rejected a well-formed reference");
        }
        instance.prepare(check_rate, binary.channel_counts.front(), ordinary_block);
        auto status = instance.status();
        if (binary.file_slots.size() == 1 && status &&
            ((valid && status->kind != VS_STATUS_READY) ||
             (!valid && status->kind != VS_STATUS_ERROR))) {
          findings.fail("file build reported an unexpected status");
        }
        instance.render(Audio(binary.channel_counts.front(), std::vector<float>(1024, 0.25f)),
                        fixed_blocks(1024, 64));
      });
    }
  }
  findings.report(context, "files",
                  "empty/missing references are accepted; single-slot WAV fixtures report "
                  "readiness/errors and process safely");
}

void check_threads(const Context &context) {
  const Binary &binary = context.binary;
  if (!binary.services) {
    context.add("threads", Status::skip, binary.services_exported ? "cannot run" : no_services);
    return;
  }
  const WorkScenario scenario = work_scenario(context, 9600, 24, 95);
  Findings findings;
  HostServices::Statistics total;
  const auto add = [&](const HostServices::Statistics &stats) {
    total.log_in_process += stats.log_in_process;
    total.open_outside_prepare += stats.open_outside_prepare;
    total.request_outside_call += stats.request_outside_call;
    total.adopt_outside_call += stats.adopt_outside_call;
    total.close_outside_release += stats.close_outside_release;
  };
  for (const HostWorkMode mode : {HostWorkMode::manual, HostWorkMode::synchronous}) {
    findings.attempt(mode == HostWorkMode::manual ? "live" : "offline", [&] {
      Instance instance(binary, mode);
      instance.configure(scenario.settings);
      instance.prepare(check_rate, scenario.channels, ordinary_block);
      ServicesHost &host = *instance.services();
      instance.render(scenario.input, fixed_blocks(9600, ordinary_block), scenario.automation, 0,
                      [&] { host.services().run_pending(); });
      instance.configure(random_settings(binary, *std::make_unique<Random>(context.options.seed)));
      host.services().run_pending();
      instance.reset();
      instance.render(scenario.input, fixed_blocks(4800, ordinary_block));
      instance.release();
      host.services().run_pending();
      add(host.statistics());
    });
  }
  const std::pair<uint64_t, const char *> misuse[] = {
      {total.log_in_process, "logged from inside process"},
      {total.open_outside_prepare, "opened work outside prepare"},
      {total.request_outside_call,
       "requested work from a thread that was not inside a call on the instance"},
      {total.adopt_outside_call, "adopted or handed back results outside process"},
      {total.close_outside_release, "closed work outside release"}};
  for (const auto &[count, what] : misuse) {
    if (count != 0) {
      findings.fail(std::string(what) + " " + std::to_string(count) + " time(s)");
    }
  }
  findings.report(context, "threads",
                  "every service call came from a thread and call the contract allows");
}

void check_routing(const Context &context, const RoutingTarget &routing) {
  const Binary &binary = context.binary;
  const Options &options = context.options;
  const size_t count = routing.routes.size();
  Findings findings;
  const auto range = [&](uint32_t id, float minimum, float maximum) {
    const Descriptor *d = binary.find(id);
    return d && d->minimum == minimum && d->maximum == maximum;
  };
  for (const RouteTarget &route : routing.routes) {
    if (route.mix && !range(*route.mix, 0.0f, 1.0f)) {
      findings.fail("routing mix parameter " + std::to_string(*route.mix) +
                    " must be a declared parameter ranging from 0 to 1");
    }
  }
  if (routing.selector) {
    const uint32_t id = *routing.selector;
    if (!range(id, 0.0f, static_cast<float>(count - 1))) {
      findings.fail("routingSelector parameter " + std::to_string(id) +
                    " must be a declared parameter ranging from 0 to " + std::to_string(count - 1));
    }
    const ParameterDisplay *display = binary.display(id);
    if (binary.metadata && display && display->described &&
        (display->kind == ParameterKind::continuous || display->steps != count)) {
      findings.fail("routingSelector parameter " + std::to_string(id) +
                    " must be described as a toggle, stepped or enumerated parameter with " +
                    std::to_string(count) + " values");
    }
    if (binary.bypass && id == *binary.bypass) {
      findings.fail("the routingSelector parameter cannot be the bypass parameter");
    }
  }
  if (!binary.parameters) {
    findings.fail("a routed module must export the parameter value extension");
  }
  if (findings.failed()) {
    findings.report(context, "routing", "");
    return;
  }
  const uint32_t max = options.max_frames;
  const size_t frames = 16384;
  size_t branches = 0;
  for (size_t index = 0; index < count; ++index) {
    const RouteTarget &route = routing.routes[index];
    if (!route.mix) {
      continue;
    }
    ++branches;
    const uint32_t mix = *route.mix;
    const std::string route_name = "route " + std::to_string(index) + " (" + route.placement + ")";
    findings.attempt(route_name, [&] {
      Instance instance(binary);
      if (routing.selector) {
        instance.configure({{*routing.selector, static_cast<float>(index)}});
      }
      const ParameterDisplay *display = binary.display(mix);
      for (float requested : {0.125f, 0.375f, 0.625f, 0.875f}) {
        instance.configure({{mix, requested}});
        const std::optional<float> stored = instance.value(mix);
        const float expected = display ? display->nearest(requested) : requested;
        if (!stored || *stored != expected) {
          findings.fail(route_name + ": mix parameter " + std::to_string(mix) + " set to " +
                        number(requested) + " stores " +
                        (stored ? number(*stored) : std::string("no value")) +
                        "; expected canonical value " + number(expected));
          break;
        }
      }
    });
    for (uint32_t channels : binary.channel_counts) {
      const std::string where = route_name + " " + layout_name(channels);
      findings.attempt(where, [&] {
        Random random(options.seed + 70 + index * 2 + channels);
        Settings base = random_settings(binary, random);
        if (routing.selector) {
          base = with_value(base, *routing.selector, static_cast<float>(index));
        }
        const Audio input = test_signal(channels, frames, check_rate, options.seed + 71);
        const auto render = [&](const Settings &settings, const std::vector<Automation> &changes) {
          Instance instance(binary);
          instance.configure(settings);
          instance.prepare(check_rate, channels, max);
          return instance.render(input, fixed_blocks(frames, std::min(ordinary_block, max)),
                                 changes);
        };
        const Audio reference = render(with_value(base, mix, 0.0f), {});
        const auto same = [&](const std::string &what, const Audio &output) {
          const Difference difference = compare(output, reference);
          if (!difference.identical) {
            findings.fail(where + ": the output changes with " + what + ": " +
                          describe(difference));
          }
        };
        for (float value : {0.5f, 1.0f}) {
          same("the mix parameter at " + number(value), render(with_value(base, mix, value), {}));
        }
        std::vector<Automation> changes{{1000, mix, 0.7f}, {5000, mix, 0.2f}, {9000, mix, 1.0f}};
        if (binary.bypass) {
          same("the bypass parameter off",
               render(with_value(with_value(base, mix, 0.0f), *binary.bypass, 0.0f), {}));
          changes.insert(changes.end(), {{3000, *binary.bypass, 0.0f},
                                         {7000, *binary.bypass, 1.0f},
                                         {12000, *binary.bypass, 0.0f}});
          std::stable_sort(
              changes.begin(), changes.end(),
              [](const Automation &a, const Automation &b) { return a.frame < b.frame; });
        }
        same("changes of the mix and bypass parameters",
             render(with_value(base, mix, 0.0f), changes));
      });
    }
  }
  std::string passed = std::to_string(count) + (count == 1 ? " route" : " routes");
  if (routing.selector) {
    const Descriptor *d = binary.find(*routing.selector);
    passed += " chosen by parameter " + std::to_string(*routing.selector) + " (default route " +
              std::to_string(route_index(d ? d->default_value : 0.0, count)) + ")";
  }
  passed += branches == 0
                ? "; no branch route"
                : "; in " + std::to_string(branches) +
                      (branches == 1 ? " branch route" : " branch routes") +
                      " mix values are stored canonically and the output is independent of the "
                      "mix and bypass parameters and their changes";
  findings.report(context, "routing", passed);
}

}
