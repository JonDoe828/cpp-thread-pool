#include <catch2/catch_test_macros.hpp>

#include "thread_pool.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

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
