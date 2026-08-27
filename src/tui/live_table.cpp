#include "portui/tui.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "portui/process_killer.hpp"
#include "portui/scanner.hpp"
#include "portui/snapshot_pipeline.hpp"

namespace portui {
namespace {

using namespace ftxui;

std::string ToString(Protocol protocol) {
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

std::string ToString(SocketState state) {
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

std::string EntryKey(const SocketEntry& entry) {
  return std::to_string(entry.pid) + ":" + std::to_string(entry.port) + ":" +
         std::to_string(static_cast<int>(entry.protocol));
}

std::string FormatBytes(std::uint64_t bytes) {
  if (bytes >= 1024ULL * 1024 * 1024) {
    return std::to_string(bytes / (1024ULL * 1024 * 1024)) + " GB";
  }
  return std::to_string(bytes / (1024 * 1024)) + " MB";
}

std::string FormatRam(std::uint64_t bytes, std::uint64_t total_bytes) {
  if (total_bytes == 0) {
    return FormatBytes(bytes);
  }
  return FormatBytes(bytes) + " / " + FormatBytes(total_bytes);
}

std::string FormatPercent(double value) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(1) << value << "%";
  return output.str();
}

std::string FormatPorts(const std::vector<SocketEntry>& sockets) {
  std::vector<std::uint16_t> ports;
  ports.reserve(sockets.size());
  for (const SocketEntry& socket : sockets) {
    ports.push_back(socket.port);
  }
  std::sort(ports.begin(), ports.end());
  ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
  if (ports.empty()) {
    return "-";
  }

  constexpr std::size_t kShownPortLimit = 3;
  std::string result;
  for (std::size_t index = 0; index < std::min(ports.size(), kShownPortLimit); ++index) {
    if (!result.empty()) {
      result += ",";
    }
    result += std::to_string(ports[index]);
  }
  if (ports.size() > kShownPortLimit) {
    result += " +" + std::to_string(ports.size() - kShownPortLimit);
  }
  return result;
}

Element Cell(const std::string& value, int width) {
  std::string display = value;
  const std::size_t max_content_width = static_cast<std::size_t>(std::max(0, width - 1));
  if (display.size() > max_content_width) {
    display.resize(max_content_width);
  }
  display.resize(static_cast<std::size_t>(width), ' ');
  return text(display);
}

class LiveTable {
 public:
  LiveTable() : pipeline_(CreateParallelScanner()) {}

  int Run() {
    pipeline_.Start();
    auto screen = ScreenInteractive::Fullscreen();
    std::atomic<bool> refresh_running = true;
    std::thread refresh_thread([&] {
      while (refresh_running.load()) {
        screen.PostEvent(Event::Custom);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });

    auto component = Renderer([this] { return Render(); });
    component = CatchEvent(component, [this, &screen](Event event) {
      if (event == Event::Custom) {
        Refresh();
        DrainKillResults();
        return true;
      }
      if (confirm_kill_) {
        if (event == Event::y || event == Event::Y) {
          killer_.Dispatch(pending_kill_pids_);
          status_lines_.push_front("Dispatched SIGTERM to " +
                                   std::to_string(pending_kill_pids_.size()) + " process(es).");
          ClearSelectedPids(pending_kill_pids_);
          pending_kill_pids_.clear();
          confirm_kill_ = false;
          return true;
        }
        if (event == Event::n || event == Event::N || event == Event::Escape) {
          pending_kill_pids_.clear();
          confirm_kill_ = false;
          return true;
        }
        return true;
      }
      if (event == Event::ArrowUp) {
        selected_row_ = std::max(0, selected_row_ - 1);
        return true;
      }
      if (event == Event::ArrowDown) {
        if (!groups_.empty()) {
          selected_row_ = std::min(selected_row_ + 1, static_cast<int>(groups_.size() - 1));
        }
        return true;
      }
      if (event == Event::Return && !groups_.empty()) {
        const int pid = groups_[selected_row_].pid;
        expanded_pid_ = expanded_pid_ == pid ? std::nullopt : std::optional<int>(pid);
        return true;
      }
      if (event == Event::Character(' ') && !groups_.empty()) {
        const int pid = groups_[selected_row_].pid;
        if (!selected_pids_.erase(pid)) {
          selected_pids_.insert(pid);
        }
        return true;
      }
      if (event == Event::k || event == Event::K) {
        pending_kill_pids_ = SelectedPids();
        if (pending_kill_pids_.empty()) {
          status_lines_.push_front("Select one or more rows before sending SIGTERM.");
        } else {
          confirm_kill_ = true;
        }
        return true;
      }
      if (event == Event::q || event == Event::Escape) {
        screen.Exit();
        return true;
      }
      return false;
    });

    screen.Loop(component);
    refresh_running.store(false);
    refresh_thread.join();
    pipeline_.Stop();
    return 0;
  }

 private:
  struct ProcessGroup {
    int pid = -1;
    std::string process_name;
    double cpu_percent = 0.0;
    std::uint64_t resident_bytes = 0;
    std::vector<SocketEntry> sockets;
  };

  std::vector<int> SelectedPids() const {
    std::vector<int> pids(selected_pids_.begin(), selected_pids_.end());
    std::sort(pids.begin(), pids.end());
    return pids;
  }

  void ClearSelectedPids(const std::vector<int>& pids) {
    for (int pid : pids) {
      selected_pids_.erase(pid);
    }
  }

  std::pair<std::size_t, std::size_t> VisibleGroupRange() const {
    constexpr std::size_t kVisibleGroupLimit = 16;
    const std::size_t first_group = selected_row_ > static_cast<int>(kVisibleGroupLimit / 2)
                                        ? static_cast<std::size_t>(selected_row_ - kVisibleGroupLimit / 2)
                                        : 0;
    return {first_group, std::min(groups_.size(), first_group + kVisibleGroupLimit)};
  }

  void BuildGroups() {
    groups_.clear();
    for (const SocketEntry& entry : entries_) {
      const auto group = std::find_if(groups_.begin(), groups_.end(), [&entry](const ProcessGroup& item) {
        return item.pid == entry.pid;
      });
      if (group == groups_.end()) {
        groups_.push_back({.pid = entry.pid, .process_name = entry.process_name, .sockets = {entry}});
      } else {
        group->sockets.push_back(entry);
      }
    }
    for (ProcessGroup& group : groups_) {
      const auto usage = std::find_if(usage_.begin(), usage_.end(), [&group](const ProcessUsage& item) {
        return item.pid == group.pid;
      });
      if (usage != usage_.end()) {
        group.cpu_percent = usage->cpu_percent;
        group.resident_bytes = usage->resident_bytes;
      }
    }
    std::sort(groups_.begin(), groups_.end(), [](const ProcessGroup& lhs, const ProcessGroup& rhs) {
      if (lhs.process_name != rhs.process_name) return lhs.process_name < rhs.process_name;
      return lhs.pid < rhs.pid;
    });
  }

  bool GroupHasEntries(const ProcessGroup& group,
                       const std::unordered_set<std::string>& keys) const {
    return std::any_of(group.sockets.begin(), group.sockets.end(), [&keys](const SocketEntry& entry) {
      return keys.contains(EntryKey(entry));
    });
  }

  void DrainKillResults() {
    for (const KillResult& result : killer_.DrainResults()) {
      status_lines_.push_front("PID " + std::to_string(result.pid) + ": " + result.message);
    }
    while (status_lines_.size() > 3) {
      status_lines_.pop_back();
    }
  }

  void Refresh() {
    const std::optional<SnapshotUpdate> update = pipeline_.Latest();
    if (!update.has_value() || update->snapshot.captured_at == captured_at_) {
      return;
    }

    captured_at_ = update->snapshot.captured_at;
    entries_ = update->snapshot.entries;
    usage_ = update->snapshot.process_usage;
    system_memory_bytes_ = update->snapshot.system_memory_bytes;
    logical_cpu_count_ = update->snapshot.logical_cpu_count;
    scanned_process_count_ = update->snapshot.scanned_process_count;
    has_snapshot_ = true;
    total_cpu_percent_ = 0.0;
    total_resident_bytes_ = 0;
    for (const ProcessUsage& usage : usage_) {
      total_cpu_percent_ += usage.cpu_percent;
      total_resident_bytes_ += usage.resident_bytes;
    }
    total_cpu_percent_ = std::min(100.0, total_cpu_percent_ /
                                             static_cast<double>(std::max(1U, logical_cpu_count_)));
    BuildGroups();
    std::erase_if(selected_pids_, [this](int pid) {
      return std::none_of(groups_.begin(), groups_.end(), [pid](const ProcessGroup& group) {
        return group.pid == pid;
      });
    });
    if (expanded_pid_.has_value() &&
        std::none_of(groups_.begin(), groups_.end(), [this](const ProcessGroup& group) {
          return group.pid == *expanded_pid_;
        })) {
      expanded_pid_.reset();
    }
    added_keys_.clear();
    changed_keys_.clear();
    for (const SocketEntry& entry : update->diff.added) {
      added_keys_.insert(EntryKey(entry));
    }
    for (const SocketEntry& entry : update->diff.changed) {
      changed_keys_.insert(EntryKey(entry));
    }
    if (groups_.empty()) {
      selected_row_ = 0;
    } else {
      selected_row_ = std::min(selected_row_, static_cast<int>(groups_.size() - 1));
    }
  }

  Element Render() const {
    Elements rows;
    rows.push_back(hbox({Cell("SEL", 6), Cell("PID", 8), Cell("PROCESS", 17),
                         Cell("PORTS", 16), Cell("LISTEN", 9), Cell("CPU", 8),
                         Cell("RAM / SYS", 15)}) |
                   bold | color(Color::Cyan));
    rows.push_back(separator());

    if (!has_snapshot_) {
      rows.push_back(text("Waiting for the first completed scan...") | dim);
    } else if (groups_.empty()) {
      rows.push_back(text("No IPv4/IPv6 sockets found in the latest scan.") | dim);
    }

    const auto [first_group, last_group] = VisibleGroupRange();
    if (first_group > 0) {
      rows.push_back(text("... " + std::to_string(first_group) + " processes above") | dim);
    }
    for (std::size_t index = first_group; index < last_group; ++index) {
      const ProcessGroup& group = groups_[index];
      const std::size_t listener_count = std::count_if(
          group.sockets.begin(), group.sockets.end(), [](const SocketEntry& entry) {
            return entry.state == SocketState::kListen;
          });
      const std::string cpu = FormatPercent(group.cpu_percent);
      const std::string ram = FormatRam(group.resident_bytes, system_memory_bytes_);
      Element row = hbox({Cell(selected_pids_.contains(group.pid) ? "[x]" : "[ ]", 6),
                          Cell(std::to_string(group.pid), 8), Cell(group.process_name, 17),
                          Cell(FormatPorts(group.sockets), 16),
                          Cell(std::to_string(listener_count), 9), Cell(cpu, 8), Cell(ram, 15)});
      if (static_cast<int>(index) == selected_row_) {
        row = row | bgcolor(Color::Blue) | color(Color::White);
      } else if (GroupHasEntries(group, added_keys_)) {
        row = row | color(Color::Green);
      } else if (GroupHasEntries(group, changed_keys_)) {
        row = row | color(Color::Yellow);
      }
      rows.push_back(row);
    }
    if (last_group < groups_.size()) {
      rows.push_back(text("... " + std::to_string(groups_.size() - last_group) + " processes below") | dim);
    }

    if (expanded_pid_.has_value()) {
      const auto group = std::find_if(groups_.begin(), groups_.end(), [this](const ProcessGroup& item) {
        return item.pid == *expanded_pid_;
      });
      if (group != groups_.end()) {
        rows.push_back(separator());
        rows.push_back(text("Ports for " + group->process_name + " (PID " +
                            std::to_string(group->pid) + ")") |
                       bold);
        rows.push_back(hbox({Cell("PORT", 8), Cell("PROTO", 10), Cell("STATE", 14),
                             Cell("FDS", 7)}) |
                       color(Color::Cyan));
        constexpr std::size_t kDetailLimit = 10;
        for (std::size_t index = 0; index < std::min(group->sockets.size(), kDetailLimit); ++index) {
          const SocketEntry& entry = group->sockets[index];
          rows.push_back(hbox({Cell(std::to_string(entry.port), 8),
                               Cell(ToString(entry.protocol), 10), Cell(ToString(entry.state), 14),
                               Cell(std::to_string(entry.fd_count), 7)}));
        }
        if (group->sockets.size() > kDetailLimit) {
          rows.push_back(text("... and " + std::to_string(group->sockets.size() - kDetailLimit) +
                              " more sockets") |
                         dim);
        }
      }
    }

    const std::string focused = groups_.empty()
                                    ? "none"
                                    : groups_[selected_row_].process_name + " (" +
                                          std::to_string(groups_[selected_row_].pid) + ")";
    const std::string summary = "focused: " + focused +
                                "  scanned: " + std::to_string(scanned_process_count_) +
                                "  socket owners: " + std::to_string(groups_.size()) +
                                "  sockets: " + std::to_string(entries_.size()) +
                                "  selected: " + std::to_string(selected_pids_.size());
    const std::string controls = "up/down: move  enter: ports  space: select  k: terminate  q: quit";
    const std::string usage_summary = has_snapshot_
                                          ? "TOTAL CPU: " + FormatPercent(total_cpu_percent_) +
                                                "  PROCESS RSS: " +
                                                FormatRam(total_resident_bytes_, system_memory_bytes_)
                                          : "TOTAL CPU: gathering...  PROCESS RSS: gathering...";
    Elements footer;
    if (confirm_kill_) {
      footer.push_back(text("Send SIGTERM to " + std::to_string(pending_kill_pids_.size()) +
                            " selected process(es)? [y/n]") |
                       bold | color(Color::Red));
    }
    for (const std::string& status : status_lines_) {
      footer.push_back(text(status) | dim);
    }
    footer.push_back(text(summary) | dim);
    footer.push_back(text(controls) | dim);
    return vbox({text("porTUI  LIVE PORT MONITOR") | bold | color(Color::Green),
                 text(usage_summary) | bold | color(Color::Cyan), separator(),
                 vbox(std::move(rows)) | flex, separator(), vbox(std::move(footer))}) |
           border;
  }

  SnapshotPipeline pipeline_;
  ProcessKiller killer_;
  std::chrono::system_clock::time_point captured_at_{};
  std::size_t scanned_process_count_ = 0;
  std::uint64_t system_memory_bytes_ = 0;
  std::uint32_t logical_cpu_count_ = 1;
  double total_cpu_percent_ = 0.0;
  std::uint64_t total_resident_bytes_ = 0;
  bool has_snapshot_ = false;
  std::vector<SocketEntry> entries_;
  std::vector<ProcessUsage> usage_;
  std::vector<ProcessGroup> groups_;
  std::unordered_set<int> selected_pids_;
  std::unordered_set<std::string> added_keys_;
  std::unordered_set<std::string> changed_keys_;
  std::vector<int> pending_kill_pids_;
  std::deque<std::string> status_lines_;
  std::optional<int> expanded_pid_;
  bool confirm_kill_ = false;
  int selected_row_ = 0;
};

}  // namespace

int RunTui() {
  return LiveTable().Run();
}

}  // namespace portui
