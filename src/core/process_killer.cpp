#include "portui/process_killer.hpp"

#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace portui {
namespace {

KillResult TerminateProcess(int pid) {
  if (pid == getpid()) {
    return {.pid = pid,
            .status = KillStatus::kError,
            .message = "refused to terminate porTUI itself"};
  }
  if (kill(pid, SIGTERM) == 0) {
    return {.pid = pid, .status = KillStatus::kSuccess, .message = "SIGTERM sent"};
  }

  switch (errno) {
    case EPERM:
      return {.pid = pid, .status = KillStatus::kPermissionDenied, .message = "permission denied"};
    case ESRCH:
      return {.pid = pid, .status = KillStatus::kAlreadyExited, .message = "already exited"};
    default:
      return {.pid = pid, .status = KillStatus::kError, .message = std::strerror(errno)};
  }
}

}  // namespace

ProcessKiller::ProcessKiller(std::size_t worker_count) {
  worker_count = std::max<std::size_t>(1, worker_count);
  workers_.reserve(worker_count);
  for (std::size_t index = 0; index < worker_count; ++index) {
    workers_.emplace_back(&ProcessKiller::RunWorker, this);
  }
}

ProcessKiller::~ProcessKiller() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  work_ready_.notify_all();
  for (std::thread& worker : workers_) {
    worker.join();
  }
}

void ProcessKiller::Dispatch(std::vector<int> pids) {
  pids.erase(std::remove_if(pids.begin(), pids.end(), [](int pid) { return pid <= 0; }), pids.end());
  std::sort(pids.begin(), pids.end());
  pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
  if (pids.empty()) {
    return;
  }

  {
    std::lock_guard lock(mutex_);
    pending_pids_.insert(pending_pids_.end(), pids.begin(), pids.end());
  }
  work_ready_.notify_all();
}

std::vector<KillResult> ProcessKiller::DrainResults() {
  std::lock_guard lock(mutex_);
  std::vector<KillResult> results;
  results.swap(results_);
  return results;
}

void ProcessKiller::RunWorker() {
  while (true) {
    int pid = -1;
    {
      std::unique_lock lock(mutex_);
      work_ready_.wait(lock, [this] { return stopping_ || !pending_pids_.empty(); });
      if (stopping_ && pending_pids_.empty()) {
        return;
      }
      pid = pending_pids_.back();
      pending_pids_.pop_back();
    }

    KillResult result = TerminateProcess(pid);
    {
      std::lock_guard lock(mutex_);
      results_.push_back(std::move(result));
    }
  }
}

}  // namespace portui
