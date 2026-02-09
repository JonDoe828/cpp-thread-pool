#pragma once
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
  static constexpr std::size_t kMaxThreads = 16;

public:
  using Task = std::function<void()>;

  explicit ThreadPool(std::size_t thread_count = 4) : stop_(false) {
    thread_count = std::clamp<std::size_t>(thread_count, 1, kMaxThreads);
    start_workers(thread_count);
  }

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto &t : workers_) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;

  void setExceptionHandler(std::function<void(std::exception_ptr)> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_)
      return; // 或 throw
    on_exception_ = std::move(handler);
  }

  // submit: 返回 future 的任务提交
  template <typename F, typename... Args>
  auto submit(F &&f, Args &&...args)
      -> std::future<std::invoke_result_t<F, Args...>> {
    using RetType = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<RetType()>>(
        [func = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(
                                        args)...)]() mutable -> RetType {
          return std::apply(std::move(func), std::move(tup));
        });

    std::future<RetType> fut = task->get_future();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) {
        throw std::runtime_error("submit on ThreadPool is stopped.");
      }
      tasks_.emplace([task]() { (*task)(); });
    }

    cv_.notify_one();
    return fut;
  }

  // post: fire-and-forget 提交
  template <typename F> bool post(F &&task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) {
        return false;
      }
      tasks_.emplace(std::forward<F>(task));
    }
    cv_.notify_one();
    return true;
  }

  int threadCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(workers_.size());
  }

private:
  void start_workers(std::size_t count) {
    while (workers_.size() < kMaxThreads && count-- > 0) {
      workers_.emplace_back([this] {
        for (;;) {
          Task task;

          {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

            if (stop_ && tasks_.empty()) {
              return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
          }

          // 执行任务（锁外）+ 异常保护
          try {
            task();
          } catch (...) {
            // 取 handler 的快照，避免在无锁状态下访问成员造成竞态
            std::function<void(std::exception_ptr)> handler;
            {
              std::lock_guard<std::mutex> lock(mutex_);
              handler = on_exception_;
            }
            if (handler)
              handler(std::current_exception());
            // 没 handler 就吞掉异常，保证 worker 不崩
          }
        }
      });
    }
  }

private:
  std::vector<std::thread> workers_;
  std::queue<Task> tasks_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_{false};

  std::function<void(std::exception_ptr)> on_exception_;
};

#endif // THREAD_POOL_H
