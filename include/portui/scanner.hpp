#pragma once

#include <cstddef>
#include <memory>

#include "portui/snapshot.hpp"

namespace portui {

class Scanner {
 public:
  virtual ~Scanner() = default;

  virtual Snapshot Scan() = 0;
};

std::unique_ptr<Scanner> CreateSerialScanner();
std::unique_ptr<Scanner> CreateParallelScanner(std::size_t worker_count = 0);

}  // namespace portui
