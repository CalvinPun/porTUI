#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#include "portui/kill_result.hpp"

namespace portui {

// Dispatches SIGTERM requests on worker threads and stores their results for the UI.
class ProcessKiller {
 public:
  explicit ProcessKiller(std::size_t worker_count = 4);
  ~ProcessKiller();

  ProcessKiller(const ProcessKiller&) = delete;
  ProcessKiller& operator=(const ProcessKiller&) = delete;

  void Dispatch(std::vector<int> pids);
  std::vector<KillResult> DrainResults();

 private:
  void RunWorker();

  std::mutex mutex_;
  std::condition_variable work_ready_;
  bool stopping_ = false;
  std::vector<int> pending_pids_;
  std::vector<KillResult> results_;
  std::vector<std::thread> workers_;
};

}  // namespace portui
