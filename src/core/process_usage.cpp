#include "portui/snapshot_pipeline.hpp"

#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <unordered_map>

#include "../scanner/scanner_support.hpp"

namespace portui::detail {

void PopulateProcessUsage(Snapshot* snapshot) {
  std::size_t memory_size = sizeof(snapshot->system_memory_bytes);
  sysctlbyname("hw.memsize", &snapshot->system_memory_bytes, &memory_size, nullptr, 0);
  static std::unordered_map<int, std::pair<std::uint64_t, std::chrono::steady_clock::time_point>> previous;
  const auto now = std::chrono::steady_clock::now();
  std::unordered_map<int, std::pair<std::uint64_t, std::chrono::steady_clock::time_point>> next;
  for (int pid : ListAllPids()) {
    proc_bsdinfo bsd{};
    proc_taskinfo task{};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, PROC_PIDTBSDINFO_SIZE) != PROC_PIDTBSDINFO_SIZE ||
        proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, PROC_PIDTASKINFO_SIZE) != PROC_PIDTASKINFO_SIZE) continue;
    const std::uint64_t cpu_time = task.pti_total_user + task.pti_total_system;
    double cpu = 0.0;
    if (const auto it = previous.find(pid); it != previous.end() && cpu_time >= it->second.first) {
      const double elapsed = std::chrono::duration<double, std::nano>(now - it->second.second).count();
      if (elapsed > 0.0) cpu = 100.0 * static_cast<double>(cpu_time - it->second.first) / elapsed;
    }
    next.emplace(pid, std::make_pair(cpu_time, now));
    const std::string name = bsd.pbi_name[0] != '\0' ? bsd.pbi_name : bsd.pbi_comm;
    snapshot->process_usage.push_back({.pid = pid, .parent_pid = static_cast<int>(bsd.pbi_ppid),
                                       .process_name = name.empty() ? "<unknown>" : name,
                                       .cpu_percent = cpu, .resident_bytes = task.pti_resident_size});
  }
  previous = std::move(next);

  // macOS can deny PROC_PIDTASKINFO while still exposing the process to ps.
  std::FILE* pipe = popen("/bin/ps -axo pid=,ppid=,pcpu=,rss=,comm=", "r");
  if (pipe == nullptr) {
    return;
  }
  std::array<char, 1024> line_buffer{};
  while (std::fgets(line_buffer.data(), static_cast<int>(line_buffer.size()), pipe) != nullptr) {
    std::istringstream line(line_buffer.data());
    int pid = -1;
    int parent_pid = -1;
    double cpu = 0.0;
    std::uint64_t resident_kib = 0;
    std::string command;
    if (!(line >> pid >> parent_pid >> cpu >> resident_kib >> command) || pid <= 0) {
      continue;
    }
    const auto found = std::find_if(snapshot->process_usage.begin(), snapshot->process_usage.end(),
                                    [pid](const ProcessUsage& usage) { return usage.pid == pid; });
    if (found == snapshot->process_usage.end()) {
      snapshot->process_usage.push_back({.pid = pid, .parent_pid = parent_pid,
                                         .process_name = std::move(command),
                                         .cpu_percent = cpu,
                                         .resident_bytes = resident_kib * 1024});
    }
  }
  pclose(pipe);
}

}  // namespace portui::detail
