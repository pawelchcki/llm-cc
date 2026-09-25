#ifndef LLM_CC_COMPARE_PARALLEL_H_
#define LLM_CC_COMPARE_PARALLEL_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace llmcc::compare {

// Runs `function(index)` for every index below `count` on at most
// `concurrency` threads and returns the results in index order, so callers
// see the same output at any concurrency. The first exception stops workers
// from taking further indices and is rethrown once all have joined.
template <typename Result>
std::vector<Result> BoundedMap(
    std::size_t count, std::size_t concurrency,
    const std::function<Result(std::size_t)>& function) {
  if (concurrency == 0) {
    throw std::invalid_argument("concurrency must be positive");
  }
  std::vector<std::optional<Result>> slots(count);
  std::atomic<std::size_t> next{0};
  std::atomic<bool> failed{false};
  std::exception_ptr failure;
  std::mutex failure_mutex;
  const auto work = [&] {
    while (!failed.load()) {
      const std::size_t index = next.fetch_add(1);
      if (index >= count) {
        return;
      }
      try {
        slots[index].emplace(function(index));
      } catch (...) {
        const std::scoped_lock lock(failure_mutex);
        if (!failure) {
          failure = std::current_exception();
        }
        failed.store(true);
        return;
      }
    }
  };
  const std::size_t threads = std::min(concurrency, count);
  if (threads <= 1) {
    work();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (std::size_t thread = 0; thread < threads; ++thread) {
      pool.emplace_back(work);
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
  std::vector<Result> results;
  results.reserve(count);
  // Without a failure every index ran, so every slot is filled.
  for (std::optional<Result>& slot : slots) {
    results.push_back(
        std::move(*slot));  // NOLINT(bugprone-unchecked-optional-access)
  }
  return results;
}

}  // namespace llmcc::compare

#endif  // LLM_CC_COMPARE_PARALLEL_H_
