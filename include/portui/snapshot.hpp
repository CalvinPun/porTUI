#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace portui {

enum class Protocol {
  kTcp,
  kUdp,
  kUnknown,
};

enum class SocketState {
  kListen,
  kEstablished,
  kUnknown,
};

struct SocketEntry {
  int pid = -1;
  std::string process_name;
  std::uint16_t port = 0;
  Protocol protocol = Protocol::kUnknown;
  SocketState state = SocketState::kUnknown;
  std::uint32_t fd_count = 1;
};

struct Snapshot {
  std::chrono::system_clock::time_point captured_at;
  std::size_t scanned_process_count = 0;
  std::vector<SocketEntry> entries;
};

}  // namespace portui
