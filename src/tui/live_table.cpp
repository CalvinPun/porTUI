#include "portui/tui.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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

std::string FormatDuration(std::chrono::nanoseconds duration) {
  const double milliseconds = std::chrono::duration<double, std::milli>(duration).count();
  std::ostringstream output;
  output << std::fixed << std::setprecision(1) << milliseconds << "ms";
  return output.str();
}

std::string BlockBar(double fraction, std::size_t width) {
  const double clamped = std::clamp(fraction, 0.0, 1.0);
  const std::size_t filled = clamped == 0.0
                                 ? 0
                                 : std::min(width, static_cast<std::size_t>(std::ceil(clamped * width)));
  std::string bar;
  for (std::size_t index = 0; index < width; ++index) {
    bar += index < filled ? "▓" : "░";
  }
  return bar;
}

std::string Sparkline(const std::deque<double>& samples) {
  constexpr std::string_view kBlocks = "▁▂▃▄▅▆▇█";
  constexpr std::size_t kWidth = 30;
  std::string line;
  for (std::size_t index = samples.size(); index < kWidth; ++index) {
    line += " ";
  }
  for (double sample : samples) {
    const std::size_t index = std::min<std::size_t>(7, static_cast<std::size_t>(std::round(
                                                        std::clamp(sample, 0.0, 100.0) / 100.0 * 7.0)));
    line += kBlocks.substr(index * 3, 3);
  }
  return line;
}

std::size_t CountUniquePorts(const std::vector<SocketEntry>& sockets) {
  std::vector<std::uint16_t> ports;
  ports.reserve(sockets.size());
  for (const SocketEntry& socket : sockets) {
    ports.push_back(socket.port);
  }
  std::sort(ports.begin(), ports.end());
  ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
  return ports.size();
}

enum class Theme {
  kCozy,
  kRosePine,
  kMidnight,
  kCount,
};

Theme active_theme = Theme::kRosePine;

std::string ThemeName(Theme theme) {
  switch (theme) {
    case Theme::kCozy:
      return "Cozy";
    case Theme::kRosePine:
      return "Rose Pine";
    case Theme::kMidnight:
      return "Midnight";
    case Theme::kCount:
      break;
  }
  return "Cozy";
}

Color CozyRose() {
  if (active_theme == Theme::kRosePine) return Color::RGB(235, 188, 186);
  if (active_theme == Theme::kMidnight) return Color::RGB(128, 200, 220);
  return Color::RGB(213, 160, 163);
}
Color CozySage() { return Color::RGB(164, 190, 132); }
Color CozyLavender() {
  if (active_theme == Theme::kRosePine) return Color::RGB(196, 167, 231);
  if (active_theme == Theme::kMidnight) return Color::RGB(137, 169, 184);
  return Color::RGB(184, 160, 190);
}
Color CozyAmber() { return Color::RGB(224, 172, 91); }
Color CozyTerracotta() { return Color::RGB(214, 120, 94); }
Color CozyCocoa() {
  if (active_theme == Theme::kRosePine) return Color::RGB(62, 55, 73);
  if (active_theme == Theme::kMidnight) return Color::RGB(42, 69, 105);
  return Color::RGB(90, 62, 50);
}
Color CozyEspresso() {
  if (active_theme == Theme::kRosePine) return Color::RGB(25, 23, 36);
  if (active_theme == Theme::kMidnight) return Color::RGB(22, 28, 36);
  return Color::RGB(38, 29, 26);
}
Color CozyStripe() {
  if (active_theme == Theme::kRosePine) return Color::RGB(34, 32, 48);
  if (active_theme == Theme::kMidnight) return Color::RGB(29, 38, 49);
  return Color::RGB(48, 36, 31);
}
Color CozyMuted() { return Color::RGB(132, 116, 103); }

Color CpuColor(double percent) {
  if (percent < 0.1) return CozyMuted();
  if (percent >= 10.0) return CozyTerracotta();
  if (percent >= 2.0) return CozyAmber();
  return CozySage();
}

Color RamColor(std::uint64_t resident_bytes) {
  constexpr std::uint64_t kMebibyte = 1024ULL * 1024;
  constexpr std::uint64_t kGreenThreshold = 64 * kMebibyte;
  constexpr std::uint64_t kYellowThreshold = 256 * kMebibyte;
  constexpr std::uint64_t kRedThreshold = 1024 * kMebibyte;
  if (resident_bytes < kGreenThreshold) return CozyMuted();
  if (resident_bytes >= kRedThreshold) return CozyTerracotta();
  if (resident_bytes >= kYellowThreshold) return CozyAmber();
  return CozySage();
}

Color StateColor(std::size_t listener_count, std::size_t established_count) {
  if (listener_count > 0) return CozySage();
  if (established_count > 0) return CozyLavender();
  return CozyMuted();
}

Element Cell(const std::string& value, int width) {
  const std::size_t max_content_width = static_cast<std::size_t>(std::max(0, width - 1));
  std::string display;
  std::size_t character_count = 0;
  for (std::size_t index = 0; index < value.size() && character_count < max_content_width;) {
    const unsigned char byte = static_cast<unsigned char>(value[index]);
    const std::size_t byte_count = byte < 0x80 ? 1 : byte < 0xE0 ? 2 : byte < 0xF0 ? 3 : 4;
    if (index + byte_count > value.size()) {
      break;
    }
    display.append(value, index, byte_count);
    index += byte_count;
    ++character_count;
  }
  display.append(static_cast<std::size_t>(width) - character_count, ' ');
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
      if (confirm_force_kill_) {
        if (event == Event::y || event == Event::Y) {
          killer_.DispatchForce(pending_force_kill_pids_);
          status_lines_.push_front("Dispatched SIGKILL to " +
                                   std::to_string(pending_force_kill_pids_.size()) + " process(es).");
          pending_force_kill_pids_.clear();
          confirm_force_kill_ = false;
          return true;
        }
        if (event == Event::n || event == Event::N || event == Event::Escape) {
          pending_force_kill_pids_.clear();
          confirm_force_kill_ = false;
          return true;
        }
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
      if (filtering_) {
        if (event == Event::Return) {
          filtering_ = false;
          return true;
        }
        if (event == Event::Escape) {
          filter_query_.clear();
          filtering_ = false;
          RebuildGroupsPreservingFocus();
          return true;
        }
        if (event == Event::Backspace) {
          if (!filter_query_.empty()) {
            filter_query_.pop_back();
            RebuildGroupsPreservingFocus();
          }
          return true;
        }
        if (event.is_character()) {
          filter_query_ += event.character();
          RebuildGroupsPreservingFocus();
          return true;
        }
        return true;
      }
      if (theme_menu_) {
        if (event == Event::ArrowUp) {
          theme_menu_row_ = std::max(0, theme_menu_row_ - 1);
          return true;
        }
        if (event == Event::ArrowDown) {
          theme_menu_row_ = std::min(theme_menu_row_ + 1, static_cast<int>(Theme::kCount) - 1);
          return true;
        }
        if (event == Event::Return) {
          active_theme = static_cast<Theme>(theme_menu_row_);
          theme_menu_ = false;
          return true;
        }
        if (event == Event::Escape || event == Event::t || event == Event::T) {
          theme_menu_ = false;
          return true;
        }
        return true;
      }
      if (sort_menu_) {
        if (event == Event::ArrowUp) {
          sort_menu_row_ = std::max(0, sort_menu_row_ - 1);
          return true;
        }
        if (event == Event::ArrowDown) {
          sort_menu_row_ = std::min(sort_menu_row_ + 1, static_cast<int>(SortMode::kCount) - 1);
          return true;
        }
        if (event == Event::Return) {
          ApplySortMode(static_cast<SortMode>(sort_menu_row_));
          sort_menu_ = false;
          return true;
        }
        if (event == Event::Escape || event == Event::s || event == Event::S) {
          sort_menu_ = false;
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
      if (event == Event::s || event == Event::S) {
        sort_menu_row_ = static_cast<int>(sort_mode_);
        sort_menu_ = true;
        return true;
      }
      if (event == Event::t || event == Event::T) {
        theme_menu_row_ = static_cast<int>(active_theme);
        theme_menu_ = true;
        return true;
      }
      if (event == Event::Character('/')) {
        filter_query_.clear();
        filtering_ = true;
        RebuildGroupsPreservingFocus();
        return true;
      }
      if (event == Event::Character('?')) {
        show_help_ = !show_help_;
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
  enum class SortMode {
    kName,
    kCpu,
    kRam,
    kPortCount,
    kListenerCount,
    kCount,
  };

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

  std::string DescribePids(const std::vector<int>& pids) const {
    std::vector<std::string> names;
    names.reserve(pids.size());
    for (int pid : pids) {
      const auto group = std::find_if(groups_.begin(), groups_.end(), [pid](const ProcessGroup& item) {
        return item.pid == pid;
      });
      names.push_back(group == groups_.end() ? "PID " + std::to_string(pid)
                                             : group->process_name + " (PID " + std::to_string(pid) + ")");
    }
    if (names.size() > 2) {
      return names[0] + ", " + names[1] + ", and " + std::to_string(names.size() - 2) + " more";
    }
    return names.empty() ? "no processes" : names.size() == 1 ? names.front() : names[0] + " and " + names[1];
  }

  std::size_t ExpandedDetailLimit() const {
    // Keep actual socket rows visible above the footer on short terminals.
    const int available_rows = Terminal::Size().dimy - 20;
    return static_cast<std::size_t>(std::clamp(available_rows, 3, 8));
  }

  std::size_t ExpandedGroupLimit() const {
    const int available_rows = Terminal::Size().dimy - 18 -
                               static_cast<int>(ExpandedDetailLimit());
    return static_cast<std::size_t>(std::clamp(available_rows, 3, 8));
  }

  std::pair<std::size_t, std::size_t> VisibleGroupRange() const {
    const std::size_t visible_group_limit =
        expanded_pid_.has_value() ? ExpandedGroupLimit() : 16;
    const std::size_t first_group = selected_row_ > static_cast<int>(visible_group_limit / 2)
                                        ? static_cast<std::size_t>(selected_row_ - visible_group_limit / 2)
                                        : 0;
    return {first_group, std::min(groups_.size(), first_group + visible_group_limit)};
  }

  static std::string SortModeName(SortMode mode) {
    switch (mode) {
      case SortMode::kName:
        return "Process name";
      case SortMode::kCpu:
        return "CPU usage";
      case SortMode::kRam:
        return "RAM usage";
      case SortMode::kPortCount:
        return "Port count";
      case SortMode::kListenerCount:
        return "Listener count";
      case SortMode::kCount:
        break;
    }
    return "Process name";
  }

  void SortGroups() {
    std::sort(groups_.begin(), groups_.end(), [this](const ProcessGroup& lhs, const ProcessGroup& rhs) {
      switch (sort_mode_) {
        case SortMode::kCpu:
          if (lhs.cpu_percent != rhs.cpu_percent) return lhs.cpu_percent > rhs.cpu_percent;
          break;
        case SortMode::kRam:
          if (lhs.resident_bytes != rhs.resident_bytes) return lhs.resident_bytes > rhs.resident_bytes;
          break;
        case SortMode::kPortCount:
          if (CountUniquePorts(lhs.sockets) != CountUniquePorts(rhs.sockets)) {
            return CountUniquePorts(lhs.sockets) > CountUniquePorts(rhs.sockets);
          }
          break;
        case SortMode::kListenerCount: {
          const auto listener_count = [](const ProcessGroup& group) {
            return std::count_if(group.sockets.begin(), group.sockets.end(), [](const SocketEntry& entry) {
              return entry.state == SocketState::kListen;
            });
          };
          if (listener_count(lhs) != listener_count(rhs)) return listener_count(lhs) > listener_count(rhs);
          break;
        }
        case SortMode::kName:
        case SortMode::kCount:
          break;
      }
      if (lhs.process_name != rhs.process_name) return lhs.process_name < rhs.process_name;
      return lhs.pid < rhs.pid;
    });
  }

  void ApplySortMode(SortMode mode) {
    const int focused_pid = groups_.empty() ? -1 : groups_[selected_row_].pid;
    sort_mode_ = mode;
    SortGroups();
    const auto focused = std::find_if(groups_.begin(), groups_.end(), [focused_pid](const ProcessGroup& group) {
      return group.pid == focused_pid;
    });
    if (focused != groups_.end()) {
      selected_row_ = static_cast<int>(std::distance(groups_.begin(), focused));
    }
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
    SortGroups();
    if (!filter_query_.empty()) {
      const std::string query = Lowercase(filter_query_);
      std::erase_if(groups_, [&query](const ProcessGroup& group) {
        return Lowercase(group.process_name).find(query) == std::string::npos;
      });
    }
  }

  static std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
      return static_cast<char>(std::tolower(character));
    });
    return value;
  }

  void RebuildGroupsPreservingFocus() {
    const int focused_pid = groups_.empty() ? -1 : groups_[selected_row_].pid;
    BuildGroups();
    const auto focused = std::find_if(groups_.begin(), groups_.end(), [focused_pid](const ProcessGroup& group) {
      return group.pid == focused_pid;
    });
    if (focused != groups_.end()) {
      selected_row_ = static_cast<int>(std::distance(groups_.begin(), focused));
    } else if (groups_.empty()) {
      selected_row_ = 0;
    } else {
      selected_row_ = std::min(selected_row_, static_cast<int>(groups_.size() - 1));
    }
  }

  bool GroupHasEntries(
      const ProcessGroup& group,
      const std::unordered_map<std::string, std::chrono::steady_clock::time_point>& highlights) const {
    return std::any_of(group.sockets.begin(), group.sockets.end(), [&highlights](const SocketEntry& entry) {
      return highlights.contains(EntryKey(entry));
    });
  }

  void DrainKillResults() {
    for (const KillResult& result : killer_.DrainResults()) {
      status_lines_.push_front("PID " + std::to_string(result.pid) + ": " + result.message);
      if (result.status == KillStatus::kStillRunning && result.can_force_kill) {
        pending_force_kill_pids_.push_back(result.pid);
      }
    }
    if (!confirm_kill_ && !confirm_force_kill_ && !pending_force_kill_pids_.empty()) {
      std::sort(pending_force_kill_pids_.begin(), pending_force_kill_pids_.end());
      pending_force_kill_pids_.erase(
          std::unique(pending_force_kill_pids_.begin(), pending_force_kill_pids_.end()),
          pending_force_kill_pids_.end());
      confirm_force_kill_ = true;
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

    const int focused_pid = groups_.empty() ? -1 : groups_[selected_row_].pid;
    captured_at_ = update->snapshot.captured_at;
    ++scan_generation_;
    entries_ = update->snapshot.entries;
    usage_ = update->snapshot.process_usage;
    system_memory_bytes_ = update->snapshot.system_memory_bytes;
    logical_cpu_count_ = update->snapshot.logical_cpu_count;
    scan_duration_ = update->snapshot.scan_duration;
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
    cpu_history_.push_back(total_cpu_percent_);
    constexpr std::size_t kCpuHistoryLength = 30;
    while (cpu_history_.size() > kCpuHistoryLength) {
      cpu_history_.pop_front();
    }
    BuildGroups();
    std::erase_if(selected_pids_, [this](int pid) {
      return std::none_of(entries_.begin(), entries_.end(), [pid](const SocketEntry& entry) {
        return entry.pid == pid;
      });
    });
    if (expanded_pid_.has_value() &&
        std::none_of(groups_.begin(), groups_.end(), [this](const ProcessGroup& group) {
          return group.pid == *expanded_pid_;
        })) {
      expanded_pid_.reset();
    }
    const auto now = std::chrono::steady_clock::now();
    const auto expires_at = now + std::chrono::seconds(3);
    std::erase_if(added_highlights_, [now](const auto& item) { return item.second <= now; });
    std::erase_if(changed_highlights_, [now](const auto& item) { return item.second <= now; });
    for (const SocketEntry& entry : update->diff.added) {
      added_highlights_[EntryKey(entry)] = expires_at;
    }
    for (const SocketEntry& entry : update->diff.changed) {
      changed_highlights_[EntryKey(entry)] = expires_at;
    }
    const auto focused = std::find_if(groups_.begin(), groups_.end(), [focused_pid](const ProcessGroup& group) {
      return group.pid == focused_pid;
    });
    if (focused != groups_.end()) {
      selected_row_ = static_cast<int>(std::distance(groups_.begin(), focused));
    } else if (groups_.empty()) {
      selected_row_ = 0;
    } else {
      selected_row_ = std::min(selected_row_, static_cast<int>(groups_.size() - 1));
    }
  }

  Element Render() const {
    Elements rows;
    rows.push_back(hbox({Cell("SEL", 6), Cell("PID", 7), Cell("PROCESS", 16),
                         Cell("PORTS", 10), Cell("LISTEN", 8), Cell("CPU", 12),
                         Cell("RAM", 18)}) |
                   bold | color(CozyRose()));
    rows.push_back(separator());

    if (!has_snapshot_) {
      rows.push_back(text("Waiting for the first completed scan...") | dim);
    } else if (groups_.empty()) {
      rows.push_back(text("No IPv4/IPv6 sockets found in the latest scan.") | dim);
    }

    const auto [first_group, last_group] = VisibleGroupRange();
    std::uint64_t max_resident_bytes = 0;
    for (const ProcessGroup& group : groups_) {
      max_resident_bytes = std::max(max_resident_bytes, group.resident_bytes);
    }
    if (first_group > 0) {
      rows.push_back(text("... " + std::to_string(first_group) + " processes above") | dim);
    }
    for (std::size_t index = first_group; index < last_group; ++index) {
      const ProcessGroup& group = groups_[index];
      const std::size_t listener_count = std::count_if(
          group.sockets.begin(), group.sockets.end(), [](const SocketEntry& entry) {
            return entry.state == SocketState::kListen;
          });
      const std::size_t established_count = std::count_if(
          group.sockets.begin(), group.sockets.end(), [](const SocketEntry& entry) {
            return entry.state == SocketState::kEstablished;
          });
      const std::string cpu = FormatPercent(group.cpu_percent);
      const std::string cpu_meter = cpu + " " + BlockBar(group.cpu_percent / 10.0, 4);
      const std::string ram = FormatBytes(group.resident_bytes) + " " +
                              BlockBar(max_resident_bytes == 0
                                           ? 0.0
                                           : static_cast<double>(group.resident_bytes) /
                                                 static_cast<double>(max_resident_bytes),
                                       4);
      const bool low_signal = listener_count == 0 && established_count == 0 &&
                              group.cpu_percent < 0.1;
      Element row = hbox({Cell(selected_pids_.contains(group.pid) ? "[x]" : "[ ]", 6),
                          Cell(std::to_string(group.pid), 7), Cell(group.process_name, 16),
                          Cell(std::to_string(CountUniquePorts(group.sockets)) + " ports", 10),
                          Cell(std::to_string(listener_count), 8) |
                              color(StateColor(listener_count, established_count)),
                          Cell(cpu_meter, 12) | color(CpuColor(group.cpu_percent)),
                          Cell(ram, 18) | color(RamColor(group.resident_bytes))});
      if (static_cast<int>(index) == selected_row_) {
        row = row | bgcolor(CozyCocoa()) | color(Color::White);
      } else if (GroupHasEntries(group, added_highlights_)) {
        row = row | bgcolor(Color::RGB(47, 68, 48));
      } else if (GroupHasEntries(group, changed_highlights_)) {
        row = row | bgcolor(Color::RGB(74, 55, 27));
      } else if (low_signal) {
        row = row | dim;
      } else if (index % 2 == 1) {
        row = row | bgcolor(CozyStripe());
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
                       color(CozyRose()));
        const std::size_t detail_limit = ExpandedDetailLimit();
        for (std::size_t index = 0; index < std::min(group->sockets.size(), detail_limit); ++index) {
          const SocketEntry& entry = group->sockets[index];
          rows.push_back(hbox({Cell(std::to_string(entry.port), 8),
                               Cell(ToString(entry.protocol), 10), Cell(ToString(entry.state), 14),
                               Cell(std::to_string(entry.fd_count), 7)}));
        }
        if (group->sockets.size() > detail_limit) {
          rows.push_back(text("... and " + std::to_string(group->sockets.size() - detail_limit) +
                              " more sockets") |
                         dim);
        }
      }
    }

    const std::string focused = groups_.empty()
                                    ? "none"
                                    : groups_[selected_row_].process_name + " (" +
                                          std::to_string(groups_[selected_row_].pid) + ")";
    const std::string summary = "  focused: " + focused +
                                "  scan: " + FormatDuration(scan_duration_) + " parallel";
    const std::string counts = "  scanned: " + std::to_string(scanned_process_count_) +
                               "  socket owners: " + std::to_string(groups_.size()) +
                               "  sockets: " + std::to_string(entries_.size()) +
                               "  selected: " + std::to_string(selected_pids_.size());
    const std::string controls = "  [up/down move] [enter ports] [space select]";
    const std::string secondary_controls = "  [s sort] [t theme] [/ filter] [k kill] [? help] [q quit]";
    const std::string usage_summary = has_snapshot_
                                          ? "TOTAL CPU: " + FormatPercent(total_cpu_percent_) + "  " +
                                                Sparkline(cpu_history_) + "  RSS: " +
                                                FormatRam(total_resident_bytes_, system_memory_bytes_)
                                          : "TOTAL CPU: gathering...  PROCESS RSS: gathering...";
    Elements footer;
    if (confirm_kill_) {
      footer.push_back(text("Send SIGTERM to " + DescribePids(pending_kill_pids_) + "? [y/n]") |
                       bold | color(CozyTerracotta()));
    }
    if (confirm_force_kill_) {
      footer.push_back(text("SIGTERM did not stop " + DescribePids(pending_force_kill_pids_) +
                            ". Send SIGKILL? [y/n]") |
                       bold | color(CozyTerracotta()));
    }
    if (show_help_) {
      footer.push_back(text("HELP  [up/down focus] [enter ports] [space select]") | color(CozyRose()));
      footer.push_back(text("      [s sort] [t theme] [/ filter] [k kill]") | color(CozyRose()));
      footer.push_back(text("      [y/n confirm] [? close] [q quit]") | color(CozyRose()));
    }
    if (sort_menu_) {
      footer.push_back(text("SORT BY  up/down: choose  enter: apply  esc: cancel") | color(CozyRose()));
      for (int index = 0; index < static_cast<int>(SortMode::kCount); ++index) {
        const SortMode mode = static_cast<SortMode>(index);
        Element option = text(std::string(index == sort_menu_row_ ? "> " : "  ") +
                              SortModeName(mode));
        if (index == sort_menu_row_) {
          option = option | bold | color(Color::White) | bgcolor(CozyCocoa());
        } else if (mode == sort_mode_) {
          option = option | color(CozySage());
        }
        footer.push_back(std::move(option));
      }
    }
    if (theme_menu_) {
      footer.push_back(text("THEME  up/down: choose  enter: apply  esc: cancel") | color(CozyRose()));
      for (int index = 0; index < static_cast<int>(Theme::kCount); ++index) {
        const Theme theme = static_cast<Theme>(index);
        Element option = text(std::string(index == theme_menu_row_ ? "> " : "  ") + ThemeName(theme));
        if (index == theme_menu_row_) {
          option = option | bold | color(Color::White) | bgcolor(CozyCocoa());
        } else if (theme == active_theme) {
          option = option | color(CozySage());
        }
        footer.push_back(std::move(option));
      }
    }
    if (filtering_) {
      footer.push_back(text("FILTER PROCESS: " + filter_query_ + "_  [enter: apply  esc: clear]") |
                       color(CozyRose()));
    } else if (!filter_query_.empty()) {
      footer.push_back(text("FILTER: " + filter_query_ + "  [/: replace  esc: clear]") | color(CozyRose()));
    }
    for (const std::string& status : status_lines_) {
      footer.push_back(text(status) | dim);
    }
    footer.push_back(text(summary) | dim);
    footer.push_back(text(counts) | dim);
    footer.push_back(text(controls) | dim);
    footer.push_back(text(secondary_controls) | dim);
    const char spinner[] = {'|', '/', '-', '\\'};
    const std::string header = "porTUI [" + std::string(1, spinner[scan_generation_ % 4]) + "]" +
                               "  LIVE PORT MONITOR  last scan: " +
                               std::to_string(has_snapshot_
                                                  ? std::chrono::duration_cast<std::chrono::seconds>(
                                                        std::chrono::system_clock::now() - captured_at_)
                                                        .count()
                                                  : 0) +
                               "s";
    return vbox({text(header) | bold | color(CozyRose()),
                 text(usage_summary) | bold | color(CozyLavender()), separator(),
                 vbox(std::move(rows)) | flex, separator(), vbox(std::move(footer))}) |
           bgcolor(CozyEspresso()) | border;
  }

  SnapshotPipeline pipeline_;
  ProcessKiller killer_;
  std::chrono::system_clock::time_point captured_at_{};
  std::size_t scanned_process_count_ = 0;
  std::uint64_t system_memory_bytes_ = 0;
  std::uint32_t logical_cpu_count_ = 1;
  std::chrono::nanoseconds scan_duration_{};
  double total_cpu_percent_ = 0.0;
  std::uint64_t total_resident_bytes_ = 0;
  std::deque<double> cpu_history_;
  bool has_snapshot_ = false;
  std::size_t scan_generation_ = 0;
  std::vector<SocketEntry> entries_;
  std::vector<ProcessUsage> usage_;
  std::vector<ProcessGroup> groups_;
  std::unordered_set<int> selected_pids_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> added_highlights_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> changed_highlights_;
  std::vector<int> pending_kill_pids_;
  std::vector<int> pending_force_kill_pids_;
  std::deque<std::string> status_lines_;
  std::optional<int> expanded_pid_;
  bool confirm_kill_ = false;
  bool confirm_force_kill_ = false;
  bool show_help_ = false;
  bool filtering_ = false;
  std::string filter_query_;
  bool theme_menu_ = false;
  int theme_menu_row_ = 0;
  SortMode sort_mode_ = SortMode::kName;
  bool sort_menu_ = false;
  int sort_menu_row_ = 0;
  int selected_row_ = 0;
};

}  // namespace

int RunTui() {
  return LiveTable().Run();
}

}  // namespace portui
