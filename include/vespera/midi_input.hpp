#ifndef VESPERA_MIDI_INPUT_HPP
#define VESPERA_MIDI_INPUT_HPP

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <vespera/midi_queue.hpp>
#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#endif

namespace vespera {
class MidiDecoder {
public:
  void reset() noexcept {
    status_ = 0;
    count_ = 0;
    sysex_ = false;
  }
  template <class Emit> void consume(uint8_t byte, Emit emit) {
    if (byte >= 0xf8) {
      return;
    }
    if (byte & 0x80) {
      count_ = 0;
      if (byte >= 0xf0) {
        status_ = 0;
        sysex_ = byte == 0xf0;
      } else {
        status_ = byte;
        sysex_ = false;
      }
      return;
    }
    if (!status_ || sysex_) {
      return;
    }
    data_[count_++] = byte;
    const auto needed = (status_ & 0xe0) == 0xc0 ? 1 : 2;
    if (count_ == needed) {
      emit(vs_midi_event{0, status_, data_[0], needed == 2 ? data_[1] : uint8_t{0}, 0});
      count_ = 0;
    }
  }

private:
  uint8_t status_ = 0, count_ = 0;
  uint8_t data_[2]{};
  bool sysex_ = false;
};

struct MidiSource {
  int32_t id;
  std::string name;
};

// Delivers one device's channel messages to up to max_targets graph nodes. Each message is pushed
// once per target, in target order, into one queue whose capacity the targets share; a full queue
// panics as it does for a single target.
class MidiInput {
public:
  static constexpr size_t max_targets = 8;

  MidiInput() = default;
  MidiInput(const MidiInput &) = delete;
  MidiInput &operator=(const MidiInput &) = delete;
  ~MidiInput() {
    disconnect();
#ifdef __APPLE__
    if (port_) {
      MIDIPortDispose(port_);
    }
    if (client_) {
      MIDIClientDispose(client_);
    }
#endif
  }

  static std::vector<MidiSource> sources() {
    std::vector<MidiSource> result;
#ifdef __APPLE__
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
      const auto source = MIDIGetSource(i);
      SInt32 id = 0;
      CFStringRef name = nullptr;
      if (MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &id) == noErr &&
          MIDIObjectGetStringProperty(source, kMIDIPropertyDisplayName, &name) == noErr) {
        char text[512]{};
        CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8);
        CFRelease(name);
        result.push_back({id, text});
      }
    }
#endif
    return result;
  }

  // Whether `source_id` names a present source, found directly by its unique ID rather than by
  // enumerating sources. The endpoints of a device that has gone away stay findable by ID while
  // marked offline, so an offline source counts as unavailable.
  static bool source_available(int32_t source_id) {
#ifdef __APPLE__
    MIDIObjectRef object = 0;
    MIDIObjectType type;
    if (source_id == 0 || MIDIObjectFindByUniqueID(source_id, &object, &type) != noErr ||
        type != kMIDIObjectType_Source) {
      return false;
    }
    SInt32 offline = 0;
    return MIDIObjectGetIntegerProperty(object, kMIDIPropertyOffline, &offline) != noErr ||
           offline == 0;
#else
    (void)source_id;
    return false;
#endif
  }

  void refresh_generation() {
#ifdef __APPLE__
    std::lock_guard lock(mutex_);
    if (queue_ && generation_ != queue_->generation()) {
      generation_ = queue_->generation();
      decoder_.reset();
    }
#endif
  }

  void disconnect() {
#ifdef __APPLE__
    MIDIEndpointRef old;
    {
      std::lock_guard lock(mutex_);
      if (queue_) {
        queue_->panic();
      }
      queue_ = nullptr;
      node_count_ = 0;
      old = source_;
      source_ = 0;
      decoder_.reset();
    }
    if (port_ && old) {
      MIDIPortDisconnectSource(port_, old);
    }
#endif
  }

  void connect(MidiQueue &queue, NodeId node, int32_t source_id, uint32_t channel) {
    connect(queue,
            node == invalid_node_id ? std::span<const NodeId>() : std::span<const NodeId>(&node, 1),
            source_id, channel);
  }

  // Connects nothing when there is no source or no target.
  void connect(MidiQueue &queue, std::span<const NodeId> nodes, int32_t source_id,
               uint32_t channel) {
    disconnect();
    if (nodes.size() > max_targets) {
      throw std::invalid_argument("Too many MIDI targets");
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (nodes[i] == invalid_node_id ||
          std::find(nodes.begin(), nodes.begin() + static_cast<std::ptrdiff_t>(i), nodes[i]) !=
              nodes.begin() + static_cast<std::ptrdiff_t>(i)) {
        throw std::invalid_argument("Invalid or repeated MIDI target");
      }
    }
    if (source_id == 0 || nodes.empty()) {
      return;
    }
    if (channel > 16) {
      throw std::invalid_argument("Invalid MIDI channel");
    }
#ifdef __APPLE__
    MIDIObjectRef object = 0;
    MIDIObjectType type;
    if (MIDIObjectFindByUniqueID(source_id, &object, &type) != noErr ||
        type != kMIDIObjectType_Source) {
      throw std::runtime_error("MIDI source is unavailable");
    }
    if (!client_ && MIDIClientCreate(CFSTR("vespera"), nullptr, nullptr, &client_) != noErr) {
      throw std::runtime_error("Could not create MIDI client");
    }
    if (!port_ &&
        MIDIInputPortCreate(client_, CFSTR("vespera input"), receive, this, &port_) != noErr) {
      throw std::runtime_error("Could not create MIDI input");
    }
    mach_timebase_info(&timebase_);
    {
      std::lock_guard lock(mutex_);
      queue_ = &queue;
      std::copy(nodes.begin(), nodes.end(), nodes_.begin());
      node_count_ = nodes.size();
      generation_ = queue.generation();
      channel_ = channel;
      source_ = object;
    }
    if (MIDIPortConnectSource(port_, object,
                              reinterpret_cast<void *>(static_cast<uintptr_t>(object))) != noErr) {
      disconnect();
      throw std::runtime_error("Could not connect MIDI source");
    }
#else
    (void)queue;
    throw std::runtime_error("MIDI device input is unavailable on this platform");
#endif
  }

private:
#ifdef __APPLE__
  static void receive(const MIDIPacketList *list, void *context, void *source) {
    auto &self = *static_cast<MidiInput *>(context);
    std::lock_guard lock(self.mutex_);
    if (!self.queue_ || reinterpret_cast<uintptr_t>(source) != self.source_) {
      return;
    }
    const auto now_ns = midi_clock_ns();
    const auto now_ticks = mach_absolute_time();
    const MIDIPacket *packet = &list->packet[0];
    for (UInt32 p = 0; p < list->numPackets; ++p) {
      uint64_t timestamp = now_ns;
      if (packet->timeStamp) {
        const double delta =
            (static_cast<double>(packet->timeStamp) - static_cast<double>(now_ticks)) *
            self.timebase_.numer / self.timebase_.denom;
        timestamp = static_cast<uint64_t>(
            std::max(0.0, static_cast<double>(now_ns) + std::clamp(delta, -1e9, 1e9)));
      }
      for (UInt16 i = 0; i < packet->length; ++i) {
        self.decoder_.consume(packet->data[i], [&](vs_midi_event message) {
          if (!self.channel_ || (message.status & 15u) + 1 == self.channel_) {
            for (size_t t = 0; t < self.node_count_; ++t) {
              self.queue_->push(self.generation_, self.nodes_[t], message, timestamp);
            }
          }
        });
      }
      packet = MIDIPacketNext(packet);
    }
  }
  std::mutex mutex_;
  MIDIClientRef client_ = 0;
  MIDIPortRef port_ = 0;
  MIDIEndpointRef source_ = 0;
  mach_timebase_info_data_t timebase_{};
  MidiDecoder decoder_;
  MidiQueue *queue_ = nullptr;
  std::array<NodeId, max_targets> nodes_{};
  size_t node_count_ = 0;
  uint64_t generation_ = 0;
  uint32_t channel_ = 0;
#endif
};
}
#endif
