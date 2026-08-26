#pragma once

#include <span>
#include <string_view>

namespace portui {

int RunApp(std::span<const std::string_view> args);

}  // namespace portui
