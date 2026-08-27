#include "portui/scanner.hpp"

#include "scanner_support.hpp"

#include <memory>

namespace portui {
namespace {

class SerialScanner final : public Scanner {
 public:
  Snapshot Scan() override {
    Snapshot snapshot;
    snapshot.captured_at = std::chrono::system_clock::now();

    const std::vector<int> pids = detail::ListAllPids();
    snapshot.scanned_process_count = pids.size();
    for (int pid : pids) {
      std::vector<SocketEntry> pid_entries = detail::ScanPid(pid);
      snapshot.entries.insert(snapshot.entries.end(),
                              std::make_move_iterator(pid_entries.begin()),
                              std::make_move_iterator(pid_entries.end()));
    }
    detail::FinalizeSnapshot(&snapshot);
    detail::AppendLsofFallback(&snapshot);
    detail::FinalizeSnapshot(&snapshot);

    return snapshot;
  }
};

}  // namespace

std::unique_ptr<Scanner> CreateSerialScanner() {
  return std::make_unique<SerialScanner>();
}

}  // namespace portui
