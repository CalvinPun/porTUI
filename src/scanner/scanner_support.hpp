#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

#include "portui/snapshot.hpp"

namespace portui::detail {

std::vector<int> ListAllPids();
std::vector<SocketEntry> ScanPid(int pid);
void FinalizeSnapshot(Snapshot* snapshot);
void AppendLsofFallback(Snapshot* snapshot);

class LsofFallbackCache {
 public:
  void AppendTo(Snapshot* snapshot);

 private:
  std::vector<SocketEntry> entries_;
  std::chrono::steady_clock::time_point last_refresh_{};
};

void PopulateProcessUsage(Snapshot* snapshot);

}  // namespace portui::detail
