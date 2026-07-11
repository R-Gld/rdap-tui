// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/app_state.hpp"
#include "rdap/bootstrap_cache.hpp"
#include "rdap/tui_app.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

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
    std::optional<rdap::BootstrapCache> disk_cache;
    const auto bootstrap_directory = rdap::default_bootstrap_cache_directory();
    if (!bootstrap_directory.empty()) {
      std::error_code error;
      std::filesystem::create_directories(bootstrap_directory, error);
      if (!error) {
        disk_cache.emplace(bootstrap_directory);
      }
    }

    const auto app_paths = rdap::default_app_paths();
    auto config = rdap::read_app_config(app_paths.config_file);
    std::optional<rdap::AppStateStore> state_store;
    rdap::AppState state;
    if (!app_paths.state_file.empty()) {
      state_store.emplace(app_paths.state_file);
      state = state_store->read();
    }

    return rdap::run_tui(disk_cache.has_value() ? &*disk_cache : nullptr, config, std::move(state),
                         state_store.has_value() ? &*state_store : nullptr);
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
