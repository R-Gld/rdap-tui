// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/app_state.hpp"

#include "rdap/resource_query.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>
#include <variant>

namespace rdap {
namespace {

constexpr int schema_version = 1;
constexpr std::size_t maximum_history_limit = 1000U;

std::optional<std::filesystem::path> absolute_environment_path(const char *name) {
  const auto *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  std::filesystem::path path(value);
  if (!path.is_absolute()) {
    return std::nullopt;
  }
  return path;
}

#if !defined(_WIN32) && !defined(__APPLE__)
std::filesystem::path linux_config_directory() {
  if (const auto xdg_config_home = absolute_environment_path("XDG_CONFIG_HOME");
      xdg_config_home.has_value()) {
    return *xdg_config_home / "rdap-tui";
  }
  const auto home = absolute_environment_path("HOME");
  if (!home.has_value()) {
    return {};
  }
  return *home / ".config" / "rdap-tui";
}

std::filesystem::path linux_state_directory() {
  if (const auto xdg_state_home = absolute_environment_path("XDG_STATE_HOME");
      xdg_state_home.has_value()) {
    return *xdg_state_home / "rdap-tui";
  }
  const auto home = absolute_environment_path("HOME");
  if (!home.has_value()) {
    return {};
  }
  return *home / ".local" / "state" / "rdap-tui";
}
#endif

std::vector<std::string> normalized_queries_from_json(const nlohmann::json &json) {
  std::vector<std::string> queries;
  if (!json.is_array()) {
    return queries;
  }
  for (const auto &entry : json) {
    if (!entry.is_string()) {
      continue;
    }
    auto normalized = normalize_stored_query(entry.get<std::string>());
    if (!normalized.has_value() || std::find(queries.begin(), queries.end(), *normalized) !=
                                      queries.end()) {
      continue;
    }
    queries.push_back(std::move(*normalized));
  }
  return queries;
}

bool write_json_atomically(const std::filesystem::path &path, const nlohmann::json &json) {
  try {
    std::error_code creation_error;
    std::filesystem::create_directories(path.parent_path(), creation_error);
    if (creation_error) {
      return false;
    }

    auto temporary_path = path;
    temporary_path += ".tmp";
    {
      std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
      if (!output.is_open()) {
        return false;
      }
      output << json.dump(2);
      output << '\n';
      if (!output.good()) {
        return false;
      }
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary_path, path, rename_error);
    return !rename_error;
  } catch (...) {
    return false;
  }
}

} // namespace

AppStateStore::AppStateStore(std::filesystem::path state_file) : state_file_(std::move(state_file)) {}

AppState AppStateStore::read() const {
  try {
    std::ifstream input(state_file_, std::ios::binary);
    if (!input.is_open()) {
      return {};
    }
    nlohmann::json json;
    input >> json;
    if (!json.is_object() || json.value("schema_version", 0) != schema_version) {
      return {};
    }
    AppState state;
    state.history = normalized_queries_from_json(json.value("history", nlohmann::json::array()));
    state.favorites =
        normalized_queries_from_json(json.value("favorites", nlohmann::json::array()));
    return state;
  } catch (...) {
    return {};
  }
}

bool AppStateStore::write(const AppState &state) const {
  nlohmann::json json;
  json["schema_version"] = schema_version;
  json["history"] = state.history;
  json["favorites"] = state.favorites;
  return write_json_atomically(state_file_, json);
}

AppPaths default_app_paths() {
#if defined(_WIN32)
  return {};
#elif defined(__APPLE__)
  const auto home = absolute_environment_path("HOME");
  if (!home.has_value()) {
    return {};
  }
  const auto directory = *home / "Library" / "Application Support" / "rdap-tui";
  return AppPaths{directory / "config.json", directory / "state.json"};
#else
  const auto config_directory = linux_config_directory();
  const auto state_directory = linux_state_directory();
  return AppPaths{config_directory.empty() ? std::filesystem::path{} : config_directory / "config.json",
                  state_directory.empty() ? std::filesystem::path{} : state_directory / "state.json"};
#endif
}

AppConfig read_app_config(const std::filesystem::path &config_file) {
  AppConfig config;
  if (config_file.empty()) {
    return config;
  }
  try {
    std::ifstream input(config_file, std::ios::binary);
    if (!input.is_open()) {
      return config;
    }
    nlohmann::json json;
    input >> json;
    if (!json.is_object() || json.value("schema_version", 0) != schema_version) {
      return config;
    }
    if (json.contains("history_limit") && json["history_limit"].is_number_unsigned()) {
      config.history_limit =
          std::min(json["history_limit"].get<std::size_t>(), maximum_history_limit);
    }
    return config;
  } catch (...) {
    return config;
  }
}

std::optional<std::string> normalize_stored_query(std::string_view query) {
  auto parsed = ResourceQueryParser::parse(query);
  if (std::holds_alternative<Error>(parsed)) {
    return std::nullopt;
  }
  return query_text(std::get<ResourceQuery>(std::move(parsed)));
}

void add_history(AppState &state, std::string query, std::size_t limit) {
  auto normalized = normalize_stored_query(query);
  if (!normalized.has_value()) {
    return;
  }
  state.history.erase(std::remove(state.history.begin(), state.history.end(), *normalized),
                      state.history.end());
  if (limit == 0U) {
    return;
  }
  state.history.insert(state.history.begin(), std::move(*normalized));
  if (state.history.size() > limit) {
    state.history.resize(limit);
  }
}

bool is_favorite(const AppState &state, std::string_view query) {
  return std::find(state.favorites.begin(), state.favorites.end(), query) != state.favorites.end();
}

bool toggle_favorite(AppState &state, std::string query) {
  auto normalized = normalize_stored_query(query);
  if (!normalized.has_value()) {
    return false;
  }
  const auto iterator = std::find(state.favorites.begin(), state.favorites.end(), *normalized);
  if (iterator != state.favorites.end()) {
    state.favorites.erase(iterator);
    return false;
  }
  state.favorites.push_back(std::move(*normalized));
  return true;
}

} // namespace rdap
