#include "portui/scanner.hpp"

#include "scanner_support.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace portui {
namespace {

constexpr std::size_t kMinWorkerCount = 4;
constexpr std::size_t kMaxWorkerCount = 8;

std::size_t DefaultWorkerCount() {
  const unsigned int hardware_workers = std::thread::hardware_concurrency();
  return std::clamp(static_cast<std::size_t>(hardware_workers == 0 ? kMinWorkerCount
                                                                    : hardware_workers),
                    kMinWorkerCount, kMaxWorkerCount);
}

class ParallelScanner final : public Scanner {
 public:
  explicit ParallelScanner(std::size_t worker_count)
      : worker_count_(std::clamp(worker_count == 0 ? DefaultWorkerCount() : worker_count,
                                 kMinWorkerCount, kMaxWorkerCount)) {
    workers_.reserve(worker_count_);
    for (std::size_t worker_index = 0; worker_index < worker_count_; ++worker_index) {
      workers_.emplace_back(&ParallelScanner::RunWorker, this, worker_index);
    }
  }

  ~ParallelScanner() override {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    work_ready_.notify_all();
    for (std::thread& worker : workers_) {
      worker.join();
    }
  }

  Snapshot Scan() override {
    Snapshot snapshot;
    snapshot.captured_at = std::chrono::system_clock::now();
    std::vector<int> pids = detail::ListAllPids();
    if (pids.empty()) {
      return snapshot;
    }

    {
      std::unique_lock lock(mutex_);
      scan_pids_ = std::move(pids);
      active_workers_ = std::min(worker_count_, scan_pids_.size());
      worker_entries_.assign(worker_count_, {});
      completed_workers_ = 0;
      ++scan_generation_;
      work_ready_.notify_all();
      work_completed_.wait(lock, [this] { return completed_workers_ == worker_count_; });
    }
    for (std::vector<SocketEntry>& entries : worker_entries_) {
      snapshot.entries.insert(snapshot.entries.end(), std::make_move_iterator(entries.begin()),
                              std::make_move_iterator(entries.end()));
    }
    detail::FinalizeSnapshot(&snapshot);
    return snapshot;
  }

 private:
  void RunWorker(std::size_t worker_index) {
    std::size_t seen_generation = 0;
    while (true) {
      std::unique_lock lock(mutex_);
      work_ready_.wait(lock, [this, seen_generation] {
        return stopping_ || scan_generation_ != seen_generation;
      });
      if (stopping_) {
        return;
      }

      seen_generation = scan_generation_;
      const std::size_t active_workers = active_workers_;
      lock.unlock();

      std::vector<SocketEntry> entries;
      if (worker_index < active_workers) {
        for (std::size_t pid_index = worker_index; pid_index < scan_pids_.size();
             pid_index += active_workers) {
          std::vector<SocketEntry> pid_entries = detail::ScanPid(scan_pids_[pid_index]);
          entries.insert(entries.end(), std::make_move_iterator(pid_entries.begin()),
                         std::make_move_iterator(pid_entries.end()));
        }
      }

      lock.lock();
      worker_entries_[worker_index] = std::move(entries);
      ++completed_workers_;
      if (completed_workers_ == worker_count_) {
        work_completed_.notify_one();
      }
    }
  }

  std::size_t worker_count_;
  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable work_ready_;
  std::condition_variable work_completed_;
  bool stopping_ = false;
  std::size_t scan_generation_ = 0;
  std::size_t active_workers_ = 0;
  std::size_t completed_workers_ = 0;
  std::vector<int> scan_pids_;
  std::vector<std::vector<SocketEntry>> worker_entries_;
};

}  // namespace

std::unique_ptr<Scanner> CreateParallelScanner(std::size_t worker_count) {
  return std::make_unique<ParallelScanner>(worker_count);
}

}  // namespace portui
