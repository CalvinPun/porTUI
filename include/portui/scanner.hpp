#pragma once

#include <memory>

#include "portui/snapshot.hpp"

namespace portui {

class Scanner {
 public:
  virtual ~Scanner() = default;

  virtual Snapshot Scan() = 0;
};

std::unique_ptr<Scanner> CreateSerialScanner();

}  // namespace portui
