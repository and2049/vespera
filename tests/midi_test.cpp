#include "fake_audio_engine.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <random>
#include <source_location>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>
#include <vespera/audio_session.hpp>
#include <vespera/midi_input.hpp>

namespace {
void check(bool ok, const std::source_location where = std::source_location::current()) {
  if (!ok) {
    throw std::runtime_error("MIDI check failed at " + std::to_string(where.line()));
  }
}
#ifdef __APPLE__
// Reports CoreMIDI's status when client creation fails. A refused first client creation has been
// seen to persist for the rest of the process (OSStatus -304), so retrying here does not help.
OSStatus create_midi_client(CFStringRef name, MIDIClientRef &client) {
  const auto status = MIDIClientCreate(name, nullptr, nullptr, &client);
  if (status != noErr) {
    std::fprintf(stderr, "MIDIClientCreate failed with OSStatus %d\n", static_cast<int>(status));
  }
  return status;
}
#endif
}
int main(int argc, char **argv) {
  if (argc != 3) {
    return 1;
  }
  try {
    using namespace vespera;
    MidiDecoder decoder;
    std::vector<vs_midi_event> decoded;
    auto emit = [&](vs_midi_event event) { decoded.push_back(event); };
    for (int byte : {0x90, 60, 0xf8, 100, 62, 0, 0xc1, 4, 0xf0, 1, 2, 0xf7, 60, 100, 0x80, 60, 0}) {
      decoder.consume(static_cast<uint8_t>(byte), emit);
    }
    check(decoded.size() == 4 && decoded[0].data1 == 60 && decoded[1].data2 == 0 &&
          decoded[2].status == 0xc1 && decoded[2].data2 == 0 && decoded[3].status == 0x80);
    MidiQueue queue;
    const auto generation = queue.generation();
    check(queue.push(generation, 1, {0, 0x90, 60, 100, 0}, 1000000));
    check(queue.push(generation, 1, {0, 0x80, 60, 0, 0}, 2000000));
    bool panic = false;
    auto events = queue.collect(1000000, 48, 48000, panic);
    check(!panic && events.size() == 1 && events[0].midi.status == 0x90);
    events = queue.collect(2000000, 48, 48000, panic);
    check(events.size() == 1 && events[0].midi.status == 0x80);
    check(queue.push(generation, 1, {0, 0x90, 61, 100, 0}, 5000000));
    check(queue.push(generation, 1, {0, 0x90, 62, 100, 0}, 3000000));
    events = queue.collect(2000000, 192, 48000, panic);
    check(events.size() == 2 && events[0].midi.data1 == 62 && events[0].frame_offset() == 48 &&
          events[1].frame_offset() == 144);
    for (uint32_t i = 0; i < MidiQueue::capacity; ++i) {
      check(queue.push(generation, 1, {0, 0x90, 60, 100, 0}));
    }
    check(!queue.push(generation, 1, {0, 0x80, 60, 0, 0}));
    check(queue.collect(0, 128, 48000, panic).empty() && panic && queue.dropped() == 1);
    queue.reset();
    check(!queue.push(generation, 1, {0, 0x90, 60, 100, 0}));
    check(!queue.push(queue.generation(), 1, {0, 0x90, 255, 100, 0}));

    // A panic discards only what was pushed before it; later pushes arrive with the panic.
    auto note_on = [](uint8_t key) { return vs_midi_event{0, 0x90, key, 100, 0}; };
    auto keys = [](std::span<const Event> events) {
      std::vector<uint8_t> result;
      for (const auto &event : events) {
        result.push_back(event.midi.data1);
      }
      return result;
    };
    {
      MidiQueue boundary;
      const auto token = boundary.generation();
      check(boundary.push(token, 1, note_on(1)));
      boundary.panic();
      check(boundary.push(token, 2, note_on(2)));
      check(boundary.push(token, 1, note_on(3)));
      check(boundary.outstanding() == 3);
      const auto delivered = boundary.collect(0, 128, 48000, panic);
      check(panic && keys(delivered) == std::vector<uint8_t>{2, 3} && delivered[0].node == 2 &&
            boundary.outstanding() == 0 && boundary.dropped() == 0);
      check(boundary.push(token, 1, note_on(4)));
      check(keys(boundary.collect(0, 128, 48000, panic)) == std::vector<uint8_t>{4} && !panic);

      // Repeated panics keep the latest boundary; a panic with nothing after it delivers nothing.
      check(boundary.push(token, 1, note_on(5)));
      boundary.panic();
      check(boundary.push(token, 1, note_on(6)));
      boundary.panic();
      check(boundary.push(token, 1, note_on(7)));
      check(keys(boundary.collect(0, 128, 48000, panic)) == std::vector<uint8_t>{7} && panic);
      check(boundary.push(token, 1, note_on(8)));
      boundary.panic();
      boundary.panic();
      check(boundary.collect(0, 128, 48000, panic).empty() && panic);
      check(boundary.collect(0, 128, 48000, panic).empty() && !panic &&
            boundary.outstanding() == 0);

      // Events already deferred to a later block obey the same boundary, as do immediate ones
      // collected together with them.
      const uint64_t later = 10'000'000;
      check(boundary.push(token, 1, note_on(9), later));
      check(boundary.collect(0, 128, 48000, panic).empty() && boundary.outstanding() == 1);
      check(boundary.push(token, 1, note_on(10), later));
      boundary.panic();
      check(boundary.push(token, 1, note_on(11), later));
      check(boundary.push(token, 1, note_on(12)));
      check(keys(boundary.collect(0, 128, 48000, panic)) == std::vector<uint8_t>{12} && panic &&
            boundary.outstanding() == 1);
      check(keys(boundary.collect(later, 128, 48000, panic)) == std::vector<uint8_t>{11} &&
            !panic && boundary.outstanding() == 0);

      // Overflow across targets panics before the overflowing event: pushes stay refused until
      // the consumer runs, and the next push is delivered rather than discarded by that panic.
      for (uint32_t i = 0; i < MidiQueue::capacity; ++i) {
        check(boundary.push(token, 1 + i % 2, note_on(static_cast<uint8_t>(i % 128))));
      }
      check(!boundary.push(token, 1, note_on(13)) && !boundary.push(token, 2, note_on(13)));
      check(boundary.dropped() == 2);
      check(boundary.collect(0, 128, 48000, panic).empty() && panic && boundary.outstanding() == 0);
      check(boundary.push(token, 2, note_on(14)));
      check(keys(boundary.collect(0, 128, 48000, panic)) == std::vector<uint8_t>{14} && !panic);

      // Panics interleaved over several trips around the ring.
      for (uint32_t round = 0; round < 3 * MidiQueue::capacity; ++round) {
        const auto key = static_cast<uint8_t>(round % 128);
        check(boundary.push(token, 1, note_on(key)));
        if (round % 7 == 0) {
          boundary.panic();
          check(boundary.push(token, 1, note_on(static_cast<uint8_t>((key + 1) % 128))));
          check(keys(boundary.collect(0, 128, 48000, panic)) ==
                    std::vector<uint8_t>{static_cast<uint8_t>((key + 1) % 128)} &&
                panic);
        } else {
          check(keys(boundary.collect(0, 128, 48000, panic)) == std::vector<uint8_t>{key} &&
                !panic);
        }
      }

      // Concurrency: one producer pushes events tagged with the number of panics it requested
      // before each push (an overflowing push requests one itself) while the consumer collects.
      // An event pushed after a panic may only be delivered once that panic is, in the same
      // block or after it, so the panic can never cut off a note it should have preceded; an
      // event pushed before a panic is never delivered after that panic.
      {
        MidiQueue raced;
        const auto raced_token = raced.generation();
        std::atomic<bool> done{false};
        std::thread producer([&] {
          std::mt19937 random(0x50414e43);
          NodeId epoch = 0;
          for (uint32_t i = 0; i < 400000; ++i) {
            if (random() % 16 == 0) {
              raced.panic();
              ++epoch;
            } else if (!raced.push(raced_token, epoch + 1, note_on(60))) {
              ++epoch;
            }
          }
          done = true;
        });
        NodeId delivered_epoch = 0;
        bool flagged_since = false, ordered = true;
        uint64_t flags = 0, delivered_events = 0, late_events = 0;
        while (!done.load() || raced.outstanding()) {
          bool raced_panic = false;
          const auto batch = raced.collect(0, 128, 48000, raced_panic);
          if (raced_panic) {
            ++flags;
            flagged_since = true;
          }
          // flagged_since: a panic was delivered after the last event of delivered_epoch, here or
          // in an earlier block. Newer events need one; events of that epoch must not follow one.
          for (const auto &event : batch) {
            const NodeId epoch = event.node - 1;
            ++delivered_events;
            ordered = ordered && epoch >= delivered_epoch;
            if (epoch > delivered_epoch) {
              late_events += !flagged_since;
              delivered_epoch = epoch;
            } else if (flagged_since) {
              ++late_events;
            }
            flagged_since = false;
          }
        }
        producer.join();
        if (!ordered || late_events) {
          std::fprintf(stderr,
                       "panic ordering: ordered %d, %llu of %llu events on the wrong side\n",
                       ordered, static_cast<unsigned long long>(late_events),
                       static_cast<unsigned long long>(delivered_events));
        }
        check(ordered && late_events == 0 && flags > 0 && delivered_events > 0);
      }

      // A reset clears a requested panic; the new generation's events are delivered.
      boundary.panic();
      boundary.reset();
      check(boundary.push(boundary.generation(), 1, note_on(15)));
      check(keys(boundary.collect(0, 128, 48000, panic)) == std::vector<uint8_t>{15} && !panic);
    }

    std::atomic<uint32_t> producers{2};
    auto produce = [&] {
      for (uint32_t i = 0; i < 20000; ++i) {
        queue.push(queue.generation(), 1, {0, 0x90, static_cast<uint8_t>(i % 128), 100, 0});
      }
      producers.fetch_sub(1);
    };
    std::thread first(produce), second(produce);
    uint32_t batches = 0;
    while (producers.load()) {
      for (const auto &event : queue.collect(0, 128, 48000, panic)) {
        check(vs_midi_event_valid(&event.midi));
      }
      if (++batches % 17 == 0) {
        queue.reset();
      }
    }
    first.join();
    second.join();
    queue.collect(0, 128, 48000, panic);

#ifdef __APPLE__
    struct VirtualSource {
      MIDIClientRef client = 0;
      MIDIEndpointRef source = 0;
      VirtualSource() {
        check(create_midi_client(CFSTR("vespera test"), client) == noErr);
        check(MIDISourceCreate(client, CFSTR("vespera test source"), &source) == noErr);
      }
      ~VirtualSource() {
        MIDIEndpointDispose(source);
        MIDIClientDispose(client);
      }
    } virtual_source;
    SInt32 source_id = 0;
    check(MIDIObjectGetIntegerProperty(virtual_source.source, kMIDIPropertyUniqueID, &source_id) ==
          noErr);

    // The direct lookup agrees with the enumeration, including for sources that come and go.
    check(MidiInput::source_available(source_id));
    check(!MidiInput::source_available(0));
    for (const auto &listed : MidiInput::sources()) {
      check(MidiInput::source_available(listed.id));
    }
    auto source_listed = [](SInt32 id) {
      const auto listed = MidiInput::sources();
      return std::any_of(listed.begin(), listed.end(),
                         [id](const MidiSource &entry) { return entry.id == id; });
    };
    MIDIEndpointRef transient = 0;
    check(MIDISourceCreate(virtual_source.client, CFSTR("vespera transient source"), &transient) ==
          noErr);
    SInt32 transient_id = 0;
    check(MIDIObjectGetIntegerProperty(transient, kMIDIPropertyUniqueID, &transient_id) == noErr);
    check(MidiInput::source_available(transient_id) && source_listed(transient_id));
    check(MIDIObjectSetIntegerProperty(transient, kMIDIPropertyOffline, 1) == noErr);
    check(!MidiInput::source_available(transient_id));
    check(MIDIObjectSetIntegerProperty(transient, kMIDIPropertyOffline, 0) == noErr);
    check(MidiInput::source_available(transient_id));
    check(MIDIEndpointDispose(transient) == noErr);
    check(!MidiInput::source_available(transient_id) && !source_listed(transient_id));
    check(MIDISourceCreate(virtual_source.client, CFSTR("vespera returning source"), &transient) ==
          noErr);
    check(MIDIObjectSetIntegerProperty(transient, kMIDIPropertyUniqueID, transient_id) == noErr);
    check(MidiInput::source_available(transient_id) && source_listed(transient_id));
    check(MIDIEndpointDispose(transient) == noErr);
    MIDIEndpointRef destination = 0;
    check(MIDIDestinationCreate(
              virtual_source.client, CFSTR("vespera test destination"),
              [](const MIDIPacketList *, void *, void *) {}, nullptr, &destination) == noErr);
    SInt32 destination_id = 0;
    check(MIDIObjectGetIntegerProperty(destination, kMIDIPropertyUniqueID, &destination_id) ==
          noErr);
    check(!MidiInput::source_available(destination_id));
    check(MIDIEndpointDispose(destination) == noErr);

    MidiQueue device_queue;
    MidiInput device_input;
    device_input.connect(device_queue, 7, source_id, 2);
    auto send_notes = [&] {
      MIDIPacketList packets{};
      const uint8_t bytes[]{0x90, 60, 100, 0x91, 72, 100};
      check(MIDIPacketListAdd(&packets, sizeof(packets), MIDIPacketListInit(&packets),
                              mach_absolute_time(), sizeof(bytes), bytes) != nullptr);
      check(MIDIReceived(virtual_source.source, &packets) == noErr);
    };
    auto receive_note = [&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < deadline) {
        auto received = device_queue.collect(midi_clock_ns(), 128, 48000, panic);
        if (!received.empty()) {
          check(received.size() == 1 && received[0].node == 7 && received[0].midi.status == 0x91 &&
                received[0].midi.data1 == 72);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      check(false);
    };
    send_notes();
    receive_note();
    device_queue.reset();
    device_input.refresh_generation();
    send_notes();
    receive_note();
    device_input.disconnect();
    check(device_queue.collect(midi_clock_ns(), 128, 48000, panic).empty() && panic);
    device_input.connect(device_queue, 7, source_id, 2);
    send_notes();
    receive_note();
    device_input.disconnect();

    // Several targets each receive every message once, in target order. The panic the previous
    // disconnect requested is still unconsumed: notes the new connection queues before the
    // consumer next runs arrive with it instead of being discarded by it.
    const std::array<NodeId, 2> pair{9, 4};
    device_input.connect(device_queue, pair, source_id, 2);
    send_notes();
    {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (device_queue.outstanding() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      check(device_queue.outstanding() == 2);
      const auto received = device_queue.collect(midi_clock_ns(), 128, 48000, panic);
      check(panic && received.size() == 2 && received[0].node == 9 && received[1].node == 4);
      for (const auto &event : received) {
        check(event.midi.status == 0x91 && event.midi.data1 == 72);
      }
    }

    // The targets share the queue: a burst that fits once but not twice drops and panics.
    const auto dropped = device_queue.dropped();
    {
      std::vector<uint8_t> burst;
      for (uint32_t note = 0; note < MidiQueue::capacity / 2 + 1; ++note) {
        burst.insert(burst.end(), {0x91, static_cast<uint8_t>(note), 100});
      }
      std::vector<std::byte> storage(burst.size() + 1024);
      auto *packets = reinterpret_cast<MIDIPacketList *>(storage.data());
      check(MIDIPacketListAdd(packets, storage.size(), MIDIPacketListInit(packets),
                              mach_absolute_time(), burst.size(), burst.data()) != nullptr);
      check(MIDIReceived(virtual_source.source, packets) == noErr);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (device_queue.dropped() < dropped + 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    check(device_queue.dropped() == dropped + 2);
    check(device_queue.collect(midi_clock_ns(), 128, 48000, panic).empty() && panic);
    send_notes();
    {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      size_t received = 0;
      while (received < 2 && std::chrono::steady_clock::now() < deadline) {
        received += device_queue.collect(midi_clock_ns(), 128, 48000, panic).size();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      check(received == 2);
    }
    device_input.disconnect();
    check(device_queue.collect(midi_clock_ns(), 128, 48000, panic).empty() && panic);
    send_notes();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(device_queue.collect(midi_clock_ns(), 128, 48000, panic).empty() && !panic);

    // Eight targets is the fixed limit; invalid or repeated targets and an empty set are refused
    // or connect nothing.
    std::array<NodeId, MidiInput::max_targets + 1> many{1, 2, 3, 4, 5, 6, 7, 8, 9};
    device_input.connect(device_queue, std::span(many).first(MidiInput::max_targets), source_id, 0);
    device_input.disconnect();
    device_queue.collect(midi_clock_ns(), 128, 48000, panic);
    auto refused = [&](std::span<const NodeId> nodes) {
      try {
        device_input.connect(device_queue, nodes, source_id, 0);
      } catch (const std::invalid_argument &) {
        return true;
      }
      return false;
    };
    check(refused(many));
    const std::array<NodeId, 2> repeated{3, 3}, invalid{3, invalid_node_id};
    check(refused(repeated) && refused(invalid));
    device_input.connect(device_queue, std::span<const NodeId>(), source_id, 0);
    send_notes();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(device_queue.collect(midi_clock_ns(), 128, 48000, panic).empty());
#endif

    Graph graph;
    const auto node = graph.add_module(argv[1]);
    check(graph.nodes()[0].module.midi_input);
    graph.prepare({48000, 128, 1});
    std::array<float, 128> input{}, output{};
    const float *in[]{input.data()};
    float *out[]{output.data()};
    Block block{in, out, 1, 128};
    std::array<Event, 3> sequence{{{node, {1, 16, .5f}},
                                   {node, {}, Event::Kind::midi, {16, 0x90, 64, 100, 0}},
                                   {node, {}, Event::Kind::midi, {96, 0x80, 64, 0, 0}}}};
    check(graph.process(block, sequence) == VS_OK);
    for (uint32_t i = 0; i < 128; ++i) {
      check(output[i] == (i >= 16 && i < 96 ? .25f : 0));
    }
    sequence[1].midi.data1 = 128;
    check(graph.process(block, sequence) == VS_ERROR_ARGUMENT);
    sequence[1].midi.data1 = 64;
    sequence[2].midi.frame_offset = 128;
    check(graph.process(block, sequence) == VS_ERROR_ARGUMENT);
    sequence[2].midi.frame_offset = 96;
    Graph legacy;
    const auto legacy_node = legacy.add_module(argv[2]);
    legacy.prepare({48000, 128, 1});
    Event unsupported{legacy_node, {}, Event::Kind::midi, {0, 0x90, 60, 100, 0}};
    check(legacy.process(block, {&unsupported, 1}) == VS_ERROR_ARGUMENT);

    std::mt19937 random(0x4d494449);
    for (uint32_t trial = 0; trial < 10000; ++trial) {
      const uint32_t frames = 1 + random() % 128;
      block.frames = frames;
      Event event{node,
                  {},
                  Event::Kind::midi,
                  {random() % frames, static_cast<uint8_t>(0x80 + random() % 0x70),
                   static_cast<uint8_t>(random() % 128), 0, 0}};
      if ((event.midi.status & 0xe0) != 0xc0) {
        event.midi.data2 = static_cast<uint8_t>(random() % 128);
      }
      check(graph.process(block, {&event, 1}) == VS_OK);
      for (uint32_t i = 0; i < frames; ++i) {
        check(std::isfinite(output[i]));
      }
    }

    fake_ae::reset();
    AudioSession session;
    const auto live = session.graph().add_module(argv[1]);
    StreamConfig config;
    config.backend_id = "fake";
    config.device_id = "input-device";
    config.output_device_id = "output-device";
    config.output_mode = OutputMode::monitor;
    session.prepare(config);
    session.start();
    auto &midi = session.midi_input();
    const auto token = midi.generation();
    check(midi.push(token, live, {0, 0x90, 64, 100, 0}));
    std::array<float, 128> right{};
    check(fake_ae::invoke(input.data(), output.data(), right.data(), 128, 0) == AE_PROCESS_OK);
    check(output[127] == .5f);
    midi.panic();
    check(fake_ae::invoke(input.data(), output.data(), right.data(), 128, 128) == AE_PROCESS_OK);
    check(output[127] == 0);
    auto candidate = std::make_unique<Graph>();
    candidate->add_module(argv[1]);
    session.request_graph_replacement(std::move(candidate), 128);
    check(fake_ae::invoke(input.data(), output.data(), right.data(), 128, 256) == AE_PROCESS_OK);
    check(!midi.push(token, live, {0, 0x90, 64, 100, 0}));
    check(output[127] == 0);
    session.poll();
    check(session.close() == CloseResult::closed);
    std::puts("MIDI routing, scheduling, overflow, decoder, lifecycle and 10000 randomized blocks "
              "passed");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
