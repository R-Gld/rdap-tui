// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/app_state.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

using namespace rdap;

namespace {

class TempDirectory {
public:
  TempDirectory() {
    std::random_device random_device;
    const auto unique = (static_cast<std::uint64_t>(random_device()) << 32U) |
                        static_cast<std::uint64_t>(random_device());
    path_ = std::filesystem::temp_directory_path() / ("rdap-tui-test-" + std::to_string(unique));
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TempDirectory(const TempDirectory &) = delete;
  TempDirectory &operator=(const TempDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

class EnvironmentVariableGuard {
public:
  explicit EnvironmentVariableGuard(std::string name) : name_(std::move(name)) {
    const auto *current = std::getenv(name_.c_str());
    if (current != nullptr) {
      original_ = current;
    }
  }

  ~EnvironmentVariableGuard() {
    if (original_.has_value()) {
      setenv(name_.c_str(), original_->c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  EnvironmentVariableGuard(const EnvironmentVariableGuard &) = delete;
  EnvironmentVariableGuard &operator=(const EnvironmentVariableGuard &) = delete;

  void set(std::string_view value) const { setenv(name_.c_str(), std::string(value).c_str(), 1); }
  void unset() const { unsetenv(name_.c_str()); }

private:
  std::string name_;
  std::optional<std::string> original_;
};

} // namespace

TEST_CASE("normalizes stored resource queries") {
  CHECK(normalize_stored_query("Example.COM") == "example.com");
  CHECK(normalize_stored_query("192.0.2.42/24") == "192.0.2.0/24");
  CHECK(normalize_stored_query("as13335") == "AS13335");
  CHECK_FALSE(normalize_stored_query("2001:db8::1%en0").has_value());
}

TEST_CASE("history deduplicates moves to front and honors limit") {
  AppState state;

  add_history(state, "example.com", 3U);
  add_history(state, "AS13335", 3U);
  add_history(state, "192.0.2.42/24", 3U);
  add_history(state, "EXAMPLE.COM", 3U);

  CHECK(state.history == std::vector<std::string>{"example.com", "192.0.2.0/24", "AS13335"});

  add_history(state, "1.1.1.1", 0U);
  CHECK(state.history == std::vector<std::string>{"example.com", "192.0.2.0/24", "AS13335"});
}

TEST_CASE("favorites toggle idempotently") {
  AppState state;

  CHECK(toggle_favorite(state, "as13335"));
  CHECK(is_favorite(state, "AS13335"));
  CHECK_FALSE(toggle_favorite(state, "AS13335"));
  CHECK_FALSE(is_favorite(state, "AS13335"));
}

TEST_CASE("state store round-trips normalized entries") {
  TempDirectory directory;
  AppStateStore store(directory.path() / "state.json");
  AppState state;
  state.history = {"Example.COM", "invalid space"};
  state.favorites = {"as13335", "as13335", "192.0.2.42/24"};

  REQUIRE(store.write(state));
  const auto read_back = store.read();

  CHECK(read_back.history == std::vector<std::string>{"example.com"});
  CHECK(read_back.favorites == std::vector<std::string>{"AS13335", "192.0.2.0/24"});
}

TEST_CASE("state store returns empty state for missing corrupt or unsupported files") {
  TempDirectory directory;
  CHECK(AppStateStore(directory.path() / "missing.json").read().history.empty());

  std::ofstream(directory.path() / "corrupt.json") << "not json";
  CHECK(AppStateStore(directory.path() / "corrupt.json").read().favorites.empty());

  std::ofstream(directory.path() / "future.json") << R"({"schema_version":99})";
  CHECK(AppStateStore(directory.path() / "future.json").read().history.empty());
}

TEST_CASE("config reads bounded history limit and falls back on invalid input") {
  TempDirectory directory;
  const auto config_file = directory.path() / "config.json";

  CHECK(read_app_config(config_file).history_limit == 200U);

  std::ofstream(config_file) << R"({"schema_version":1,"history_limit":5000})";
  CHECK(read_app_config(config_file).history_limit == 1000U);

  std::ofstream(config_file, std::ios::trunc) << R"({"schema_version":1,"history_limit":"many"})";
  CHECK(read_app_config(config_file).history_limit == 200U);
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("Linux app paths prefer absolute XDG directories") {
  TempDirectory directory;
  EnvironmentVariableGuard xdg_config("XDG_CONFIG_HOME");
  EnvironmentVariableGuard xdg_state("XDG_STATE_HOME");
  EnvironmentVariableGuard home("HOME");
  xdg_config.set((directory.path() / "config").string());
  xdg_state.set((directory.path() / "state").string());
  home.set((directory.path() / "home").string());

  const auto paths = default_app_paths();

  CHECK(paths.config_file == directory.path() / "config" / "rdap-tui" / "config.json");
  CHECK(paths.state_file == directory.path() / "state" / "rdap-tui" / "state.json");
}

TEST_CASE("Linux app paths ignore relative XDG directories and fall back to HOME") {
  TempDirectory directory;
  EnvironmentVariableGuard xdg_config("XDG_CONFIG_HOME");
  EnvironmentVariableGuard xdg_state("XDG_STATE_HOME");
  EnvironmentVariableGuard home("HOME");
  xdg_config.set("relative/config");
  xdg_state.set("relative/state");
  home.set((directory.path() / "home").string());

  const auto paths = default_app_paths();

  CHECK(paths.config_file == directory.path() / "home" / ".config" / "rdap-tui" / "config.json");
  CHECK(paths.state_file ==
        directory.path() / "home" / ".local" / "state" / "rdap-tui" / "state.json");
}

TEST_CASE("Linux app paths are unavailable when HOME and XDG paths are invalid") {
  EnvironmentVariableGuard xdg_config("XDG_CONFIG_HOME");
  EnvironmentVariableGuard xdg_state("XDG_STATE_HOME");
  EnvironmentVariableGuard home("HOME");
  xdg_config.unset();
  xdg_state.set("relative/state");
  home.set("relative/home");

  const auto paths = default_app_paths();

  CHECK(paths.config_file.empty());
  CHECK(paths.state_file.empty());
}
#endif
