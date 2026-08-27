#pragma once

#include <cstddef>
#include <vector>

#include "portui/snapshot.hpp"

namespace portui::detail {

std::vector<int> ListAllPids();
std::vector<SocketEntry> ScanPid(int pid);
void FinalizeSnapshot(Snapshot* snapshot);
void AppendLsofFallback(Snapshot* snapshot);

}  // namespace portui::detail
