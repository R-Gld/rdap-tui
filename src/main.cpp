// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/tui_app.hpp"

#include <iostream>
#include <string_view>

namespace {

void print_help(std::string_view executable) {
  std::cout << "Usage: " << executable << " [--help] [--version]\n\n"
            << "Interactive RDAP client for domain lookups.\n"
            << "  --help     Show this help\n"
            << "  --version  Show the version\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 1) {
    return rdap::run_tui();
  }
  if (argc == 2) {
    const std::string_view argument(argv[1]);
    if (argument == "--help" || argument == "-h") {
      print_help(argv[0]);
      return 0;
    }
    if (argument == "--version") {
      std::cout << "rdap-tui " << RDAP_TUI_VERSION << '\n';
      return 0;
    }
  }

  std::cerr << "Unknown arguments. Run '" << argv[0] << " --help' for usage.\n";
  return 2;
}
