#include "scanner_support.hpp"

#include <arpa/inet.h>
#include <libproc.h>
#include <sys/proc_info.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace portui::detail {
namespace {

constexpr std::size_t kPidBatchSize = 4096;
constexpr std::size_t kFallbackFdCapacity = 64;
constexpr std::size_t kMaxPidCapacity = 1U << 20;
constexpr std::size_t kMaxFdCapacity = 1U << 20;

Protocol MapProtocol(const socket_info& socket) {
  switch (socket.soi_protocol) {
    case IPPROTO_TCP:
      return Protocol::kTcp;
    case IPPROTO_UDP:
      return Protocol::kUdp;
    default:
      return Protocol::kUnknown;
  }
}

SocketState MapSocketState(const socket_fdinfo& socket_fd) {
  if (socket_fd.psi.soi_kind != SOCKINFO_TCP) {
    return SocketState::kUnknown;
  }

  switch (socket_fd.psi.soi_proto.pri_tcp.tcpsi_state) {
    case TSI_S_LISTEN:
      return SocketState::kListen;
    case TSI_S_ESTABLISHED:
      return SocketState::kEstablished;
    default:
      return SocketState::kUnknown;
  }
}

std::uint16_t ExtractLocalPort(const socket_fdinfo& socket_fd) {
  if (socket_fd.psi.soi_kind == SOCKINFO_TCP) {
    return static_cast<std::uint16_t>(
        ntohs(static_cast<std::uint16_t>(socket_fd.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport)));
  }

  if (socket_fd.psi.soi_kind == SOCKINFO_IN) {
    return static_cast<std::uint16_t>(
        ntohs(static_cast<std::uint16_t>(socket_fd.psi.soi_proto.pri_in.insi_lport)));
  }

  return 0;
}

bool IsInetSocket(const socket_fdinfo& socket_fd) {
  return socket_fd.psi.soi_family == AF_INET || socket_fd.psi.soi_family == AF_INET6;
}

bool SameSocket(const SocketEntry& lhs, const SocketEntry& rhs) {
  return lhs.pid == rhs.pid && lhs.process_name == rhs.process_name &&
         lhs.port == rhs.port && lhs.protocol == rhs.protocol;
}

int StatePriority(SocketState state) {
  switch (state) {
    case SocketState::kListen:
      return 2;
    case SocketState::kEstablished:
      return 1;
    case SocketState::kUnknown:
      return 0;
  }
  return 0;
}

std::string ResolveProcessName(int pid, const proc_bsdinfo& bsd_info) {
  if (bsd_info.pbi_name[0] != '\0') {
    return bsd_info.pbi_name;
  }
  if (bsd_info.pbi_comm[0] != '\0') {
    return bsd_info.pbi_comm;
  }

  std::array<char, 2 * MAXCOMLEN> name_buffer{};
  if (proc_name(pid, name_buffer.data(), name_buffer.size()) > 0) {
    return name_buffer.data();
  }
  return "<unknown>";
}

std::vector<proc_fdinfo> ListProcessFds(int pid, std::size_t hint) {
  std::size_t capacity = std::min(std::max(hint, kFallbackFdCapacity), kMaxFdCapacity);
  while (true) {
    std::vector<proc_fdinfo> fds(capacity);
    const int bytes_filled = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(),
                                          static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
    if (bytes_filled <= 0) {
      return {};
    }
    const std::size_t fd_count = static_cast<std::size_t>(bytes_filled) / sizeof(proc_fdinfo);
    if (fd_count < capacity || capacity == kMaxFdCapacity) {
      fds.resize(fd_count);
      return fds;
    }
    capacity = std::min(capacity * 2, kMaxFdCapacity);
  }
}

bool ReadSocketInfo(int pid, int fd, socket_fdinfo* socket_fd) {
  std::memset(socket_fd, 0, sizeof(*socket_fd));
  return proc_pidfdinfo(pid, fd, PROC_PIDFDSOCKETINFO, socket_fd,
                        PROC_PIDFDSOCKETINFO_SIZE) == PROC_PIDFDSOCKETINFO_SIZE;
}

}  // namespace

std::vector<int> ListAllPids() {
  std::size_t capacity = kPidBatchSize;
  while (true) {
    std::vector<int> pids(capacity);
    const int bytes_filled =
        proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(int)));
    if (bytes_filled <= 0) {
      return {};
    }
    const std::size_t pid_count = static_cast<std::size_t>(bytes_filled) / sizeof(int);
    if (pid_count < capacity || capacity == kMaxPidCapacity) {
      pids.resize(pid_count);
      pids.erase(std::remove(pids.begin(), pids.end(), 0), pids.end());
      return pids;
    }
    capacity = std::min(capacity * 2, kMaxPidCapacity);
  }
}

std::vector<SocketEntry> ScanPid(int pid) {
  proc_bsdinfo bsd_info{};
  if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd_info, PROC_PIDTBSDINFO_SIZE) !=
      PROC_PIDTBSDINFO_SIZE) {
    return {};
  }

  const std::string process_name = ResolveProcessName(pid, bsd_info);
  const std::size_t fd_hint = bsd_info.pbi_nfiles > 0
                                  ? static_cast<std::size_t>(bsd_info.pbi_nfiles)
                                  : kFallbackFdCapacity;
  const std::vector<proc_fdinfo> fds = ListProcessFds(pid, fd_hint);
  std::vector<SocketEntry> entries;

  for (const proc_fdinfo& fd_info : fds) {
    if (fd_info.proc_fdtype != PROX_FDTYPE_SOCKET) {
      continue;
    }

    socket_fdinfo socket_fd{};
    if (!ReadSocketInfo(pid, fd_info.proc_fd, &socket_fd) || !IsInetSocket(socket_fd)) {
      continue;
    }

    SocketEntry entry{
        .pid = pid,
        .process_name = process_name,
        .port = ExtractLocalPort(socket_fd),
        .protocol = MapProtocol(socket_fd.psi),
        .state = MapSocketState(socket_fd),
    };
    if (entry.protocol != Protocol::kUnknown && entry.port != 0) {
      entries.push_back(std::move(entry));
    }
  }

  return entries;
}

void FinalizeSnapshot(Snapshot* snapshot) {
  std::sort(snapshot->entries.begin(), snapshot->entries.end(),
            [](const SocketEntry& lhs, const SocketEntry& rhs) {
              if (lhs.pid != rhs.pid) return lhs.pid < rhs.pid;
              if (lhs.process_name != rhs.process_name) return lhs.process_name < rhs.process_name;
              if (lhs.port != rhs.port) return lhs.port < rhs.port;
              if (lhs.protocol != rhs.protocol) return lhs.protocol < rhs.protocol;
              return lhs.state < rhs.state;
            });
  std::vector<SocketEntry> compacted;
  compacted.reserve(snapshot->entries.size());
  for (const SocketEntry& entry : snapshot->entries) {
    if (compacted.empty() || !SameSocket(compacted.back(), entry)) {
      compacted.push_back(entry);
      continue;
    }

    SocketEntry& existing = compacted.back();
    existing.fd_count += entry.fd_count;
    if (StatePriority(entry.state) > StatePriority(existing.state)) {
      existing.state = entry.state;
    }
  }
  std::sort(compacted.begin(), compacted.end(), [](const SocketEntry& lhs, const SocketEntry& rhs) {
    if (lhs.port != rhs.port) return lhs.port < rhs.port;
    if (lhs.pid != rhs.pid) return lhs.pid < rhs.pid;
    if (lhs.process_name != rhs.process_name) return lhs.process_name < rhs.process_name;
    return lhs.protocol < rhs.protocol;
  });
  snapshot->entries = std::move(compacted);
}

}  // namespace portui::detail
