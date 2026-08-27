#include "portui/snapshot_pipeline.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

namespace portui {
namespace {

using EntryKey = std::tuple<int, std::uint16_t, Protocol>;

EntryKey KeyFor(const SocketEntry& entry) {
  return {entry.pid, entry.port, entry.protocol};
}

bool HasChanged(const SocketEntry& previous, const SocketEntry& current) {
  return previous.process_name != current.process_name || previous.state != current.state ||
         previous.fd_count != current.fd_count;
}

}  // namespace

DiffResult DiffSnapshots(const Snapshot& previous, const Snapshot& current) {
  std::map<EntryKey, std::vector<const SocketEntry*>> previous_entries;
  std::map<EntryKey, std::vector<const SocketEntry*>> current_entries;

  for (const SocketEntry& entry : previous.entries) {
    previous_entries[KeyFor(entry)].push_back(&entry);
  }
  for (const SocketEntry& entry : current.entries) {
    current_entries[KeyFor(entry)].push_back(&entry);
  }

  DiffResult diff;
  for (const auto& [key, previous_group] : previous_entries) {
    const auto current_it = current_entries.find(key);
    if (current_it == current_entries.end()) {
      for (const SocketEntry* entry : previous_group) {
        diff.removed.push_back(*entry);
      }
      continue;
    }

    const std::vector<const SocketEntry*>& current_group = current_it->second;
    std::vector<bool> current_matched(current_group.size(), false);
    std::vector<const SocketEntry*> unmatched_previous;
    for (const SocketEntry* previous_entry : previous_group) {
      bool found_match = false;
      for (std::size_t index = 0; index < current_group.size(); ++index) {
        if (!current_matched[index] && !HasChanged(*previous_entry, *current_group[index])) {
          current_matched[index] = true;
          found_match = true;
          break;
        }
      }
      if (!found_match) {
        unmatched_previous.push_back(previous_entry);
      }
    }

    std::vector<const SocketEntry*> unmatched_current;
    for (std::size_t index = 0; index < current_group.size(); ++index) {
      if (!current_matched[index]) {
        unmatched_current.push_back(current_group[index]);
      }
    }

    const std::size_t changed_count = std::min(unmatched_previous.size(), unmatched_current.size());
    for (std::size_t index = 0; index < changed_count; ++index) {
      diff.changed.push_back(*unmatched_current[index]);
    }
    for (std::size_t index = changed_count; index < unmatched_previous.size(); ++index) {
      diff.removed.push_back(*unmatched_previous[index]);
    }
    for (std::size_t index = changed_count; index < unmatched_current.size(); ++index) {
      diff.added.push_back(*unmatched_current[index]);
    }
  }

  for (const auto& [key, current_group] : current_entries) {
    const auto previous_it = previous_entries.find(key);
    if (previous_it == previous_entries.end()) {
      for (const SocketEntry* entry : current_group) {
        diff.added.push_back(*entry);
      }
    }
  }
  return diff;
}

}  // namespace portui
