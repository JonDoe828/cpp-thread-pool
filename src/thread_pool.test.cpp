#include <catch2/catch_test_macros.hpp>

#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <thread>
#include <vector>
using namespace std::chrono_literals;

namespace {
bool wait_until(std::function<bool()> pred,
                std::chrono::steady_clock::time_point deadline,
                std::chrono::milliseconds step = 1ms) {
  while (!pred() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(step);
  }
  return pred();
}
} // namespace

TEST_CASE("threadpool executes submitted void tasks", "[threadpool]") {
  ThreadPool pool(4);

  std::atomic<int> counter{0};
  constexpr int N = 200;

  for (int i = 0; i < N; ++i) {
    pool.submit(
        [&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
  }

  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (counter.load(std::memory_order_relaxed) != N &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }

  REQUIRE(counter.load(std::memory_order_relaxed) == N);
}

TEST_CASE("threadpool commit returns future with correct value",
          "[threadpool]") {
  ThreadPool pool(2);

  auto f1 = pool.submit([] { return 42; });
  auto f2 = pool.submit([](int a, int b) { return a + b; }, 10, 32);

  REQUIRE(f1.get() == 42);
  REQUIRE(f2.get() == 42);
}

TEST_CASE("threadpool handles many futures correctly", "[threadpool]") {
  ThreadPool pool(4);

  constexpr int N = 100;
  std::vector<std::future<int>> futs;
  futs.reserve(N);

  for (int i = 0; i < N; ++i) {
    futs.emplace_back(pool.submit([i] {
      std::this_thread::sleep_for(1ms);
      return i * i;
    }));
  }

  long long sum = 0;
  for (int i = 0; i < N; ++i) {
    sum += futs[i].get();
  }

  const long long expected = 1LL * (N - 1) * N * (2LL * N - 1) / 6;
  REQUIRE(sum == expected);
}

TEST_CASE("threadpool destructor does not deadlock with pending tasks",
          "[threadpool]") {
  REQUIRE_NOTHROW([] {
    ThreadPool pool(4);
    for (int i = 0; i < 200; ++i) {
      pool.submit([] { std::this_thread::sleep_for(2ms); });
    }
  }());
}

TEST_CASE("threadpool clamps thread count to [1, kMaxThreads]",
          "[threadpool]") {
  // depends on your ThreadPool clamp + threadCount() locking
  ThreadPool p0(0);
  REQUIRE(p0.threadCount() >= 1);

  ThreadPool pHuge(1000000);
  REQUIRE(pHuge.threadCount() <= 16); // matches kMaxThreads in your header
}

TEST_CASE("threadpool submit propagates exceptions via future",
          "[threadpool]") {
  ThreadPool pool(2);

  auto fut = pool.submit([]() -> int { throw std::runtime_error("boom"); });

  REQUIRE_THROWS_AS(fut.get(), std::runtime_error);
}

TEST_CASE("threadpool post returns true while running", "[threadpool]") {
  ThreadPool pool(2);

  std::atomic<int> counter{0};
  REQUIRE(pool.post([&] { counter.fetch_add(1, std::memory_order_relaxed); }));
  REQUIRE(pool.post([&] { counter.fetch_add(1, std::memory_order_relaxed); }));

  auto deadline = std::chrono::steady_clock::now() + 1s;
  REQUIRE(wait_until(
      [&] { return counter.load(std::memory_order_relaxed) == 2; }, deadline));
}

TEST_CASE(
    "threadpool continues after exception in post task and handler is called",
    "[threadpool]") {
  ThreadPool pool(2);

  std::atomic<int> handler_called{0};
  pool.setExceptionHandler([&](std::exception_ptr ep) {
    (void)ep;
    handler_called.fetch_add(1, std::memory_order_relaxed);
  });

  // One post task throws
  REQUIRE(pool.post([] { throw std::runtime_error("post boom"); }));

  // Then submit tasks should still run (pool not terminated)
  auto fut = pool.submit([] { return 7; });

  REQUIRE(fut.get() == 7);

  // Handler should have been called at least once
  auto deadline = std::chrono::steady_clock::now() + 1s;
  REQUIRE(wait_until(
      [&] { return handler_called.load(std::memory_order_relaxed) >= 1; },
      deadline));
}

TEST_CASE("threadpool supports concurrent submissions from multiple threads",
          "[threadpool]") {
  ThreadPool pool(4);

  constexpr int submit_threads = 8;
  constexpr int per_thread = 200;
  std::atomic<int> counter{0};

  std::vector<std::thread> producers;
  producers.reserve(submit_threads);

  for (int t = 0; t < submit_threads; ++t) {
    producers.emplace_back([&] {
      for (int i = 0; i < per_thread; ++i) {
        pool.submit(
            [&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
      }
    });
  }

  for (auto &th : producers)
    th.join();

  const int expected = submit_threads * per_thread;
  auto deadline = std::chrono::steady_clock::now() + 3s;
  REQUIRE(wait_until(
      [&] { return counter.load(std::memory_order_relaxed) == expected; },
      deadline));
}

TEST_CASE("threadpool supports nested submit (task submits another task)",
          "[threadpool]") {
  ThreadPool pool(2);

  auto outer = pool.submit([&pool] {
    auto inner = pool.submit([] { return 123; });
    return inner.get(); // waits inside worker thread
  });

  REQUIRE(outer.get() == 123);
}

TEST_CASE("threadpool does not block submissions when tasks are long-running",
          "[threadpool]") {
  ThreadPool pool(2);

  // occupy one worker for a bit
  auto long_fut = pool.submit([] {
    std::this_thread::sleep_for(200ms);
    return 1;
  });

  // submit more tasks; should return futures quickly and execute eventually
  std::vector<std::future<int>> futs;
  for (int i = 0; i < 20; ++i) {
    futs.emplace_back(pool.submit([i] { return i; }));
  }

  REQUIRE(long_fut.get() == 1);
  for (int i = 0; i < 20; ++i) {
    REQUIRE(futs[i].get() == i);
  }
}

TEST_CASE(
    "threadpool drain shutdown: tasks submitted before destruction complete",
    "[threadpool]") {
  std::atomic<int> done{0};

  {
    ThreadPool pool(4);
    for (int i = 0; i < 300; ++i) {
      pool.post([&done] {
        std::this_thread::sleep_for(1ms);
        done.fetch_add(1, std::memory_order_relaxed);
      });
    }
    // leaving scope triggers destructor; drain should run all tasks
  }

  REQUIRE(done.load(std::memory_order_relaxed) == 300);
}
