#include <vespera/sdk/main_thread.hpp>

#include <dispatch/dispatch.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <vector>

using vespera::sdk::MainThread;
using Result = MainThread::Result;
using namespace std::chrono_literals;

namespace {
void check(bool ok, std::source_location location = std::source_location::current()) {
  if (!ok) {
    std::fprintf(stderr, "main_thread test: line %u\n", location.line());
    std::abort();
  }
}

void pump_until(const std::atomic<bool> &done) {
  const auto limit = std::chrono::steady_clock::now() + 5s;
  while (!done && std::chrono::steady_clock::now() < limit) {
    MainThread::wait(.005);
  }
  check(done);
}

void nested_dispatch_test() {
  check(MainThread::on_main());
  std::atomic<bool> done = false;
  std::vector<int> order;
  std::thread worker([&] {
    check(MainThread::post([&] { order.push_back(1); }));
    check(MainThread::post([&] { order.push_back(2); }));
    check(MainThread::call(
              [&] {
                check(MainThread::on_main());
                order.push_back(3);
              },
              .5) == Result::done);
    done = true;
  });
  pump_until(done);
  worker.join();
  check(order == std::vector<int>({1, 2, 3}));
}

void running_test() {
  std::atomic<bool> done = false, entered = false;
  std::thread worker([&] {
    int stack_value = 0;
    check(MainThread::call(
              [&] {
                entered = true;
                std::this_thread::sleep_for(350ms);
                check(!done);
                stack_value = 9;
              },
              .2) == Result::done);
    check(stack_value == 9);
    done = true;
  });
  pump_until(done);
  worker.join();
  check(entered);
}
}

int main(int argc, char **argv) {
  check(argc == 3);
  check(MainThread::call([] { check(MainThread::on_main()); }, 0) == Result::done);
  {
    std::thread worker([] {
      // The lambda references a worker-local value that is dead when the queued entry is drained.
      int value = 0;
      check(MainThread::call([&] { check(++value == 0); }, .025) == Result::not_started);
    });
    worker.join();
  }
  MainThread::wait(.025);
  running_test();

  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  void *other = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
  check(library && other);
  auto post = reinterpret_cast<bool (*)(bool *)>(dlsym(other, "lifecycle_post"));
  auto call = reinterpret_cast<bool (*)(std::atomic<bool> *, std::atomic<bool> *)>(
      dlsym(library, "lifecycle_call"));
  auto acquired = reinterpret_cast<int (*)()>(dlsym(other, "lifecycle_acquired"));
  check(post && call && acquired);
  std::atomic<bool> done = false, ready = false;
  std::thread worker([&] { check(call(&ready, &done)); });
  ready.wait(false);
  std::this_thread::sleep_for(25ms);
  check(!done);
  check(acquired() == 1);
  pump_until(done);
  worker.join();
  bool posted = false;
  check(post(&posted) && !posted);
  // Inline work in another image can overtake a posted teardown. Both jobs take the
  // lifecycle lock, but taking it does not drain a peer image's queue.
  check(MainThread::call(
            [&] {
              auto acquired_inline =
                  reinterpret_cast<int (*)()>(dlsym(library, "lifecycle_acquired"));
              check(acquired_inline && acquired_inline() == 1 && !posted);
            },
            0) == Result::done);
  check(dlclose(library) == 0);
  check(dlclose(other) == 0);
  MainThread::wait(.025);
  check(posted);

  dispatch_async_f(dispatch_get_main_queue(), nullptr, [](void *) {
    nested_dispatch_test();
    std::exit(0);
  });
  CFRunLoopRun();
  return 1;
}
