#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "portui/diff.hpp"
#include "portui/scanner.hpp"

namespace portui {

struct SnapshotUpdate {
  Snapshot snapshot;
  DiffResult diff;
};

// Owns a scanner thread that publishes completed snapshots without blocking readers.
class SnapshotPipeline {
 public:
  explicit SnapshotPipeline(
      std::unique_ptr<Scanner> scanner,
      std::chrono::milliseconds interval = std::chrono::seconds(1));
  ~SnapshotPipeline();

  SnapshotPipeline(const SnapshotPipeline&) = delete;
  SnapshotPipeline& operator=(const SnapshotPipeline&) = delete;

  void Start();
  void Stop();
  std::optional<SnapshotUpdate> Latest() const;

 private:
  void Run();

  std::unique_ptr<Scanner> scanner_;
  std::chrono::milliseconds interval_;
  mutable std::mutex mutex_;
  std::condition_variable wakeup_;
  std::thread worker_;
  bool running_ = false;
  bool stop_requested_ = false;
  std::optional<SnapshotUpdate> latest_;
};

DiffResult DiffSnapshots(const Snapshot& previous, const Snapshot& current);

}  // namespace portui
