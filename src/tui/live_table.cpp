#include "portui/tui.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

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

Element Cell(const std::string& value, int width) {
  return text(value) | size(WIDTH, EQUAL, width);
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
        return true;
      }
      if (event == Event::ArrowUp) {
        selected_row_ = std::max(0, selected_row_ - 1);
        return true;
      }
      if (event == Event::ArrowDown) {
        if (!entries_.empty()) {
          selected_row_ = std::min(selected_row_ + 1, static_cast<int>(entries_.size() - 1));
        }
        return true;
      }
      if (event == Event::Character(' ') && !entries_.empty()) {
        const std::string key = EntryKey(entries_[selected_row_]);
        if (!selected_keys_.erase(key)) {
          selected_keys_.insert(key);
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
  void Refresh() {
    const std::optional<SnapshotUpdate> update = pipeline_.Latest();
    if (!update.has_value() || update->snapshot.captured_at == captured_at_) {
      return;
    }

    captured_at_ = update->snapshot.captured_at;
    entries_ = update->snapshot.entries;
    added_keys_.clear();
    changed_keys_.clear();
    for (const SocketEntry& entry : update->diff.added) {
      added_keys_.insert(EntryKey(entry));
    }
    for (const SocketEntry& entry : update->diff.changed) {
      changed_keys_.insert(EntryKey(entry));
    }
    if (entries_.empty()) {
      selected_row_ = 0;
    } else {
      selected_row_ = std::min(selected_row_, static_cast<int>(entries_.size() - 1));
    }
  }

  Element Render() const {
    Elements rows;
    rows.push_back(hbox({Cell("SEL", 5), Cell("PID", 8), Cell("PROCESS", 22),
                         Cell("PORT", 8), Cell("PROTO", 10), Cell("STATE", 14)}) |
                   bold | color(Color::Cyan));
    rows.push_back(separator());

    if (entries_.empty()) {
      rows.push_back(text("Waiting for the first completed scan...") | dim);
    }

    for (std::size_t index = 0; index < entries_.size(); ++index) {
      const SocketEntry& entry = entries_[index];
      const std::string key = EntryKey(entry);
      const bool selected = selected_keys_.contains(key);
      Element row = hbox({Cell(selected ? "[x]" : "[ ]", 5),
                          Cell(std::to_string(entry.pid), 8), Cell(entry.process_name, 22),
                          Cell(std::to_string(entry.port), 8), Cell(ToString(entry.protocol), 10),
                          Cell(ToString(entry.state), 14)});
      if (static_cast<int>(index) == selected_row_) {
        row = row | bgcolor(Color::Blue) | color(Color::White);
      } else if (added_keys_.contains(key)) {
        row = row | color(Color::Green);
      } else if (changed_keys_.contains(key)) {
        row = row | color(Color::Yellow);
      }
      rows.push_back(row);
    }

    const std::string summary = "sockets: " + std::to_string(entries_.size()) +
                                "  selected: " + std::to_string(selected_keys_.size()) +
                                "  up/down: move  space: select  q: quit";
    return vbox({text("porTUI  LIVE PORT MONITOR") | bold | color(Color::Green), separator(),
                 vbox(std::move(rows)) | flex, separator(), text(summary) | dim}) |
           border;
  }

  SnapshotPipeline pipeline_;
  std::chrono::system_clock::time_point captured_at_{};
  std::vector<SocketEntry> entries_;
  std::unordered_set<std::string> selected_keys_;
  std::unordered_set<std::string> added_keys_;
  std::unordered_set<std::string> changed_keys_;
  int selected_row_ = 0;
};

}  // namespace

int RunTui() {
  return LiveTable().Run();
}

}  // namespace portui
