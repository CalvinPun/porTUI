#pragma once

#include <string>

namespace portui {

enum class KillStatus {
  kSuccess,
  kStillRunning,
  kPermissionDenied,
  kAlreadyExited,
  kError,
};

struct KillResult {
  int pid = -1;
  KillStatus status = KillStatus::kError;
  bool can_force_kill = false;
  std::string message;
};

}  // namespace portui
