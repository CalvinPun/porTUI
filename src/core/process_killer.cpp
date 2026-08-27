#include "portui/process_killer.hpp"

#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>

namespace portui {
namespace {

KillResult ErrorForErrno(int pid) {
  switch (errno) {
    case EPERM:
      return {.pid = pid, .status = KillStatus::kPermissionDenied, .message = "permission denied"};
    case ESRCH:
      return {.pid = pid, .status = KillStatus::kAlreadyExited, .message = "already exited"};
    default:
      return {.pid = pid, .status = KillStatus::kError, .message = std::strerror(errno)};
  }
}

KillResult TerminateProcess(int pid) {
  if (pid == getpid()) {
    return {.pid = pid,
            .status = KillStatus::kError,
            .message = "refused to terminate porTUI itself"};
  }
  if (kill(pid, SIGTERM) != 0) {
    return ErrorForErrno(pid);
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));
  if (kill(pid, 0) != 0) {
    if (errno == ESRCH) {
      return {.pid = pid, .status = KillStatus::kSuccess, .message = "terminated after SIGTERM"};
    }
    return ErrorForErrno(pid);
  }
  return {.pid = pid,
          .status = KillStatus::kStillRunning,
          .can_force_kill = true,
          .message = "still running after SIGTERM"};
}

KillResult ForceKillProcess(int pid) {
  if (pid == getpid()) {
    return {.pid = pid,
            .status = KillStatus::kError,
            .message = "refused to terminate porTUI itself"};
  }
  if (kill(pid, SIGKILL) != 0) {
    return ErrorForErrno(pid);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (kill(pid, 0) != 0) {
    if (errno == ESRCH) {
      return {.pid = pid, .status = KillStatus::kSuccess, .message = "terminated with SIGKILL"};
    }
    return ErrorForErrno(pid);
  }
  return {.pid = pid, .status = KillStatus::kStillRunning, .message = "still running after SIGKILL"};
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
    for (int pid : pids) {
      pending_requests_.push_back({.pid = pid, .force = false});
    }
  }
  work_ready_.notify_all();
}

void ProcessKiller::DispatchForce(std::vector<int> pids) {
  pids.erase(std::remove_if(pids.begin(), pids.end(), [](int pid) { return pid <= 0; }), pids.end());
  std::sort(pids.begin(), pids.end());
  pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
  if (pids.empty()) {
    return;
  }

  {
    std::lock_guard lock(mutex_);
    for (int pid : pids) {
      pending_requests_.push_back({.pid = pid, .force = true});
    }
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
    KillRequest request;
    {
      std::unique_lock lock(mutex_);
      work_ready_.wait(lock, [this] { return stopping_ || !pending_requests_.empty(); });
      if (stopping_ && pending_requests_.empty()) {
        return;
      }
      request = pending_requests_.back();
      pending_requests_.pop_back();
    }

    KillResult result = request.force ? ForceKillProcess(request.pid) : TerminateProcess(request.pid);
    {
      std::lock_guard lock(mutex_);
      results_.push_back(std::move(result));
    }
  }
}

}  // namespace portui
