#pragma once

#include <vector>

#include "portui/snapshot.hpp"

namespace portui {

struct DiffResult {
  std::vector<SocketEntry> added;
  std::vector<SocketEntry> removed;
  std::vector<SocketEntry> changed;
};

}  // namespace portui
