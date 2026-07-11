// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rdap {

struct AppConfig {
  std::size_t history_limit{200};
};

struct AppState {
  std::vector<std::string> history;
  std::vector<std::string> favorites;
};

struct AppPaths {
  std::filesystem::path config_file;
  std::filesystem::path state_file;
};

class AppStateStore {
public:
  explicit AppStateStore(std::filesystem::path state_file);

  [[nodiscard]] AppState read() const;
  bool write(const AppState &state) const;

private:
  std::filesystem::path state_file_;
};

[[nodiscard]] AppPaths default_app_paths();
[[nodiscard]] AppConfig read_app_config(const std::filesystem::path &config_file);
[[nodiscard]] std::optional<std::string> normalize_stored_query(std::string_view query);

void add_history(AppState &state, std::string query, std::size_t limit);
[[nodiscard]] bool is_favorite(const AppState &state, std::string_view query);
[[nodiscard]] bool toggle_favorite(AppState &state, std::string query);

} // namespace rdap
