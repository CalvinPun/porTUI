#pragma once

#include <chrono>
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
};

struct Snapshot {
  std::chrono::system_clock::time_point captured_at;
  std::vector<SocketEntry> entries;
};

}  // namespace portui
