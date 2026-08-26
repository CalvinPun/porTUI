#pragma once

#include "portui/snapshot.hpp"

namespace portui {

class Scanner {
 public:
  virtual ~Scanner() = default;

  virtual Snapshot Scan() = 0;
};

}  // namespace portui
