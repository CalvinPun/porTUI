#include <span>
#include <string_view>
#include <vector>

#include "portui/app.hpp"

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);

  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  return portui::RunApp(std::span<const std::string_view>(args));
}
