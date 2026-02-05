#pragma once
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// 线程池最大容量,应尽量设小一点
#define THREADPOOL_MAX_NUM 16
// 线程池是否可以自动增长(如果需要,且不超过 THREADPOOL_MAX_NUM)
// #define THREADPOOL_AUTO_GROW

class threadpool {
  unsigned short _initSize;
  using Task = std::function<void()>;

  std::vector<std::thread> _pool;
  std::queue<Task> _tasks;

  std::mutex _lock;
#ifdef THREADPOOL_AUTO_GROW
  std::mutex _lockGrow;
#endif
  std::condition_variable _task_cv;

  std::atomic<bool> _run{true};
  std::atomic<int> _idlThrNum{0};

public:
  explicit threadpool(unsigned short size = 4) : _initSize(size) {
    addThread(size);
  }

  ~threadpool() {
    _run = false;
    _task_cv.notify_all();
    for (std::thread &t : _pool) {
      if (t.joinable())
        t.join();
    }
  }

  template <class F, class... Args>
  auto commit(F &&f, Args &&...args) -> std::future<decltype(f(args...))> {
    if (!_run) {
      throw std::runtime_error("commit on ThreadPool is stopped.");
    }

    using RetType = decltype(f(args...));

    // packaged_task + future

    auto task = std::make_shared<std::packaged_task<RetType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<RetType> fut = task->get_future();
    {
      std::lock_guard<std::mutex> lock(_lock);
      _tasks.emplace([task]() { (*task)(); });
    }

#ifdef THREADPOOL_AUTO_GROW
    if (_idlThrNum < 1 && _pool.size() < THREADPOOL_MAX_NUM)
      addThread(1);
#endif
    _task_cv.notify_one();
    return fut;
  }

  template <class F> void commit2(F &&task) {
    if (!_run)
      return;
    {
      std::lock_guard<std::mutex> lock(_lock);
      _tasks.emplace(std::forward<F>(task));
    }
#ifdef THREADPOOL_AUTO_GROW
    if (_idlThrNum < 1 && _pool.size() < THREADPOOL_MAX_NUM)
      addThread(1);
#endif
    _task_cv.notify_one();
  }

  int idlCount() { return _idlThrNum; }
  int thrCount() { return static_cast<int>(_pool.size()); }

#ifndef THREADPOOL_AUTO_GROW
private:
#endif
  void addThread(unsigned short size) {
#ifdef THREADPOOL_AUTO_GROW
    if (!_run)
      throw std::runtime_error("Grow on ThreadPool is stopped.");
    std::unique_lock<std::mutex> lockGrow(_lockGrow);
#endif
    for (; _pool.size() < THREADPOOL_MAX_NUM && size > 0; --size) {
      _pool.emplace_back([this] {
        while (true) {
          Task task;
          {
            std::unique_lock<std::mutex> lock(_lock);
            _task_cv.wait(lock, [this] { return !_run || !_tasks.empty(); });
            if (!_run && _tasks.empty())
              return;

            _idlThrNum--;
            task = std::move(_tasks.front());
            _tasks.pop();
          }

          task();

#ifdef THREADPOOL_AUTO_GROW
          if (_idlThrNum > 0 && _pool.size() > _initSize)
            return;
#endif
          {
            std::unique_lock<std::mutex> lock(_lock);
            _idlThrNum++;
          }
        }
      });

      {
        std::unique_lock<std::mutex> lock(_lock);
        _idlThrNum++;
      }
    }
  }
};

#endif // THREAD_POOL_H
