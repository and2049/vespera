#include <vespera/graph_loader.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace vespera;
using Clock = std::chrono::steady_clock;
void check(bool value) {
  if (!value) {
    throw std::runtime_error("graph loader check failed");
  }
}
template <class Predicate> void eventually(Predicate predicate) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    check(Clock::now() < deadline);
    std::this_thread::yield();
  }
}
std::unique_ptr<Graph> prepared() {
  auto graph = std::make_unique<Graph>();
  graph->prepare({48000, 128, 1});
  return graph;
}
}

int main() {
  try {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, finish = false;
    std::atomic<bool> cancelled_on_stop{false}, queued_ran{false};
    const auto control_thread = std::this_thread::get_id();
    GraphLoader loader;
    const auto first = loader.submit([&](const GraphLoader::Cancelled &cancelled) {
      check(std::this_thread::get_id() != control_thread);
      {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        check(changed.wait_until(lock, Clock::now() + std::chrono::seconds(5),
                                 [&] { return finish; }));
      }
      check(cancelled());
      return prepared();
    });
    {
      std::unique_lock lock(mutex);
      check(changed.wait_until(lock, Clock::now() + std::chrono::seconds(5),
                               [&] { return entered; }));
    }
    const auto second = loader.submit([](const auto &) { return prepared(); });
    const auto latest = loader.submit([](const auto &) { return prepared(); });
    check(first < second && second < latest);
    check(loader.snapshot().state == GraphLoader::State::queued);
    {
      std::lock_guard lock(mutex);
      finish = true;
      changed.notify_all();
    }
    eventually([&] { return loader.snapshot().state == GraphLoader::State::ready; });
    auto result = loader.take_ready();
    check(result && result->generation == latest && result->graph->state() == GraphState::prepared);
    check(loader.retire(result->graph));
    loader.submit(
        [](const auto &) -> std::unique_ptr<Graph> { throw std::runtime_error("bad candidate"); });
    eventually([&] { return loader.snapshot().state == GraphLoader::State::failed; });
    check(loader.snapshot().error == "bad candidate" && !loader.take_ready());
    loader.submit([](const auto &) { return prepared(); });
    eventually([&] { return loader.snapshot().state == GraphLoader::State::ready; });
    loader.cancel();
    check(!loader.take_ready());
    check(loader.snapshot().state == GraphLoader::State::cancelled);
    entered = finish = false;
    loader.submit([&](const GraphLoader::Cancelled &cancelled) {
      {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return finish; }));
      }
      cancelled_on_stop = cancelled();
      return prepared();
    });
    {
      std::unique_lock lock(mutex);
      check(changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
    }
    loader.submit([&](const auto &) {
      queued_ran = true;
      return prepared();
    });
    loader.request_stop();
    loader.request_stop();
    check(!loader.stopped());
    bool refused = false;
    try {
      loader.submit([](const auto &) { return prepared(); });
    } catch (const std::logic_error &) {
      refused = true;
    }
    check(refused);
    {
      std::lock_guard lock(mutex);
      finish = true;
      changed.notify_all();
    }
    eventually([&] { return loader.stopped(); });
    loader.shutdown();
    loader.shutdown();
    check(cancelled_on_stop && !queued_ran && !loader.take_ready());
    check(loader.snapshot().state == GraphLoader::State::stopped);
    loader.cancel();
    check(loader.snapshot().state == GraphLoader::State::stopped);
    auto graph = prepared();
    check(!loader.retire(graph) && graph != nullptr);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
