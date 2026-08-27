#include "scanner_support.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>

namespace portui::detail {
namespace {

Protocol ParseProtocol(std::string_view type) {
  if (type.starts_with("TCP")) {
    return Protocol::kTcp;
  }
  if (type.starts_with("UDP")) {
    return Protocol::kUdp;
  }
  return Protocol::kUnknown;
}

SocketState ParseState(std::string_view name) {
  if (name.find("(LISTEN)") != std::string_view::npos) {
    return SocketState::kListen;
  }
  if (name.find("(ESTABLISHED)") != std::string_view::npos) {
    return SocketState::kEstablished;
  }
  return SocketState::kUnknown;
}

std::string DecodeCommandName(std::string_view command) {
  std::string decoded;
  decoded.reserve(command.size());
  for (std::size_t index = 0; index < command.size(); ++index) {
    if (command[index] == '\\' && index + 3 < command.size() && command[index + 1] == 'x' &&
        std::isxdigit(static_cast<unsigned char>(command[index + 2])) != 0 &&
        std::isxdigit(static_cast<unsigned char>(command[index + 3])) != 0) {
      const std::string hex(command.substr(index + 2, 2));
      decoded.push_back(static_cast<char>(std::strtoul(hex.c_str(), nullptr, 16)));
      index += 3;
      continue;
    }
    decoded.push_back(command[index]);
  }
  return decoded;
}

std::uint16_t ParseLocalPort(std::string_view name) {
  const std::size_t connection_separator = name.find("->");
  const std::string_view local = name.substr(0, connection_separator);
  const std::size_t colon = local.rfind(':');
  if (colon == std::string_view::npos || colon + 1 == local.size()) {
    return 0;
  }

  const std::string_view port_text = local.substr(colon + 1);
  if (!std::all_of(port_text.begin(), port_text.end(), [](unsigned char character) {
        return std::isdigit(character) != 0;
      })) {
    return 0;
  }

  const unsigned long port = std::strtoul(std::string(port_text).c_str(), nullptr, 10);
  return port > 0 && port <= 65535 ? static_cast<std::uint16_t>(port) : 0;
}

bool HasSocket(const Snapshot& snapshot, const SocketEntry& candidate) {
  return std::any_of(snapshot.entries.begin(), snapshot.entries.end(),
                     [&candidate](const SocketEntry& entry) {
                       return entry.pid == candidate.pid && entry.port == candidate.port &&
                              entry.protocol == candidate.protocol;
                     });
}

}  // namespace

std::vector<SocketEntry> CollectLsofFallback() {
  constexpr char kCommand[] = "/usr/sbin/lsof -n -P -i";
  std::FILE* pipe = popen(kCommand, "r");
  if (pipe == nullptr) {
    return {};
  }

  Snapshot fallback;
  std::array<char, 4096> line_buffer{};
  bool is_header = true;
  while (std::fgets(line_buffer.data(), static_cast<int>(line_buffer.size()), pipe) != nullptr) {
    if (is_header) {
      is_header = false;
      continue;
    }

    std::istringstream line(line_buffer.data());
    std::string command;
    std::string pid_text;
    std::string user;
    std::string fd;
    std::string address_type;
    std::string device;
    std::string size;
    std::string protocol_text;
    if (!(line >> command >> pid_text >> user >> fd >> address_type >> device >> size >> protocol_text)) {
      continue;
    }

    std::string name;
    std::getline(line >> std::ws, name);
    const Protocol protocol = ParseProtocol(protocol_text);
    const std::uint16_t port = ParseLocalPort(name);
    if (protocol == Protocol::kUnknown || port == 0) {
      continue;
    }

    char* end = nullptr;
    const long pid = std::strtol(pid_text.c_str(), &end, 10);
    if (end == pid_text.c_str() || *end != '\0' || pid <= 0) {
      continue;
    }
    fallback.entries.push_back({
        .pid = static_cast<int>(pid),
        .process_name = DecodeCommandName(command),
        .port = port,
        .protocol = protocol,
        .state = ParseState(name),
    });
  }
  pclose(pipe);

  FinalizeSnapshot(&fallback);
  return fallback.entries;
}

void AppendFallbackEntries(Snapshot* snapshot, const std::vector<SocketEntry>& entries) {
  for (const SocketEntry& entry : entries) {
    if (!HasSocket(*snapshot, entry)) {
      snapshot->entries.push_back(entry);
    }
  }
}

void AppendLsofFallback(Snapshot* snapshot) {
  AppendFallbackEntries(snapshot, CollectLsofFallback());
}

void LsofFallbackCache::AppendTo(Snapshot* snapshot) {
  constexpr auto kRefreshInterval = std::chrono::seconds(5);
  const auto now = std::chrono::steady_clock::now();
  if (last_refresh_ == std::chrono::steady_clock::time_point{} ||
      now - last_refresh_ >= kRefreshInterval) {
    const auto refresh_started_at = now;
    entries_ = CollectLsofFallback();
    snapshot->lsof_refresh_duration = std::chrono::steady_clock::now() - refresh_started_at;
    snapshot->lsof_refreshed = true;
    last_refresh_ = std::chrono::steady_clock::now();
  }
  AppendFallbackEntries(snapshot, entries_);
}

}  // namespace portui::detail
