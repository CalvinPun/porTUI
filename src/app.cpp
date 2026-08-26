#include "portui/app.hpp"

#include <iostream>

#include "portui/debug.hpp"
#include "portui/snapshot.hpp"

namespace portui {
namespace {

Snapshot BuildBootstrapSnapshot() {
  Snapshot snapshot{
      .captured_at = std::chrono::system_clock::now(),
      .entries = {
          SocketEntry{
              .pid = 101,
              .process_name = "bootstrap-listener",
              .port = 3000,
              .protocol = Protocol::kTcp,
              .state = SocketState::kListen,
          },
          SocketEntry{
              .pid = 202,
              .process_name = "bootstrap-client",
              .port = 8080,
              .protocol = Protocol::kTcp,
              .state = SocketState::kEstablished,
          },
      },
  };

  return snapshot;
}

}  // namespace

int RunApp(std::span<const std::string_view> args) {
  const Snapshot snapshot = BuildBootstrapSnapshot();

  if (!args.empty() && args.front() == "--debug-snapshot") {
    std::cout << FormatSnapshotForDebug(snapshot);
    return 0;
  }

  std::cout << "porTUI phase 0 scaffold ready.\n";
  std::cout << "Run with --debug-snapshot to inspect the bootstrap data model.\n";
  return 0;
}

}  // namespace portui
