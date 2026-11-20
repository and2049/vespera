#ifndef VESPERA_GRAPH_LOADER_HPP
#define VESPERA_GRAPH_LOADER_HPP

#include <vespera/runtime.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace vespera {

class GraphLoader {
public:
  enum class State { idle, queued, preparing, waiting, ready, failed, cancelled, stopped };
  using Cancelled = std::function<bool()>;
  using Build = std::function<std::unique_ptr<Graph>(const Cancelled &)>;
  struct Snapshot {
    uint64_t generation = 0;
    State state = State::idle;
    std::vector<ModuleStatus> modules;
    std::string error;
  };
  struct Result {
    uint64_t generation = 0;
    std::unique_ptr<Graph> graph;
  };

  GraphLoader();
  ~GraphLoader();
  GraphLoader(const GraphLoader &) = delete;
  GraphLoader &operator=(const GraphLoader &) = delete;
  uint64_t submit(Build build);
  void cancel();
  Snapshot snapshot() const;
  std::optional<Result> take_ready();
  // Transfers a quiescent graph for worker destruction; false leaves it with the caller when
  // both retirement slots are occupied or the loader is stopped. Never call from process.
  bool retire(std::unique_ptr<Graph> &graph);
  // Refuse new work and cancel the current generation without joining the worker.
  void request_stop();
  // True once the worker has finished building and destroying graphs.
  bool stopped() const;
  // Requests stop and joins. An uncooperative module can delay shutdown.
  void shutdown();

private:
  struct Job {
    uint64_t generation;
    Build build;
  };
  void run();
  bool has_retired() const;
  void publish(uint64_t generation, State state, std::vector<ModuleStatus> modules = {},
               std::string error = {});

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::atomic<uint64_t> generation_{0};
  bool stopping_ = false;
  std::optional<Job> job_;
  std::optional<Result> ready_;
  std::array<std::unique_ptr<Graph>, 2> retired_;
  Snapshot snapshot_;
  std::mutex join_mutex_;
  std::thread worker_;
};

}
#endif
