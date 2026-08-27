#include "portui/app.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "portui/debug.hpp"
#include "portui/scanner.hpp"

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

}  // namespace

int RunApp(std::span<const std::string_view> args) {
  if (!args.empty() && args.front() == "--benchmark") {
    PrintBenchmark();
    return 0;
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

  std::cout << "porTUI phase 2 scaffold ready.\n";
  std::cout << "Run with --scan to print a serial socket snapshot.\n";
  std::cout << "Run with --parallel-scan or --benchmark to exercise concurrent scanning.\n";
  return 0;
}

}  // namespace portui
