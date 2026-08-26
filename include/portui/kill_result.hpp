#pragma once

#include <string>

namespace portui {

enum class KillStatus {
  kSuccess,
  kPermissionDenied,
  kAlreadyExited,
  kError,
};

struct KillResult {
  int pid = -1;
  KillStatus status = KillStatus::kError;
  std::string message;
};

}  // namespace portui
