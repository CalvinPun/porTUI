#include "portui/scanner.hpp"

#include "scanner_support.hpp"

#include <chrono>
#include <memory>

namespace portui {
namespace {

class SerialScanner final : public Scanner {
 public:
  Snapshot Scan() override {
    const auto scan_started_at = std::chrono::steady_clock::now();
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
    snapshot.scan_duration = std::chrono::steady_clock::now() - scan_started_at;
    fallback_cache_.AppendTo(&snapshot);
    detail::FinalizeSnapshot(&snapshot);

    return snapshot;
  }

 private:
  detail::LsofFallbackCache fallback_cache_;
};

}  // namespace

std::unique_ptr<Scanner> CreateSerialScanner() {
  return std::make_unique<SerialScanner>();
}

}  // namespace portui
