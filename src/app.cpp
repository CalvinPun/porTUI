#include "portui/app.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "portui/debug.hpp"
#include "portui/scanner.hpp"
#include "portui/snapshot_pipeline.hpp"
#include "portui/tui.hpp"

namespace portui {
namespace {

constexpr std::size_t kBenchmarkRuns = 5;

struct BenchmarkResult {
  double average_milliseconds = 0.0;
  std::size_t final_entry_count = 0;
};

BenchmarkResult Benchmark(Scanner* scanner) {
  using Clock = std::chrono::steady_clock;
  std::chrono::nanoseconds elapsed{};
  Snapshot snapshot;

  for (std::size_t run = 0; run < kBenchmarkRuns; ++run) {
    const Clock::time_point started_at = Clock::now();
    snapshot = scanner->Scan();
    elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started_at);
  }

  return {
      .average_milliseconds =
          std::chrono::duration<double, std::milli>(elapsed).count() / kBenchmarkRuns,
      .final_entry_count = snapshot.entries.size(),
  };
}

void PrintBenchmark() {
  auto serial_scanner = CreateSerialScanner();
  auto parallel_scanner = CreateParallelScanner();
  const BenchmarkResult serial = Benchmark(serial_scanner.get());
  const BenchmarkResult parallel = Benchmark(parallel_scanner.get());

  std::cout << "Benchmark: " << kBenchmarkRuns << " live scans per mode\n";
  std::cout << "serial:   " << serial.average_milliseconds << " ms average, "
            << serial.final_entry_count << " entries in final scan\n";
  std::cout << "parallel: " << parallel.average_milliseconds << " ms average, "
            << parallel.final_entry_count << " entries in final scan\n";
  if (parallel.average_milliseconds > 0.0) {
    std::cout << "speedup:  " << serial.average_milliseconds / parallel.average_milliseconds
              << "x\n";
  }
  std::cout << "Note: live scans may differ slightly as processes open or close sockets.\n";
}

void WatchSnapshots() {
  SnapshotPipeline pipeline(CreateParallelScanner());
  pipeline.Start();
  std::chrono::system_clock::time_point last_capture{};

  std::cout << "Watching for socket changes. Press Ctrl-C to stop.\n";
  while (true) {
    const std::optional<SnapshotUpdate> update = pipeline.Latest();
    if (update.has_value() && update->snapshot.captured_at != last_capture) {
      last_capture = update->snapshot.captured_at;
      std::cout << "added=" << update->diff.added.size()
                << " removed=" << update->diff.removed.size()
                << " changed=" << update->diff.changed.size() << '\n';
      std::cout << FormatSnapshotForDebug(update->snapshot);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

}  // namespace

int RunApp(std::span<const std::string_view> args) {
  if (!args.empty() && (args.front() == "--help" || args.front() == "-h")) {
    std::cout << "porTUI - live macOS port monitor\n\n";
    std::cout << "Usage: portui [option]\n\n";
    std::cout << "  --tui             Launch the interactive monitor (default)\n";
    std::cout << "  --scan            Print a serial socket snapshot\n";
    std::cout << "  --parallel-scan   Print a parallel socket snapshot\n";
    std::cout << "  --benchmark       Compare serial and parallel scans\n";
    std::cout << "  --watch           Print live snapshots and diffs\n";
    std::cout << "  --help, -h        Show this help\n";
    return 0;
  }
  if (!args.empty() && args.front() == "--benchmark") {
    PrintBenchmark();
    return 0;
  }

  if (!args.empty() && args.front() == "--watch") {
    WatchSnapshots();
    return 0;
  }

  if (args.empty() || args.front() == "--tui") {
    return RunTui();
  }

  if (!args.empty() &&
      (args.front() == "--debug-snapshot" || args.front() == "--scan" ||
       args.front() == "--parallel-scan")) {
    std::unique_ptr<Scanner> scanner = args.front() == "--parallel-scan"
                                           ? CreateParallelScanner()
                                           : CreateSerialScanner();
    const Snapshot snapshot = scanner->Scan();
    std::cout << FormatSnapshotForDebug(snapshot);
    return 0;
  }

  std::cout << "porTUI phase 4 scaffold ready.\n";
  std::cout << "Run with --scan to print a serial socket snapshot.\n";
  std::cout << "Run with --parallel-scan or --benchmark to exercise concurrent scanning.\n";
  std::cout << "Run with --watch to print background scan updates and diffs.\n";
  std::cout << "Run with --tui to launch the interactive port monitor.\n";
  return 0;
}

}  // namespace portui
