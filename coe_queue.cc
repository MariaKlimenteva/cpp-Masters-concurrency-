#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <syncstream>

using task_t = std::move_only_function<int()>;

template <typename F, typename... Args> 
auto create_task(F&& f, Args &&... args) {
  using result_type = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
  auto bound_task = [func = std::forward<F>(f), 
                    tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
    return std::apply(std::move(func), std::move(tup));
  };
  std::packaged_task<result_type()> p_task{std::move(bound_task)};
  auto fut = p_task.get_future();
  task_t t{[p_task = std::move(p_task)]() mutable {
    p_task();
    return 0;
  }};

  return std::make_pair(std::move(t), std::move(fut));
}

class ThreadSafeQueue {
private:
  std::queue<task_t> queue_;
  std::mutex mutex_;
  std::condition_variable cv_;

public:
  void push(task_t &&t) {
    {
      std::lock_guard<std::mutex> lk{mutex_};
      queue_.push(std::move(t));
    }
    cv_.notify_one();
  }

  task_t pop() {
    std::unique_lock lk{mutex_};
    cv_.wait(lk, [this] { return !queue_.empty(); });
    task_t res = std::move(queue_.front());
    queue_.pop();
    return res;
  }
};

void consumer_thread_func(ThreadSafeQueue &queue, std::mutex &cout_mutex) {
  try {
    for (;;) {
      task_t cur = queue.pop();
      int res = std::move(cur)();
      if (res == -1) {
        queue.push([] { return -1; });
        break;
      }
    }
  } catch (const std::exception &e) {
      std::lock_guard lk{cout_mutex};
      std::cerr << "\nException in thread: " << e.what() << "\n";
  }
}

int fn1(int x, int y, int z) {
  std::osyncstream{std::cout} << "a";
  return x + y + z;
}

double fn2(std::vector<int> v) {
  std::osyncstream{std::cout} << "a";
  return v.size() + 0.5;
}

void fn3() {
  std::osyncstream{std::cout} << "c"; 
}
void test_queue(ThreadSafeQueue& queue, int ntasks) {
  std::future<int> first_future;
  std::future<double> second_future;

  for (int jdx = 0; jdx < ntasks; ++jdx) {
    switch (jdx % 3) {
    case 0: {
      auto &&[t, f] = create_task(fn1, 1, 2, 3);
      queue.push(std::move(t));
      first_future = std::move(f);
      break;
    }
    case 1: {
      std::vector v{1, 2, 3};
      auto &&[t, f] = create_task(fn2, v);
      queue.push(std::move(t));
      second_future = std::move(f);
      break;
    }
    case 2: {
      auto &&[t, f] = create_task(fn3);
#if defined(IMPLY_ORDER)
      first_future.get();
      second_future.get();
#endif
      queue.push(std::move(t));
      break;
    }
    }
  }

  // put final task
  queue.push([] { return -1; });
}

int main() {
  int nthreads = 3;
  int ntasks = 200;
  ThreadSafeQueue queue;
  std::mutex cout_mutex;

  std::vector<std::thread> consumers;
  for (int i = 0; i < nthreads; ++i)
    consumers.emplace_back(consumer_thread_func, std::ref(queue), std::ref(cout_mutex));

  try {
    test_queue(queue, ntasks);

    for (int i = 0; i < nthreads; ++i)
      consumers[i].join();

    std::cout << "\nJoined\n";
    return 0;
  } catch (std::exception &e) {
    std::cout << "\n" << e.what() << "\n";
  } 
}
