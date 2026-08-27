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

struct ProcessUsage {
  int pid = -1;
  int parent_pid = -1;
  std::string process_name;
  double cpu_percent = 0.0;
  std::uint64_t resident_bytes = 0;
};

struct Snapshot {
  std::chrono::system_clock::time_point captured_at;
  std::size_t scanned_process_count = 0;
  std::chrono::nanoseconds scan_duration{};
  std::chrono::nanoseconds lsof_refresh_duration{};
  bool lsof_refreshed = false;
  std::uint64_t system_memory_bytes = 0;
  std::uint32_t logical_cpu_count = 1;
  std::vector<SocketEntry> entries;
  std::vector<ProcessUsage> process_usage;
};

}  // namespace portui
