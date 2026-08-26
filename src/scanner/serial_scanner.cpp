#include "portui/scanner.hpp"

#include <arpa/inet.h>
#include <libproc.h>
#include <sys/proc_info.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace portui {
namespace {

constexpr std::size_t kPidBatchSize = 4096;
constexpr std::size_t kFallbackFdCapacity = 64;

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

SocketState MapTcpState(const tcp_sockinfo& tcp_info) {
  switch (tcp_info.tcpsi_state) {
    case TSI_S_LISTEN:
      return SocketState::kListen;
    case TSI_S_ESTABLISHED:
      return SocketState::kEstablished;
    default:
      return SocketState::kUnknown;
  }
}

SocketState MapSocketState(const socket_fdinfo& socket_fd) {
  if (socket_fd.psi.soi_kind == SOCKINFO_TCP) {
    return MapTcpState(socket_fd.psi.soi_proto.pri_tcp);
  }

  return SocketState::kUnknown;
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

bool SameEntry(const SocketEntry& lhs, const SocketEntry& rhs) {
  return lhs.pid == rhs.pid && lhs.process_name == rhs.process_name &&
         lhs.port == rhs.port && lhs.protocol == rhs.protocol &&
         lhs.state == rhs.state;
}

std::string ResolveProcessName(int pid, const proc_bsdinfo& bsd_info) {
  if (bsd_info.pbi_name[0] != '\0') {
    return bsd_info.pbi_name;
  }

  if (bsd_info.pbi_comm[0] != '\0') {
    return bsd_info.pbi_comm;
  }

  std::array<char, 2 * MAXCOMLEN> name_buffer{};
  const int name_size = proc_name(pid, name_buffer.data(), name_buffer.size());
  if (name_size > 0) {
    return std::string(name_buffer.data());
  }

  return "<unknown>";
}

std::vector<int> ListAllPids() {
  std::vector<int> pids(kPidBatchSize);
  const int bytes_filled =
      proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(int)));
  if (bytes_filled <= 0) {
    return {};
  }

  const std::size_t pid_count = static_cast<std::size_t>(bytes_filled) / sizeof(int);
  pids.resize(pid_count);
  pids.erase(std::remove(pids.begin(), pids.end(), 0), pids.end());
  return pids;
}

std::vector<proc_fdinfo> ListProcessFds(int pid, std::size_t hint) {
  std::vector<proc_fdinfo> fds(std::max(hint, kFallbackFdCapacity));
  const int bytes_filled = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(),
                                        static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
  if (bytes_filled <= 0) {
    return {};
  }

  const std::size_t fd_count =
      static_cast<std::size_t>(bytes_filled) / sizeof(proc_fdinfo);
  fds.resize(fd_count);
  return fds;
}

bool ReadBsdInfo(int pid, proc_bsdinfo* bsd_info) {
  std::memset(bsd_info, 0, sizeof(*bsd_info));
  const int bytes_read =
      proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, bsd_info, PROC_PIDTBSDINFO_SIZE);
  return bytes_read == PROC_PIDTBSDINFO_SIZE;
}

bool ReadSocketInfo(int pid, int fd, socket_fdinfo* socket_fd) {
  std::memset(socket_fd, 0, sizeof(*socket_fd));
  const int bytes_read = proc_pidfdinfo(pid, fd, PROC_PIDFDSOCKETINFO, socket_fd,
                                        PROC_PIDFDSOCKETINFO_SIZE);
  return bytes_read == PROC_PIDFDSOCKETINFO_SIZE;
}

class SerialScanner final : public Scanner {
 public:
  Snapshot Scan() override {
    Snapshot snapshot;
    snapshot.captured_at = std::chrono::system_clock::now();

    const std::vector<int> pids = ListAllPids();
    for (int pid : pids) {
      proc_bsdinfo bsd_info{};
      if (!ReadBsdInfo(pid, &bsd_info)) {
        continue;
      }

      const std::string process_name = ResolveProcessName(pid, bsd_info);
      const std::size_t fd_hint =
          bsd_info.pbi_nfiles > 0 ? static_cast<std::size_t>(bsd_info.pbi_nfiles)
                                  : kFallbackFdCapacity;
      const std::vector<proc_fdinfo> fds = ListProcessFds(pid, fd_hint);

      for (const proc_fdinfo& fd_info : fds) {
        if (fd_info.proc_fdtype != PROX_FDTYPE_SOCKET) {
          continue;
        }

        socket_fdinfo socket_fd{};
        if (!ReadSocketInfo(pid, fd_info.proc_fd, &socket_fd)) {
          continue;
        }

        if (!IsInetSocket(socket_fd)) {
          continue;
        }

        SocketEntry entry{
            .pid = pid,
            .process_name = process_name,
            .port = ExtractLocalPort(socket_fd),
            .protocol = MapProtocol(socket_fd.psi),
            .state = MapSocketState(socket_fd),
        };

        if (entry.protocol == Protocol::kUnknown || entry.port == 0) {
          continue;
        }

        snapshot.entries.push_back(std::move(entry));
      }
    }

    std::sort(snapshot.entries.begin(), snapshot.entries.end(),
              [](const SocketEntry& lhs, const SocketEntry& rhs) {
                if (lhs.port != rhs.port) {
                  return lhs.port < rhs.port;
                }
                if (lhs.pid != rhs.pid) {
                  return lhs.pid < rhs.pid;
                }
                if (lhs.process_name != rhs.process_name) {
                  return lhs.process_name < rhs.process_name;
                }
                if (lhs.protocol != rhs.protocol) {
                  return lhs.protocol < rhs.protocol;
                }
                return lhs.state < rhs.state;
              });
    snapshot.entries.erase(
        std::unique(snapshot.entries.begin(), snapshot.entries.end(), SameEntry),
        snapshot.entries.end());

    return snapshot;
  }
};

}  // namespace

std::unique_ptr<Scanner> CreateSerialScanner() {
  return std::make_unique<SerialScanner>();
}

}  // namespace portui
