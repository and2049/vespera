#include "harness.hpp"

#include "allocation.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>

namespace vespera::module_check {
namespace {

uint64_t allocations_in_process = 0;

std::string text(const char *value) { return value ? value : ""; }

template <class Entry> Entry symbol(void *library, const char *name) {
  return reinterpret_cast<Entry>(dlsym(library, name));
}

}

Binary::Binary(const std::filesystem::path &file) : path(file) {
  dlerror();
  void *library = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    const char *message = dlerror();
    unusable = "cannot load: " + text(message ? message : "dlopen failed");
    return;
  }
  const auto entry = symbol<vs_module_entry_fn>(library, VS_MODULE_ENTRY_NAME);
  entry_exported = entry != nullptr;
  if (!entry) {
    unusable = "does not export " VS_MODULE_ENTRY_NAME;
    return;
  }
  api = entry(VS_MODULE_ABI_VERSION);
  if (!api) {
    unusable = VS_MODULE_ENTRY_NAME " returns null for module ABI " +
               std::to_string(VS_MODULE_ABI_VERSION);
    return;
  }
  if (api->struct_size < sizeof(vs_module_api)) {
    unusable = "vs_module_api.struct_size " + std::to_string(api->struct_size) +
               " is smaller than " + std::to_string(sizeof(vs_module_api));
    return;
  }
  if (api->abi_version != VS_MODULE_ABI_VERSION) {
    unusable = "vs_module_api.abi_version is " + std::to_string(api->abi_version) + ", expected " +
               std::to_string(VS_MODULE_ABI_VERSION);
    return;
  }
  if (!api->create || !api->destroy || !api->prepare || !api->release || !api->reset ||
      !api->process) {
    unusable = "vs_module_api is missing required functions";
    return;
  }
  if (api->parameter_count > 1024 || (api->parameter_count != 0 && !api->parameters)) {
    unusable = "parameter table is null or longer than 1024 entries";
    return;
  }
  for (uint32_t i = 0; i < api->parameter_count; ++i) {
    const vs_parameter_descriptor &p = api->parameters[i];
    descriptors.push_back(
        {p.id, text(p.name), text(p.unit), p.minimum, p.maximum, p.default_value});
  }
  if (api->supported_layouts & VS_LAYOUT_MONO) {
    channel_counts.push_back(1);
  }
  if (api->supported_layouts & VS_LAYOUT_STEREO) {
    channel_counts.push_back(2);
  }
  if (api->capabilities & VS_MODULE_BYPASS_PARAMETER) {
    const Descriptor *declared = find(api->bypass_parameter);
    if (declared && declared->minimum == 0.0f && declared->maximum == 1.0f) {
      bypass = declared->id;
    }
  }

  if (const auto entry_state =
          symbol<vs_module_state_entry_fn>(library, VS_MODULE_STATE_ENTRY_NAME)) {
    state_exported = true;
    const vs_module_state_api *candidate = entry_state(VS_MODULE_STATE_ABI_VERSION);
    if (!candidate) {
      state_error = "state entry returns null for state ABI 1";
    } else if (candidate->struct_size < sizeof(vs_module_state_api) ||
               candidate->abi_version != VS_MODULE_STATE_ABI_VERSION) {
      state_error = "state descriptor has a short struct_size or another ABI version";
    } else if (candidate->schema_version == 0) {
      state_error = "state descriptor declares schema version 0";
    } else if (!candidate->size || !candidate->save || !candidate->load) {
      state_error = "state descriptor is missing functions";
    } else {
      state = candidate;
    }
  }
  if (const auto entry_metadata =
          symbol<vs_module_metadata_entry_fn>(library, VS_MODULE_METADATA_ENTRY_NAME)) {
    metadata_exported = true;
    metadata = entry_metadata(VS_MODULE_METADATA_ABI_VERSION);
    if (!metadata) {
      metadata_error = "parameter metadata entry returns null for metadata ABI " +
                       std::to_string(VS_MODULE_METADATA_ABI_VERSION);
    }
  }
  std::string reason;
  if (auto described = describe_parameters(*api, metadata, &reason)) {
    displays = std::move(*described);
  } else {
    metadata_error = reason;
    metadata = nullptr;
    displays = *describe_parameters(*api, nullptr, &reason);
  }
  if (const auto entry_services =
          symbol<vs_module_services_entry_fn>(library, VS_MODULE_SERVICES_ENTRY_NAME)) {
    services_exported = true;
    const vs_module_services_api *candidate = entry_services(VS_MODULE_SERVICES_ABI_VERSION);
    if (!candidate) {
      services_error = "services entry returns null for services ABI " +
                       std::to_string(VS_MODULE_SERVICES_ABI_VERSION);
    } else if (candidate->struct_size < sizeof(vs_module_services_api) ||
               candidate->abi_version != VS_MODULE_SERVICES_ABI_VERSION) {
      services_error = "services descriptor has a short struct_size or another ABI version";
    } else if (!candidate->attach || !candidate->status) {
      services_error = "services descriptor is missing functions";
    } else {
      services = candidate;
    }
  }
  if (const auto entry_files =
          symbol<vs_module_files_entry_fn>(library, VS_MODULE_FILES_ENTRY_NAME)) {
    const auto candidate = entry_files(VS_MODULE_FILES_ABI_VERSION);
    if (auto slots = describe_file_slots(candidate, &files_error)) {
      files = candidate;
      file_slots = std::move(*slots);
    }
  }
  if (const auto entry_parameters =
          symbol<vs_module_parameter_entry_fn>(library, VS_MODULE_PARAMETER_ENTRY_NAME)) {
    parameters_exported = true;
    const vs_module_parameter_api *candidate = entry_parameters(VS_MODULE_PARAMETER_ABI_VERSION);
    if (!candidate) {
      parameters_error = "parameter value entry returns null for parameter ABI 1";
    } else if (candidate->struct_size < sizeof(vs_module_parameter_api) ||
               candidate->abi_version != VS_MODULE_PARAMETER_ABI_VERSION) {
      parameters_error =
          "parameter value descriptor has a short struct_size or another ABI version";
    } else if (!candidate->get_value || !candidate->set_value) {
      parameters_error = "parameter value descriptor is missing functions";
    } else {
      parameters = candidate;
    }
  }
}

const ParameterDisplay *Binary::display(uint32_t id) const {
  const auto it = std::find_if(displays.begin(), displays.end(),
                               [id](const ParameterDisplay &d) { return d.id == id; });
  return it == displays.end() ? nullptr : &*it;
}

const Descriptor *Binary::find(uint32_t id) const {
  const auto it = std::find_if(descriptors.begin(), descriptors.end(),
                               [&](const Descriptor &d) { return d.id == id; });
  return it == descriptors.end() ? nullptr : &*it;
}

uint64_t Random::next() {
  state_ += 0x9e3779b97f4a7c15ull;
  uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

double Random::uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

uint32_t Random::below(uint32_t limit) {
  return limit == 0 ? 0 : static_cast<uint32_t>(next() % limit);
}

uint64_t process_allocations() { return allocations_in_process; }

ServicesHost::ServicesHost(const std::string &module, HostWorkMode mode) {
  HostServices::Options options;
  options.mode = mode;
  options.audit = true;
  services_ = HostServices::create(options);
  client_ = services_->client(nullptr, module, "");
  inner_ = client_->descriptor();
  table_ = *inner_;
  table_.context = this;
  table_.open_work = open_work;
  table_.request = request;
  table_.adopt = adopt;
  table_.retire = retire;
  table_.close_work = close_work;
  table_.notify = notify;
  table_.log = log;
}

ServicesHost::~ServicesHost() {
  release_hold();
  client_->close_all();
}

vs_work *ServicesHost::open_work(void *context, const vs_work_desc *desc) {
  auto &self = *static_cast<ServicesHost *>(context);
  if (!desc || desc->struct_size < sizeof(vs_work_desc) || !desc->run || !desc->dispose) {
    return self.inner_->open_work(self.inner_->context, desc);
  }
  Work *work = nullptr;
  {
    std::lock_guard lock(self.mutex_);
    work = &self.works_.emplace_back();
  }
  work->host = &self;
  work->desc = *desc;
  const vs_work_desc proxied{sizeof(vs_work_desc), work, run, dispose};
  work->handle = self.inner_->open_work(self.inner_->context, &proxied);
  return work->handle;
}

vs_result ServicesHost::request(void *context, vs_work *work, uint64_t token) {
  auto &self = *static_cast<ServicesHost *>(context);
  const bool paused = (self.inner_->flags & VS_HOST_WORK_INLINE) && allocation::pause();
  const auto result = self.inner_->request(self.inner_->context, work, token);
  if (paused) {
    allocation::resume();
  }
  return result;
}

int ServicesHost::adopt(void *context, vs_work *work, void **current, uint64_t *token) {
  auto &self = *static_cast<ServicesHost *>(context);
  self.hold_point();
  return self.inner_->adopt(self.inner_->context, work, current, token);
}

int ServicesHost::retire(void *context, vs_work *work, void *result) {
  auto &self = *static_cast<ServicesHost *>(context);
  self.hold_point();
  return self.inner_->retire(self.inner_->context, work, result);
}

void ServicesHost::close_work(void *context, vs_work *work, void *const *held, uint32_t count) {
  auto &self = *static_cast<ServicesHost *>(context);
  self.inner_->close_work(self.inner_->context, work, held, count);
  std::lock_guard lock(self.mutex_);
  for (Work &candidate : self.works_) {
    if (candidate.handle == work) {
      candidate.closed = true;
    }
  }
}

void ServicesHost::notify(void *context, uint32_t what) {
  auto &self = *static_cast<ServicesHost *>(context);
  self.inner_->notify(self.inner_->context, what);
}

void ServicesHost::log(void *context, uint32_t level, const char *message) {
  auto &self = *static_cast<ServicesHost *>(context);
  self.inner_->log(self.inner_->context, level, message);
}

vs_result ServicesHost::run(void *context, const vs_work_run *run, void **result) {
  Work &work = *static_cast<Work *>(context);
  ServicesHost &self = *work.host;
  if (work.closed) {
    ++self.after_close_;
  }
  {
    std::lock_guard lock(self.mutex_);
    self.running_ = &work;
    self.cancelled_at_.reset();
  }
  if (self.cancel_next_.exchange(false, std::memory_order_acq_rel)) {
    self.cancel_running();
  }
  const bool paused = allocation::pause();
  const vs_result status = work.desc.run(work.desc.context, run, result);
  if (paused) {
    allocation::resume();
  }
  const auto returned = std::chrono::steady_clock::now();
  std::lock_guard lock(self.mutex_);
  self.running_ = nullptr;
  if (self.cancelled_at_) {
    self.cancelled_returns_.push_back(
        std::chrono::duration<double>(returned - *self.cancelled_at_).count());
  }
  return status;
}

void ServicesHost::dispose(void *context, void *result) {
  Work &work = *static_cast<Work *>(context);
  if (work.closed) {
    ++work.host->after_close_;
  }
  const bool paused = allocation::pause();
  work.desc.dispose(work.desc.context, result);
  if (paused) {
    allocation::resume();
  }
}

bool ServicesHost::cancel_running() {
  vs_work *handle = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (!running_) {
      return false;
    }
    handle = running_->handle;
    cancelled_at_ = std::chrono::steady_clock::now();
  }
  HostServices::Client::Scope scope(*client_, HostServices::Client::Call::control);
  inner_->request(inner_->context, handle, 0);
  return true;
}

double ServicesHost::typical_cancelled_return() const {
  std::lock_guard lock(const_cast<std::mutex &>(mutex_));
  if (cancelled_returns_.empty()) {
    return 0.0;
  }
  std::vector<double> sorted = cancelled_returns_;
  std::sort(sorted.begin(), sorted.end());
  return sorted[sorted.size() / 2];
}

void ServicesHost::hold_processing() {
  std::lock_guard lock(mutex_);
  hold_ = true;
  held_ = false;
}

bool ServicesHost::wait_processing_held(std::chrono::steady_clock::time_point deadline) {
  std::unique_lock lock(mutex_);
  return changed_.wait_until(lock, deadline, [&] { return held_; });
}

void ServicesHost::release_hold() {
  std::lock_guard lock(mutex_);
  hold_ = false;
  changed_.notify_all();
}

void ServicesHost::hold_point() {
  if (!hold_.load(std::memory_order_acquire)) {
    return;
  }
  std::unique_lock lock(mutex_);
  if (!hold_) {
    return;
  }
  held_ = true;
  changed_.notify_all();
  changed_.wait(lock, [&] { return !hold_; });
}

Instance::Instance(const Binary &binary, HostWorkMode mode) : binary_(binary) {
  void *created = nullptr;
  const vs_result result = binary.api->create(&created);
  if (result != VS_OK || !created) {
    throw CheckError("create returned " + result_name(result) +
                     (result == VS_OK ? " with a null instance" : ""));
  }
  handle_ = created;
  if (binary.services) {
    services_ = std::make_unique<ServicesHost>(binary.api->id ? binary.api->id : "", mode);
    vs_result attached = VS_ERROR_PROCESS;
    {
      HostServices::Client::Scope attach(services_->client(), HostServices::Client::Call::attach);
      attached = binary.services->attach(handle_, services_->table());
    }
    if (attached != VS_OK) {
      binary.api->destroy(handle_);
      throw CheckError("attach returned " + result_name(attached));
    }
  }
}

Instance::~Instance() {
  if (prepared_) {
    const Call releasing(services_.get(), HostServices::Client::Call::release);
    binary_.api->release(handle_);
  }
  if (services_) {
    services_->client().close_all();
  }
  binary_.api->destroy(handle_);
}

std::optional<vs_module_status> Instance::status() const {
  if (!binary_.services) {
    return std::nullopt;
  }
  vs_module_status status{};
  status.struct_size = sizeof(status);
  if (binary_.services->status(handle_, &status) != VS_OK) {
    return std::nullopt;
  }
  return status;
}

vs_result Instance::try_prepare(const vs_prepare_info *info, vs_module_timing *timing) {
  const Call preparing(services_.get(), HostServices::Client::Call::prepare);
  const vs_result result = binary_.api->prepare(handle_, info, timing);
  if (result == VS_OK) {
    prepared_ = true;
    channels_ = info ? info->channels : 0;
    position_ = 0;
  }
  return result;
}

vs_module_timing Instance::prepare(double rate, uint32_t channels, uint32_t max_frames) {
  const vs_prepare_info info{sizeof(vs_prepare_info), rate, max_frames, channels};
  vs_module_timing timing{sizeof(vs_module_timing), 0, 0};
  const vs_result result = try_prepare(&info, &timing);
  if (result != VS_OK) {
    throw CheckError("prepare at " + number(rate) + " Hz, " + std::to_string(channels) +
                     " channel(s), " + std::to_string(max_frames) + " frames returned " +
                     result_name(result));
  }
  return timing;
}

void Instance::release() {
  {
    const Call releasing(services_.get(), HostServices::Client::Call::release);
    binary_.api->release(handle_);
  }
  if (services_) {
    services_->client().close_all();
  }
  prepared_ = false;
}

void Instance::reset() {
  const Call control(services_.get(), HostServices::Client::Call::control);
  binary_.api->reset(handle_);
}

void Instance::configure(const Settings &settings) {
  const Call control(services_.get(), HostServices::Client::Call::control);
  for (const auto &[id, value] : settings) {
    if (binary_.parameters) {
      const vs_result result = binary_.parameters->set_value(handle_, id, value);
      if (result != VS_OK) {
        throw CheckError("set_value(" + std::to_string(id) + ", " + number(value) + ") returned " +
                         result_name(result));
      }
    } else {
      pending_.push_back({0, id, value});
    }
  }
}

std::optional<float> Instance::value(uint32_t id) const {
  const Call control(services_.get(), HostServices::Client::Call::control);
  float result = 0.0f;
  if (!binary_.parameters || binary_.parameters->get_value(handle_, id, &result) != VS_OK) {
    return std::nullopt;
  }
  return result;
}

vs_result Instance::process(const vs_process_block &block) {
  const Call processing(services_.get(), HostServices::Client::Call::process);
  allocation::arm();
  const vs_result result = binary_.api->process(handle_, &block);
  allocations_in_process += allocation::disarm();
  return result;
}

Audio Instance::render(const Audio &input, const std::vector<uint32_t> &blocks,
                       const std::vector<Automation> &automation, uint32_t first_flags,
                       const std::function<void()> &after_block) {
  if (input.size() != channels_) {
    throw CheckError("internal: render input has the wrong channel count");
  }
  const size_t frames = input.empty() ? 0 : input[0].size();
  Audio output(channels_, std::vector<float>(frames));
  std::vector<Automation> events = std::move(pending_);
  pending_.clear();
  events.insert(events.end(), automation.begin(), automation.end());
  std::vector<vs_parameter_event> local;
  local.reserve(events.size());
  std::vector<const float *> inputs(channels_);
  std::vector<float *> outputs(channels_);
  size_t next = 0;
  size_t start = 0;
  for (size_t index = 0; index < blocks.size() && start < frames; ++index) {
    const uint32_t count = blocks[index];
    local.clear();
    while (next < events.size() && events[next].frame < start + count) {
      const Automation &event = events[next++];
      const uint64_t offset = event.frame > start ? event.frame - start : 0;
      local.push_back({event.id, static_cast<uint32_t>(offset), event.value});
    }
    for (uint32_t c = 0; c < channels_; ++c) {
      inputs[c] = input[c].data() + start;
      outputs[c] = output[c].data() + start;
    }
    vs_process_block block{};
    block.struct_size = sizeof(vs_process_block);
    block.inputs = inputs.data();
    block.outputs = outputs.data();
    block.channels = channels_;
    block.frames = count;
    block.frame_position = position_;
    block.flags = index == 0 ? first_flags : 0u;
    block.events = local.empty() ? nullptr : local.data();
    block.event_count = static_cast<uint32_t>(local.size());
    const vs_result result = process(block);
    if (result != VS_OK) {
      throw CheckError(
          "process returned " + result_name(result) + " for a block of " + std::to_string(count) +
          " frames at frame " + std::to_string(start) +
          (local.empty() ? "" : " with " + std::to_string(local.size()) + " event(s)"));
    }
    start += count;
    position_ += count;
    if (after_block) {
      after_block();
    }
  }
  return output;
}

std::vector<uint32_t> fixed_blocks(size_t frames, uint32_t size) {
  std::vector<uint32_t> blocks;
  for (size_t start = 0; start < frames; start += size) {
    blocks.push_back(static_cast<uint32_t>(std::min<size_t>(size, frames - start)));
  }
  return blocks;
}

std::vector<uint32_t> random_blocks(size_t frames, uint32_t maximum, Random &random) {
  std::vector<uint32_t> blocks;
  for (size_t start = 0; start < frames;) {
    const uint32_t size =
        static_cast<uint32_t>(std::min<size_t>(1 + random.below(maximum), frames - start));
    blocks.push_back(size);
    start += size;
  }
  return blocks;
}

std::vector<uint32_t> split_blocks(size_t frames, uint32_t size,
                                   const std::vector<Automation> &automation) {
  std::vector<uint32_t> blocks;
  size_t next = 0;
  for (size_t start = 0; start < frames;) {
    size_t end = std::min<size_t>(start + size, frames);
    while (next < automation.size() && automation[next].frame <= start) {
      ++next;
    }
    if (next < automation.size() && automation[next].frame < end) {
      end = static_cast<size_t>(automation[next].frame);
    }
    blocks.push_back(static_cast<uint32_t>(end - start));
    start = end;
  }
  return blocks;
}

Audio test_signal(uint32_t channels, size_t frames, double rate, uint64_t seed) {
  Audio audio(channels, std::vector<float>(frames));
  for (uint32_t c = 0; c < channels; ++c) {
    Random random(seed * 31 + c);
    double phase = 0.0;
    double partial = 0.0;
    const double glide =
        std::log(2000.0 / 80.0) / std::max<double>(1.0, static_cast<double>(frames));
    for (size_t i = 0; i < frames; ++i) {
      const double frequency = 80.0 * std::exp(glide * static_cast<double>(i)) * (1.0 + 0.01 * c);
      phase += 2.0 * std::numbers::pi * frequency / rate;
      partial += 2.0 * std::numbers::pi * 3.0 * frequency / rate;
      double sample =
          0.25 * std::sin(phase) + 0.08 * std::sin(partial) + 0.02 * (2.0 * random.uniform() - 1.0);
      if (i % 4001 == 17) {
        sample += 0.5;
      }
      audio[c][i] = static_cast<float>(sample);
    }
  }
  return audio;
}

Audio silence(uint32_t channels, size_t frames) {
  return Audio(channels, std::vector<float>(frames, 0.0f));
}

Difference compare(const Audio &a, const Audio &b, size_t from) {
  Difference result;
  if (a.size() != b.size()) {
    return {false, std::numeric_limits<double>::infinity(), 0, 0};
  }
  for (uint32_t c = 0; c < a.size(); ++c) {
    if (a[c].size() != b[c].size()) {
      return {false, std::numeric_limits<double>::infinity(), 0, c};
    }
    for (size_t i = from; i < a[c].size(); ++i) {
      uint32_t x = 0;
      uint32_t y = 0;
      std::memcpy(&x, &a[c][i], sizeof(x));
      std::memcpy(&y, &b[c][i], sizeof(y));
      if (x == y) {
        continue;
      }
      const double difference =
          std::isfinite(a[c][i]) && std::isfinite(b[c][i])
              ? std::abs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i]))
              : std::numeric_limits<double>::infinity();
      if (result.identical || difference > result.maximum) {
        result.maximum = difference;
        result.frame = i;
        result.channel = c;
      }
      result.identical = false;
    }
  }
  return result;
}

bool within(const Difference &difference, float tolerance) {
  return difference.identical ||
         (tolerance > 0.0f && difference.maximum <= static_cast<double>(tolerance));
}

std::string describe(const Difference &difference) {
  if (difference.identical) {
    return "bit-identical";
  }
  return "max difference " + number(difference.maximum) + " at frame " +
         std::to_string(difference.frame) + ", channel " + std::to_string(difference.channel);
}

bool finite(const Audio &audio, size_t *frame) {
  for (const auto &channel : audio) {
    for (size_t i = 0; i < channel.size(); ++i) {
      if (!std::isfinite(channel[i])) {
        if (frame) {
          *frame = i;
        }
        return false;
      }
    }
  }
  return true;
}

Settings random_settings(const Binary &binary, Random &random) {
  Settings settings;
  for (const Descriptor &d : binary.descriptors) {
    if (binary.bypass && d.id == *binary.bypass) {
      settings.emplace_back(d.id, 1.0f);
      continue;
    }
    if (binary.serial_route && d.id == binary.serial_route->parameter) {
      settings.emplace_back(d.id, binary.serial_route->value);
      continue;
    }
    const double value =
        d.minimum + random.uniform() * (static_cast<double>(d.maximum) - d.minimum);
    settings.emplace_back(d.id, std::clamp(static_cast<float>(value), d.minimum, d.maximum));
  }
  return settings;
}

Settings endpoint_settings(const Binary &binary, bool maximum) {
  Settings settings;
  for (const Descriptor &d : binary.descriptors) {
    const bool bypass = binary.bypass && d.id == *binary.bypass;
    if (binary.serial_route && d.id == binary.serial_route->parameter) {
      settings.emplace_back(d.id, binary.serial_route->value);
      continue;
    }
    settings.emplace_back(d.id, bypass ? 1.0f : maximum ? d.maximum : d.minimum);
  }
  return settings;
}

std::string result_name(vs_result result) {
  switch (result) {
  case VS_OK:
    return "VS_OK";
  case VS_ERROR_ARGUMENT:
    return "VS_ERROR_ARGUMENT";
  case VS_ERROR_STATE:
    return "VS_ERROR_STATE";
  case VS_ERROR_UNSUPPORTED:
    return "VS_ERROR_UNSUPPORTED";
  case VS_ERROR_PROCESS:
    return "VS_ERROR_PROCESS";
  default:
    return "result " + std::to_string(result);
  }
}

std::string range_text(float minimum, float maximum) {
  return "[" + number(minimum) + ", " + number(maximum) + "]";
}

std::string number(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return buffer;
}

}
