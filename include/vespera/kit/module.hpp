#ifndef VESPERA_KIT_MODULE_HPP
#define VESPERA_KIT_MODULE_HPP

#include <vespera/dsp/common.hpp>
#include <vespera/dsp/utility.hpp>
#include <vespera/kit/audio.hpp>
#include <vespera/kit/bypass.hpp>
#include <vespera/kit/host.hpp>
#include <vespera/kit/parameters.hpp>
#include <vespera/kit/state.hpp>
#include <vespera/module.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

/*
 * kit::Module<Config> implements the module ABI for a parameter table and a processor.
 *
 * Config declares:
 *   static constexpr const char *id, *name, *version;
 *   static constexpr uint32_t layouts;            // VS_LAYOUT_MONO and/or VS_LAYOUT_STEREO
 *   static constexpr auto parameters = kit::parameters(...);
 *   using Processor = P;
 *   static constexpr kit::BypassPolicy bypass;     // optional, default exact
 *   static constexpr bool flush_subnormals;        // optional, default true
 *   static constexpr bool sanitise_input;          // optional, default true
 * and optionally the state declarations described in state.hpp.
 *
 * Processor is constructed while the module is prepared, from a kit::Setup (and the config's
 * Extra, when declared), optionally followed by a kit::Host & (see host.hpp), and destroyed on
 * release; construction may allocate and throw. It
 * declares latency and tail, each either as a static constexpr member or a const noexcept member
 * function evaluated once after construction:
 *   latency_frames (uint32_t), tail_frames (uint64_t; UINT64_MAX means unbounded)
 * and implements, all noexcept:
 *   void reset();                                   // clear audio history
 *   Frame process_frame(const Frame &);  or  void process(Block &);
 *   void set_parameter(size_t index, float value);  // optional; table index, every change
 *   void tempo(const vs_tempo_context *);           // optional; once per block, may be null
 *   void midi(const vs_midi_event &); void panic(); // optional, together
 * Declaring tempo or midi declares VS_MODULE_HOST_TEMPO or VS_MODULE_MIDI_INPUT.
 *
 * Parameter changes reach set_parameter at their frame, before MIDI events at the same frame,
 * and before that frame is processed. A block processor is called once per span between event
 * frames. Non-finite input samples arrive as zero unless the config sets sanitise_input to
 * false, which passes every input bit pattern through. Unless the config sets flush_subnormals to
 * false, subnormal arithmetic flushes to zero during processing; a module that must pass
 * subnormal samples through its arithmetic unchanged opts out.
 */

namespace vespera::kit {

inline constexpr uint32_t max_block_frames = 65536;
inline constexpr size_t max_parameters = 1024;

namespace detail {

template <class P>
concept frame_processor = requires(P &processor, const Frame &frame) {
  { processor.process_frame(frame) } noexcept -> std::same_as<Frame>;
};

template <class P>
concept block_processor = requires(P &processor, Block &block) {
  { processor.process(block) } noexcept;
};

template <class P>
concept static_latency = requires { typename std::integral_constant<uint32_t, P::latency_frames>; };

template <class P>
concept computed_latency = requires(const P &processor) {
  { processor.latency_frames() } noexcept -> std::convertible_to<uint32_t>;
};

template <class P>
concept static_tail = requires { typename std::integral_constant<uint64_t, P::tail_frames>; };

template <class P>
concept computed_tail = requires(const P &processor) {
  { processor.tail_frames() } noexcept -> std::convertible_to<uint64_t>;
};

template <class P>
concept parameter_receiver = requires(P &processor, size_t index, float value) {
  { processor.set_parameter(index, value) } noexcept;
};

template <class P>
concept tempo_reader = requires(P &processor, const vs_tempo_context *tempo) {
  { processor.tempo(tempo) } noexcept;
};

template <class P>
concept midi_receiver = requires(P &processor, const vs_midi_event &event) {
  { processor.midi(event) } noexcept;
};

template <class P>
concept panics = requires(P &processor) {
  { processor.panic() } noexcept;
};

template <class Config> constexpr BypassPolicy bypass_policy() {
  if constexpr (requires { Config::bypass; }) {
    return Config::bypass;
  } else {
    return BypassPolicy::exact;
  }
}

template <class P> constexpr uint32_t bypass_latency() {
  if constexpr (static_latency<P>) {
    return P::latency_frames;
  } else {
    return prepared_latency;
  }
}

struct NoBypass {};

struct MidiEvents {
  const uint8_t *data = nullptr;
  uint32_t count = 0;
  uint32_t size = 0;

  vs_midi_event operator[](uint32_t index) const noexcept {
    vs_midi_event event{};
    std::memcpy(&event, data + size_t{index} * size, sizeof(event));
    return event;
  }
};

constexpr bool covers(const vs_process_block &block, size_t end) noexcept {
  return block.struct_size >= end;
}

}

template <class Config> class Module {
  using Processor = typename Config::Processor;
  using Codec = StateCodec<Config>;
  using Extra = typename Codec::Extra;
  static constexpr auto &table = Config::parameters;
  static constexpr size_t count = table.size();
  using Values = std::array<float, count>;
  static constexpr auto file_table = [] {
    if constexpr (requires { Config::files.descriptors; }) {
      return Config::files.descriptors;
    } else {
      return std::array<vs_file_slot, 0>{};
    }
  }();

  static constexpr bool frame_form = detail::frame_processor<Processor>;
  static constexpr bool block_form = detail::block_processor<Processor>;
  static constexpr bool exact =
      table.has_bypass() && detail::bypass_policy<Config>() == BypassPolicy::exact;
  static constexpr bool reads_tempo = detail::tempo_reader<Processor>;
  static constexpr bool reads_midi = detail::midi_receiver<Processor>;
  static constexpr size_t bypass_index = table.bypass_index();
  static constexpr bool flushes_subnormals = [] {
    if constexpr (requires { Config::flush_subnormals; }) {
      return bool{Config::flush_subnormals};
    } else {
      return true;
    }
  }();
  static constexpr bool sanitises_input = [] {
    if constexpr (requires { Config::sanitise_input; }) {
      return bool{Config::sanitise_input};
    } else {
      return true;
    }
  }();

  static float admit(float value) noexcept {
    if constexpr (sanitises_input) {
      return std::isfinite(value) ? value : 0.0f;
    } else {
      return value;
    }
  }

  static_assert(frame_form != block_form,
                "Processor must implement exactly one of process_frame(const Frame &) noexcept "
                "and process(Block &) noexcept");
  static_assert(
      requires(Processor &processor) {
        { processor.reset() } noexcept;
      }, "Processor must implement reset() noexcept");
  static_assert(detail::static_latency<Processor> || detail::computed_latency<Processor>,
                "Processor must declare latency_frames");
  static_assert(detail::static_tail<Processor> || detail::computed_tail<Processor>,
                "Processor must declare tail_frames");
  static_assert(!reads_midi || detail::panics<Processor>,
                "a Processor receiving MIDI must implement panic() noexcept");
  static constexpr bool uses_host =
      Codec::has_extra ? std::constructible_from<Processor, const Setup &, const Extra &, Host &>
                       : std::constructible_from<Processor, const Setup &, Host &>;
  static_assert(uses_host || (Codec::has_extra
                                  ? std::constructible_from<Processor, const Setup &, const Extra &>
                                  : std::constructible_from<Processor, const Setup &>),
                "Processor must be constructible from const Setup & (and const Extra & when the "
                "config declares Extra), optionally followed by Host &");
  static_assert(Config::layouts != 0 &&
                    (Config::layouts & ~uint32_t{VS_LAYOUT_MONO | VS_LAYOUT_STEREO}) == 0,
                "layouts must combine VS_LAYOUT_MONO and VS_LAYOUT_STEREO");
  static_assert(table.has_bypass() || detail::bypass_policy<Config>() == BypassPolicy::exact,
                "a bypass policy needs a parameter declared with .bypass()");
  static_assert(count <= max_parameters, "modules declare at most 1024 parameters");

  struct Engine {
    Host host;
    Processor processor;
    [[no_unique_address]] std::conditional_t<
        exact, ExactBypass<detail::bypass_latency<Processor>()>, detail::NoBypass> bypass;
    std::array<std::vector<float>, max_channels> scratch;
    uint32_t channels;
    uint32_t max_frames;

    Engine(const Setup &setup, const Extra &extra, const Values &values, detail::Link &link)
        : host(link, setup), processor(construct(setup, extra, host)),
          bypass(make_bypass(setup, values, processor)), channels(setup.channels),
          max_frames(setup.max_frames) {
      if constexpr (block_form) {
        for (uint32_t c = 0; c < channels; ++c) {
          scratch[c].resize(max_frames);
        }
      }
    }

    uint32_t latency() const noexcept {
      if constexpr (detail::static_latency<Processor>) {
        return Processor::latency_frames;
      } else {
        return processor.latency_frames();
      }
    }

    uint64_t tail() const noexcept {
      if constexpr (detail::static_tail<Processor>) {
        return Processor::tail_frames;
      } else {
        return processor.tail_frames();
      }
    }

    void reset() noexcept {
      processor.reset();
      if constexpr (exact) {
        bypass.reset();
      }
    }

  private:
    static Processor construct(const Setup &setup, [[maybe_unused]] const Extra &extra,
                               [[maybe_unused]] Host &host) {
      if constexpr (Codec::has_extra && uses_host) {
        return Processor(setup, extra, host);
      } else if constexpr (Codec::has_extra) {
        return Processor(setup, extra);
      } else if constexpr (uses_host) {
        return Processor(setup, host);
      } else {
        return Processor(setup);
      }
    }

    static auto make_bypass(const Setup &setup, [[maybe_unused]] const Values &values,
                            [[maybe_unused]] const Processor &processor) {
      if constexpr (!exact) {
        return detail::NoBypass{};
      } else if constexpr (detail::static_latency<Processor>) {
        return ExactBypass<Processor::latency_frames>(setup.sample_rate, values[bypass_index]);
      } else {
        return ExactBypass<>(setup.sample_rate, values[bypass_index], processor.latency_frames());
      }
    }
  };

  enum class Lifecycle { created, prepared };

  struct Instance {
    Lifecycle lifecycle = Lifecycle::created;
    Values values = table.defaults();
    [[no_unique_address]] Extra extra{};
    // Declared before the engine, whose works use it until they close.
    detail::Link link;
    std::optional<Engine> engine;
  };

  struct Cursor {
    uint32_t parameter = 0;
    uint32_t midi = 0;
  };

  static Instance &self(void *instance) noexcept { return *static_cast<Instance *>(instance); }

  static void apply(Instance &instance, size_t index, float value) noexcept {
    instance.values[index] = value;
    if (!instance.engine) {
      return;
    }
    if constexpr (exact) {
      if (index == bypass_index) {
        instance.engine->bypass.set_enabled(value);
      }
    }
    if constexpr (detail::parameter_receiver<Processor>) {
      instance.engine->processor.set_parameter(index, value);
    }
  }

  static bool accepts(const vs_prepare_info &info) noexcept {
    const bool layout = (info.channels == 1 && (Config::layouts & VS_LAYOUT_MONO)) ||
                        (info.channels == 2 && (Config::layouts & VS_LAYOUT_STEREO));
    return layout && std::isfinite(info.sample_rate) &&
           info.sample_rate >= dsp::minimum_sample_rate &&
           info.sample_rate <= dsp::maximum_sample_rate && info.max_frames != 0 &&
           info.max_frames <= max_block_frames;
  }

  static bool valid_events(const vs_process_block &block) noexcept {
    for (uint32_t i = 0; i < block.event_count; ++i) {
      const auto &event = block.events[i];
      if (!table.accepts(table.index(event.parameter_id), event.value) ||
          event.frame_offset >= block.frames ||
          (i != 0 && event.frame_offset < block.events[i - 1].frame_offset)) {
        return false;
      }
    }
    return true;
  }

  static bool read_midi(const vs_process_block &block, detail::MidiEvents &midi) noexcept {
    if (!detail::covers(block, offsetof(vs_process_block, midi_event_size) + sizeof(uint32_t)) ||
        block.midi_event_count == 0) {
      return true;
    }
    if (!block.midi_events || block.midi_event_size < sizeof(vs_midi_event)) {
      return false;
    }
    midi = {static_cast<const uint8_t *>(block.midi_events), block.midi_event_count,
            block.midi_event_size};
    uint32_t previous = 0;
    for (uint32_t i = 0; i < midi.count; ++i) {
      const vs_midi_event event = midi[i];
      if (!vs_midi_event_valid(&event) || event.frame_offset >= block.frames ||
          event.frame_offset < previous) {
        return false;
      }
      previous = event.frame_offset;
    }
    return true;
  }

  // Applies the events at frame. Work they requested inline is adopted at that frame, so offline
  // output does not depend on how blocks are split.
  static void dispatch(Instance &instance, const vs_process_block &block,
                       [[maybe_unused]] const detail::MidiEvents &midi, Cursor &cursor,
                       uint32_t frame) noexcept {
    const Cursor before = cursor;
    while (cursor.parameter < block.event_count &&
           block.events[cursor.parameter].frame_offset == frame) {
      const auto &event = block.events[cursor.parameter++];
      apply(instance, table.index(event.parameter_id), event.value);
    }
    if constexpr (reads_midi) {
      while (cursor.midi < midi.count && midi[cursor.midi].frame_offset == frame) {
        instance.engine->processor.midi(midi[cursor.midi++]);
      }
    }
    if (instance.link.has_works() && instance.link.inline_work() &&
        (cursor.parameter != before.parameter || cursor.midi != before.midi)) {
      instance.link.adopt_all();
    }
  }

  static uint32_t next_event(const vs_process_block &block, const detail::MidiEvents &midi,
                             const Cursor &cursor) noexcept {
    uint32_t next = block.frames;
    if (cursor.parameter < block.event_count) {
      next = std::min(next, block.events[cursor.parameter].frame_offset);
    }
    if (cursor.midi < midi.count) {
      next = std::min(next, midi[cursor.midi].frame_offset);
    }
    return next;
  }

  static void run_frames(Instance &instance, const vs_process_block &block,
                         const detail::MidiEvents &midi) noexcept {
    Engine &engine = *instance.engine;
    Cursor cursor;
    for (uint32_t frame = 0; frame < block.frames; ++frame) {
      dispatch(instance, block, midi, cursor, frame);
      Frame input{};
      for (uint32_t c = 0; c < block.channels; ++c) {
        input[c] = admit(block.inputs[c][frame]);
      }
      Frame output = engine.processor.process_frame(input);
      if constexpr (exact) {
        output = engine.bypass.process(input, output);
      }
      for (uint32_t c = 0; c < block.channels; ++c) {
        block.outputs[c][frame] = output[c];
      }
      if (instance.link.has_works()) {
        instance.link.advance(1);
      }
    }
  }

  static void run_blocks(Instance &instance, const vs_process_block &block,
                         const detail::MidiEvents &midi) noexcept {
    Engine &engine = *instance.engine;
    for (uint32_t c = 0; c < block.channels; ++c) {
      std::transform(block.inputs[c], block.inputs[c] + block.frames, engine.scratch[c].begin(),
                     admit);
    }
    Cursor cursor;
    uint32_t start = 0;
    while (start < block.frames) {
      dispatch(instance, block, midi, cursor, start);
      const uint32_t end = next_event(block, midi, cursor);
      Block span;
      span.channels = block.channels;
      span.frames = end - start;
      for (uint32_t c = 0; c < block.channels; ++c) {
        span.inputs[c] = engine.scratch[c].data() + start;
        span.outputs[c] = block.outputs[c] + start;
      }
      engine.processor.process(span);
      if (instance.link.has_works()) {
        instance.link.advance(span.frames);
      }
      if constexpr (exact) {
        for (uint32_t frame = start; frame < end; ++frame) {
          Frame dry{};
          Frame wet{};
          for (uint32_t c = 0; c < block.channels; ++c) {
            dry[c] = engine.scratch[c][frame];
            wet[c] = block.outputs[c][frame];
          }
          const Frame output = engine.bypass.process(dry, wet);
          for (uint32_t c = 0; c < block.channels; ++c) {
            block.outputs[c][frame] = output[c];
          }
        }
      }
      start = end;
    }
  }

  static vs_result create(void **instance) {
    if (!instance) {
      return VS_ERROR_ARGUMENT;
    }
    *instance = nullptr;
    try {
      auto value = std::make_unique<Instance>();
      auto slots = describe_file_slots(&files_api);
      if (!slots) {
        return VS_ERROR_ARGUMENT;
      }
      value->link.declare_files(std::move(*slots));
      *instance = value.release();
      return VS_OK;
    } catch (...) {
      return VS_ERROR_PROCESS;
    }
  }

  static void destroy(void *instance) { delete static_cast<Instance *>(instance); }

  static vs_result prepare(void *instance, const vs_prepare_info *info, vs_module_timing *timing) {
    if (!instance || !info || !timing || info->struct_size < sizeof(*info) ||
        timing->struct_size < sizeof(*timing) || !accepts(*info)) {
      return VS_ERROR_ARGUMENT;
    }
    Instance &module = self(instance);
    if (module.lifecycle != Lifecycle::created) {
      return VS_ERROR_STATE;
    }
    try {
      module.engine.emplace(
          Setup{info->sample_rate, info->channels, info->max_frames, module.values}, module.extra,
          module.values, module.link);
    } catch (...) {
      module.engine.reset();
      return VS_ERROR_PROCESS;
    }
    timing->latency_frames = module.engine->latency();
    timing->tail_frames = module.engine->tail();
    module.lifecycle = Lifecycle::prepared;
    return VS_OK;
  }

  static void release(void *instance) {
    if (instance) {
      Instance &module = self(instance);
      module.engine.reset();
      module.lifecycle = Lifecycle::created;
    }
  }

  static void reset(void *instance) {
    if (instance && self(instance).engine) {
      self(instance).engine->reset();
    }
  }

  static vs_result process(void *instance, const vs_process_block *block) {
    if (!instance || !block || block->struct_size < VS_PROCESS_BLOCK_V1_SIZE) {
      return VS_ERROR_ARGUMENT;
    }
    Instance &module = self(instance);
    if (module.lifecycle != Lifecycle::prepared) {
      return VS_ERROR_STATE;
    }
    Engine &engine = *module.engine;
    if (block->channels != engine.channels || block->frames == 0 ||
        block->frames > engine.max_frames || !block->inputs || !block->outputs ||
        (block->event_count && !block->events)) {
      return VS_ERROR_ARGUMENT;
    }
    for (uint32_t c = 0; c < block->channels; ++c) {
      if (!block->inputs[c] || !block->outputs[c]) {
        return VS_ERROR_ARGUMENT;
      }
    }
    if (!valid_events(*block)) {
      return VS_ERROR_ARGUMENT;
    }
    if (module.link.needs_offline() && !(block->flags & VS_PROCESS_OFFLINE)) {
      return VS_ERROR_UNSUPPORTED;
    }
    detail::MidiEvents midi;
    if constexpr (reads_midi) {
      if (!read_midi(*block, midi)) {
        return VS_ERROR_ARGUMENT;
      }
    }
    [[maybe_unused]] const vs_tempo_context *tempo = nullptr;
    if constexpr (reads_tempo) {
      if (detail::covers(*block, offsetof(vs_process_block, tempo) + sizeof(block->tempo)) &&
          block->tempo) {
        if (block->tempo->struct_size < sizeof(vs_tempo_context)) {
          return VS_ERROR_ARGUMENT;
        }
        tempo = block->tempo;
      }
    }
    if constexpr (flushes_subnormals) {
      dsp::DenormalGuard denormals;
      render(module, *block, midi, tempo);
    } else {
      render(module, *block, midi, tempo);
    }
    return VS_OK;
  }

  static void render(Instance &module, const vs_process_block &block,
                     const detail::MidiEvents &midi,
                     [[maybe_unused]] const vs_tempo_context *tempo) noexcept {
    Engine &engine = *module.engine;
    if (module.link.has_works()) {
      module.link.adopt_all();
    }
    if (block.flags & VS_PROCESS_DISCONTINUITY) {
      engine.reset();
    }
    if constexpr (reads_midi) {
      if (block.flags & VS_PROCESS_MIDI_PANIC) {
        engine.processor.panic();
      }
    }
    if constexpr (reads_tempo) {
      engine.processor.tempo(tempo);
    }
    if constexpr (frame_form) {
      run_frames(module, block, midi);
    } else {
      run_blocks(module, block, midi);
    }
  }

  static vs_result get_value(void *instance, uint32_t id, float *value) {
    const size_t index = table.index(id);
    if (!instance || !value || index == count) {
      return VS_ERROR_ARGUMENT;
    }
    *value = self(instance).values[index];
    return VS_OK;
  }

  static vs_result set_value(void *instance, uint32_t id, float value) {
    const size_t index = table.index(id);
    if (!instance || !table.accepts(index, value)) {
      return VS_ERROR_ARGUMENT;
    }
    apply(self(instance), index, value);
    return VS_OK;
  }

  static vs_result state_size(void *instance, uint64_t *bytes) {
    if (!instance || !bytes) {
      return VS_ERROR_ARGUMENT;
    }
    *bytes = Codec::size(self(instance).extra);
    return VS_OK;
  }

  static vs_result save(void *instance, uint8_t *buffer, uint64_t capacity, uint64_t *written) {
    if (!instance || !written) {
      return VS_ERROR_ARGUMENT;
    }
    const Instance &module = self(instance);
    const uint64_t bytes = Codec::size(module.extra);
    if ((!buffer && bytes != 0) || capacity < bytes) {
      return VS_ERROR_ARGUMENT;
    }
    Codec::save(module.values, module.extra, buffer);
    *written = bytes;
    return VS_OK;
  }

  static vs_result load(void *instance, uint32_t schema, const uint8_t *buffer, uint64_t bytes) {
    if (!instance) {
      return VS_ERROR_ARGUMENT;
    }
    if (!Codec::supports(schema)) {
      return VS_ERROR_UNSUPPORTED;
    }
    if ((!buffer && bytes != 0) || !Codec::plausible(schema, bytes)) {
      return VS_ERROR_ARGUMENT;
    }
    Instance &module = self(instance);
    if (module.lifecycle != Lifecycle::created) {
      return VS_ERROR_STATE;
    }
    try {
      return Codec::load(schema, {buffer, static_cast<size_t>(bytes)}, module.values, module.extra);
    } catch (...) {
      return VS_ERROR_PROCESS;
    }
  }

  static vs_result attach(void *instance, const vs_host_services *services) {
    if (!instance) {
      return VS_ERROR_ARGUMENT;
    }
    Instance &module = self(instance);
    if (module.link.host() || module.lifecycle != Lifecycle::created) {
      return VS_ERROR_STATE;
    }
    return module.link.attach(services);
  }

  static vs_result status(void *instance, vs_module_status *status) {
    return instance ? self(instance).link.status(status) : VS_ERROR_ARGUMENT;
  }

  static vs_result set_file(void *instance, uint32_t slot, const vs_file_ref *file) {
    if (!instance || !file) {
      return VS_ERROR_ARGUMENT;
    }
    auto &module = self(instance);
    if (module.lifecycle != Lifecycle::created) {
      return VS_ERROR_STATE;
    }
    try {
      if (!valid_file_reference(*file, module.link.slot(slot).max_bytes)) {
        return VS_ERROR_ARGUMENT;
      }
      File value;
      value.state = file->state;
      value.path = file->path ? file->path : "";
      value.name = file->name ? file->name : "";
      value.bytes = file->bytes;
      std::copy(std::begin(file->sha256), std::end(file->sha256), value.sha256.begin());
      module.link.set_file(slot, std::move(value));
      return VS_OK;
    } catch (...) {
      return VS_ERROR_ARGUMENT;
    }
  }

public:
  static constexpr uint32_t capabilities = table.capabilities() |
                                           (reads_tempo ? uint32_t{VS_MODULE_HOST_TEMPO} : 0u) |
                                           (reads_midi ? uint32_t{VS_MODULE_MIDI_INPUT} : 0u);

  static constexpr vs_module_api api = {sizeof(vs_module_api),
                                        VS_MODULE_ABI_VERSION,
                                        Config::id,
                                        Config::name,
                                        Config::version,
                                        Config::layouts,
                                        table.descriptors.data(),
                                        static_cast<uint32_t>(count),
                                        create,
                                        destroy,
                                        prepare,
                                        release,
                                        reset,
                                        process,
                                        capabilities,
                                        table.bypass_parameter()};

  static constexpr vs_module_state_api state_api = {sizeof(vs_module_state_api),
                                                    VS_MODULE_STATE_ABI_VERSION,
                                                    Codec::schema,
                                                    state_size,
                                                    save,
                                                    load};

  static constexpr vs_module_parameter_api parameter_api = {
      sizeof(vs_module_parameter_api), VS_MODULE_PARAMETER_ABI_VERSION, get_value, set_value};

  static constexpr vs_module_metadata_api metadata_api = {
      sizeof(vs_module_metadata_api), VS_MODULE_METADATA_ABI_VERSION, table.metadata.data(),
      static_cast<uint32_t>(count), sizeof(vs_parameter_metadata)};

  static constexpr vs_module_services_api services_api = {
      sizeof(vs_module_services_api), VS_MODULE_SERVICES_ABI_VERSION, attach, status};
  static constexpr vs_module_files_api files_api = {
      sizeof(vs_module_files_api), VS_MODULE_FILES_ABI_VERSION,
      file_table.data(),           static_cast<uint32_t>(file_table.size()),
      sizeof(vs_file_slot),        set_file};
  static constexpr bool has_files = !file_table.empty();
};

}

#define VESPERA_KIT_MODULE(Config)                                                                 \
  extern "C" VS_MODULE_EXPORT const vs_module_api *vespera_module_entry(uint32_t version) {        \
    return version == VS_MODULE_ABI_VERSION ? &::vespera::kit::Module<Config>::api : nullptr;      \
  }                                                                                                \
  extern "C" VS_MODULE_EXPORT const vs_module_state_api *vespera_module_state_entry(               \
      uint32_t version) {                                                                          \
    return version == VS_MODULE_STATE_ABI_VERSION ? &::vespera::kit::Module<Config>::state_api     \
                                                  : nullptr;                                       \
  }                                                                                                \
  extern "C" VS_MODULE_EXPORT const vs_module_parameter_api *vespera_module_parameter_entry(       \
      uint32_t version) {                                                                          \
    return version == VS_MODULE_PARAMETER_ABI_VERSION                                              \
               ? &::vespera::kit::Module<Config>::parameter_api                                    \
               : nullptr;                                                                          \
  }                                                                                                \
  extern "C" VS_MODULE_EXPORT const vs_module_metadata_api *vespera_module_metadata_entry(         \
      uint32_t version) {                                                                          \
    return version == VS_MODULE_METADATA_ABI_VERSION                                               \
               ? &::vespera::kit::Module<Config>::metadata_api                                     \
               : nullptr;                                                                          \
  }                                                                                                \
  extern "C" VS_MODULE_EXPORT const vs_module_services_api *vespera_module_services_entry(         \
      uint32_t version) {                                                                          \
    return version == VS_MODULE_SERVICES_ABI_VERSION                                               \
               ? &::vespera::kit::Module<Config>::services_api                                     \
               : nullptr;                                                                          \
  }                                                                                                \
  extern "C" VS_MODULE_EXPORT const vs_module_files_api *vespera_module_files_entry(               \
      uint32_t version) {                                                                          \
    return version == VS_MODULE_FILES_ABI_VERSION && ::vespera::kit::Module<Config>::has_files     \
               ? &::vespera::kit::Module<Config>::files_api                                        \
               : nullptr;                                                                          \
  }

#endif
