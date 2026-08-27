#include "portui/snapshot_pipeline.hpp"

#include "../scanner/scanner_support.hpp"

#include <stdexcept>
#include <utility>

namespace portui {

SnapshotPipeline::SnapshotPipeline(std::unique_ptr<Scanner> scanner,
                                   std::chrono::milliseconds interval)
    : scanner_(std::move(scanner)), interval_(interval) {
  if (!scanner_) {
    throw std::invalid_argument("SnapshotPipeline requires a scanner");
  }
  if (interval_ <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("SnapshotPipeline interval must be positive");
  }
}

SnapshotPipeline::~SnapshotPipeline() {
  Stop();
}

void SnapshotPipeline::Start() {
  std::lock_guard lock(mutex_);
  if (running_) {
    return;
  }

  stop_requested_ = false;
  running_ = true;
  worker_ = std::thread(&SnapshotPipeline::Run, this);
}

void SnapshotPipeline::Stop() {
  std::thread worker;
  {
    std::lock_guard lock(mutex_);
    if (!running_) {
      return;
    }
    stop_requested_ = true;
    wakeup_.notify_one();
    worker = std::move(worker_);
  }
  worker.join();
  {
    std::lock_guard lock(mutex_);
    running_ = false;
  }
}

std::optional<SnapshotUpdate> SnapshotPipeline::Latest() const {
  std::lock_guard lock(mutex_);
  return latest_;
}

void SnapshotPipeline::Run() {
  std::optional<Snapshot> previous;
  while (true) {
    const auto scan_started_at = std::chrono::steady_clock::now();
    Snapshot snapshot = scanner_->Scan();
    detail::PopulateProcessUsage(&snapshot);
    snapshot.scan_duration = std::chrono::steady_clock::now() - scan_started_at;
    DiffResult diff;
    if (previous.has_value()) {
      diff = DiffSnapshots(*previous, snapshot);
    } else {
      diff.added = snapshot.entries;
    }

    {
      std::lock_guard lock(mutex_);
      if (stop_requested_) {
        return;
      }
      latest_ = SnapshotUpdate{.snapshot = snapshot, .diff = std::move(diff)};
    }
    previous = std::move(snapshot);

    std::unique_lock lock(mutex_);
    if (wakeup_.wait_for(lock, interval_, [this] { return stop_requested_; })) {
      return;
    }
  }
}

}  // namespace portui
