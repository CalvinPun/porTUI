#include "portui/app.hpp"

#include <iostream>

#include "portui/debug.hpp"
#include "portui/scanner.hpp"

namespace portui {

int RunApp(std::span<const std::string_view> args) {
  auto scanner = CreateSerialScanner();

  if (!args.empty() &&
      (args.front() == "--debug-snapshot" || args.front() == "--scan")) {
    const Snapshot snapshot = scanner->Scan();
    std::cout << FormatSnapshotForDebug(snapshot);
    return 0;
  }

  std::cout << "porTUI phase 1 scaffold ready.\n";
  std::cout << "Run with --scan to print a serial socket snapshot.\n";
  return 0;
}

}  // namespace portui
