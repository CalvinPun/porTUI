#include "portui/debug.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace portui {
namespace {

const char* ToString(Protocol protocol) {
  switch (protocol) {
    case Protocol::kTcp:
      return "TCP";
    case Protocol::kUdp:
      return "UDP";
    case Protocol::kUnknown:
      return "UNKNOWN";
  }

  return "UNKNOWN";
}

const char* ToString(SocketState state) {
  switch (state) {
    case SocketState::kListen:
      return "LISTEN";
    case SocketState::kEstablished:
      return "ESTABLISHED";
    case SocketState::kUnknown:
      return "UNKNOWN";
  }

  return "UNKNOWN";
}

}  // namespace

std::string FormatSnapshotForDebug(const Snapshot& snapshot) {
  std::ostringstream output;
  const std::time_t timestamp =
      std::chrono::system_clock::to_time_t(snapshot.captured_at);

  output << "snapshot captured_at="
         << std::put_time(std::localtime(&timestamp), "%Y-%m-%d %H:%M:%S")
         << " scanned_processes=" << snapshot.scanned_process_count
         << '\n';

  if (snapshot.entries.empty()) {
    output << "(no entries)\n";
    return output.str();
  }

  for (const SocketEntry& entry : snapshot.entries) {
    output << "pid=" << entry.pid << " process=" << entry.process_name
           << " port=" << entry.port
           << " protocol=" << ToString(entry.protocol)
           << " state=" << ToString(entry.state)
           << " fds=" << entry.fd_count << '\n';
  }

  return output.str();
}

}  // namespace portui
