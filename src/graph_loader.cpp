#include <vespera/graph_loader.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace vespera {

GraphLoader::GraphLoader() : worker_([this] { run(); }) {}
GraphLoader::~GraphLoader() { shutdown(); }

bool GraphLoader::has_retired() const {
  return std::any_of(retired_.begin(), retired_.end(),
                     [](const auto &graph) { return bool(graph); });
}

uint64_t GraphLoader::submit(Build build) {
  if (!build) {
    throw std::invalid_argument("graph preparation needs a build function");
  }
  std::lock_guard lock(mutex_);
  if (stopping_) {
    throw std::logic_error("graph loader is stopped");
  }
  const auto previous = generation_.load(std::memory_order_relaxed);
  if (previous == std::numeric_limits<uint64_t>::max()) {
    throw std::overflow_error("graph preparation generation exhausted");
  }
  const auto generation = previous + 1;
  generation_.store(generation, std::memory_order_release);
  job_.emplace(Job{generation, std::move(build)});
  snapshot_ = {generation, State::queued, {}, {}};
  changed_.notify_all();
  return generation;
}

void GraphLoader::cancel() {
  std::lock_guard lock(mutex_);
  if (stopping_) {
    return;
  }
  generation_.fetch_add(1, std::memory_order_acq_rel);
  job_.reset();
  snapshot_ = {generation_.load(), State::cancelled, {}, {}};
  changed_.notify_all();
}

GraphLoader::Snapshot GraphLoader::snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

std::optional<GraphLoader::Result> GraphLoader::take_ready() {
  std::lock_guard lock(mutex_);
  if (ready_ && ready_->generation != generation_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  auto result = std::move(ready_);
  ready_.reset();
  return result;
}

bool GraphLoader::retire(std::unique_ptr<Graph> &graph) {
  std::lock_guard lock(mutex_);
  if (!graph) {
    return true;
  }
  if (stopping_) {
    return false;
  }
  for (auto &slot : retired_) {
    if (!slot) {
      slot = std::move(graph);
      changed_.notify_all();
      return true;
    }
  }
  return false;
}

void GraphLoader::publish(uint64_t generation, State state, std::vector<ModuleStatus> modules,
                          std::string error) {
  std::lock_guard lock(mutex_);
  if (generation == generation_.load(std::memory_order_acquire) && !stopping_) {
    snapshot_ = {generation, state, std::move(modules), std::move(error)};
  }
}

void GraphLoader::request_stop() {
  std::lock_guard lock(mutex_);
  if (stopping_) {
    return;
  }
  stopping_ = true;
  generation_.fetch_add(1, std::memory_order_acq_rel);
  job_.reset();
  changed_.notify_all();
}

bool GraphLoader::stopped() const {
  std::lock_guard lock(mutex_);
  return snapshot_.state == State::stopped;
}

void GraphLoader::shutdown() {
  request_stop();
  std::lock_guard lock(join_mutex_);
  if (worker_.joinable()) {
    worker_.join();
  }
}

void GraphLoader::run() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
  for (;;) {
    std::optional<Job> job;
    std::array<std::unique_ptr<Graph>, 2> retired;
    std::optional<Result> stale;
    bool stopping;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [&] {
        return stopping_ || job_ || has_retired() ||
               (ready_ && ready_->generation != generation_.load(std::memory_order_acquire));
      });
      stopping = stopping_;
      retired.swap(retired_);
      if (ready_ &&
          (stopping_ || ready_->generation != generation_.load(std::memory_order_acquire))) {
        stale = std::move(ready_);
        ready_.reset();
      }
      if (!stopping_ && job_) {
        job = std::move(job_);
        job_.reset();
      }
    }
    for (auto &graph : retired) {
      graph.reset();
    }
    stale.reset();
    if (stopping) {
      std::lock_guard lock(mutex_);
      snapshot_ = {generation_.load(std::memory_order_relaxed), State::stopped, {}, {}};
      return;
    }
    if (!job) {
      continue;
    }
    const auto generation = job->generation;
    const Cancelled cancelled = [this, generation] {
      return generation_.load(std::memory_order_acquire) != generation;
    };
    try {
      publish(generation, State::preparing);
      auto graph = job->build(cancelled);
      if (!graph || graph->state() != GraphState::prepared) {
        throw std::runtime_error("candidate graph was not prepared");
      }
      std::set<NodeId> timing_reprepared;
      uint32_t timing_preparations = 0;
      while (!cancelled()) {
        const bool idle =
            graph->wait_for_work(std::chrono::steady_clock::now() + std::chrono::milliseconds(10));
        bool timing = false;
        for (const auto &notice : graph->take_notices()) {
          if ((notice.what & VS_NOTIFY_TIMING) && timing_reprepared.insert(notice.node).second) {
            timing = true;
          }
        }
        if (timing) {
          if (++timing_preparations > max_preset_nodes) {
            throw std::runtime_error("Candidate timing did not settle");
          }
          publish(generation, State::preparing);
          auto next = job->build(cancelled);
          if (!next || next->state() != GraphState::prepared) {
            throw std::runtime_error("Timing candidate was not prepared");
          }
          graph.swap(next);
          continue;
        }
        std::vector<ModuleStatus> modules;
        bool busy = !idle, failed = false;
        for (const auto node : graph->node_ids()) {
          if (auto status = graph->module_status(node)) {
            busy |= status->kind == VS_STATUS_BUSY;
            failed |= status->kind == VS_STATUS_ERROR;
            if (status->kind != VS_STATUS_READY) {
              modules.push_back(std::move(*status));
            }
          }
        }
        if (failed && idle) {
          publish(generation, State::failed, std::move(modules),
                  "Candidate needs attention; previous audio is unchanged");
          break;
        }
        if (!busy) {
          std::lock_guard lock(mutex_);
          if (!cancelled() && !stopping_) {
            ready_.emplace(Result{generation, std::move(graph)});
            snapshot_ = {generation, State::ready, std::move(modules), {}};
          }
          break;
        }
        publish(generation, State::waiting, std::move(modules));
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(10), [&] { return cancelled(); });
      }
    } catch (const std::exception &error) {
      const std::string_view text(error.what(), strnlen(error.what(), 4096));
      publish(generation, State::failed, {},
              display_detail::valid_utf8(text) ? std::string(text)
                                               : "Candidate preparation failed");
    } catch (...) {
      publish(generation, State::failed, {}, "Candidate preparation failed");
    }
  }
}
}
