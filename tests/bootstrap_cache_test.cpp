// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/bootstrap_cache.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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
    // Each Catch2 TEST_CASE runs as its own process under ctest (which runs
    // several in parallel), so uniqueness must hold across processes, not
    // just within one -- a per-process counter alone is not enough.
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

#if !defined(_WIN32) && !defined(__APPLE__)
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
#endif

CachedBootstrap sample_entry() {
  CachedBootstrap entry;
  entry.body = R"({"version":"1.0","services":[]})";
  entry.etag = "\"abc123\"";
  entry.last_modified = "Wed, 21 Oct 2015 07:28:00 GMT";
  entry.cached_at = std::chrono::system_clock::now();
  entry.max_age = std::chrono::hours(24);
  return entry;
}

} // namespace

TEST_CASE("parses max-age from a cache-control header") {
  const std::map<std::string, std::string, std::less<>> headers{{"cache-control", "max-age=86400"}};
  CHECK(parse_cache_control_max_age(headers) == std::chrono::seconds(86400));
}

TEST_CASE("max-age parsing ignores casing and other directives") {
  const std::map<std::string, std::string, std::less<>> headers{
      {"cache-control", "public, MAX-AGE=3600, immutable"}};
  CHECK(parse_cache_control_max_age(headers) == std::chrono::seconds(3600));
}

TEST_CASE("max-age parsing falls back to a default when the header is absent") {
  const std::map<std::string, std::string, std::less<>> headers;
  CHECK(parse_cache_control_max_age(headers, std::chrono::hours(1)) == std::chrono::hours(1));
}

TEST_CASE("max-age parsing falls back when the value is not a valid integer") {
  const std::map<std::string, std::string, std::less<>> headers{{"cache-control", "max-age=soon"}};
  CHECK(parse_cache_control_max_age(headers, std::chrono::hours(1)) == std::chrono::hours(1));
}

TEST_CASE("classifies an absent cache entry") {
  CHECK(classify_bootstrap_freshness(std::nullopt, std::chrono::system_clock::now()) ==
        BootstrapFreshness::absent);
}

TEST_CASE("classifies a fresh cache entry within max-age") {
  auto entry = sample_entry();
  entry.max_age = std::chrono::hours(24);
  const auto now = entry.cached_at + std::chrono::hours(1);
  CHECK(classify_bootstrap_freshness(entry, now) == BootstrapFreshness::fresh);
}

TEST_CASE("classifies a stale cache entry past max-age") {
  auto entry = sample_entry();
  entry.max_age = std::chrono::hours(1);
  const auto now = entry.cached_at + std::chrono::hours(2);
  CHECK(classify_bootstrap_freshness(entry, now) == BootstrapFreshness::stale);
}

TEST_CASE("clamps a clock that moved backward to a fresh entry") {
  auto entry = sample_entry();
  entry.max_age = std::chrono::hours(1);
  const auto now = entry.cached_at - std::chrono::hours(2);
  CHECK(classify_bootstrap_freshness(entry, now) == BootstrapFreshness::fresh);
}

TEST_CASE("builds conditional headers from etag and last-modified") {
  const auto headers = build_conditional_headers(sample_entry());
  REQUIRE(headers.size() == 2U);
  CHECK(headers[0] == "If-None-Match: \"abc123\"");
  CHECK(headers[1] == "If-Modified-Since: Wed, 21 Oct 2015 07:28:00 GMT");
}

TEST_CASE("builds no conditional headers when neither etag nor last-modified is present") {
  CachedBootstrap entry;
  entry.body = "{}";
  entry.cached_at = std::chrono::system_clock::now();
  CHECK(build_conditional_headers(entry).empty());
}

TEST_CASE("round-trips a cache entry to disk and back") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  const auto entry = sample_entry();

  REQUIRE(cache.write(BootstrapKind::domain, entry));
  const auto read_back = cache.read(BootstrapKind::domain);

  REQUIRE(read_back.has_value());
  CHECK(read_back->body == entry.body);
  CHECK(read_back->etag == entry.etag);
  CHECK(read_back->last_modified == entry.last_modified);
  CHECK(read_back->max_age == entry.max_age);
  CHECK(std::chrono::duration_cast<std::chrono::seconds>(read_back->cached_at.time_since_epoch()) ==
        std::chrono::duration_cast<std::chrono::seconds>(entry.cached_at.time_since_epoch()));
}

TEST_CASE("cached bootstrap kinds do not collide on disk") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  auto domain_entry = sample_entry();
  domain_entry.body = "domain";
  auto asn_entry = sample_entry();
  asn_entry.body = "asn";

  REQUIRE(cache.write(BootstrapKind::domain, domain_entry));
  REQUIRE(cache.write(BootstrapKind::asn, asn_entry));

  REQUIRE(cache.read(BootstrapKind::domain)->body == "domain");
  REQUIRE(cache.read(BootstrapKind::asn)->body == "asn");
}

TEST_CASE("read returns nullopt for a missing cache file") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  CHECK_FALSE(cache.read(BootstrapKind::domain).has_value());
}

TEST_CASE("read returns nullopt for a corrupt cache file") {
  TempDirectory directory;
  std::ofstream(directory.path() / "dns.json") << "not json";
  BootstrapCache cache(directory.path());
  CHECK_FALSE(cache.read(BootstrapKind::domain).has_value());
}

TEST_CASE("read returns nullopt for an unsupported schema version") {
  TempDirectory directory;
  std::ofstream(directory.path() / "dns.json")
      << R"({"schema_version":99,"body":"{}","cached_at_unix":0,"max_age_seconds":86400})";
  BootstrapCache cache(directory.path());
  CHECK_FALSE(cache.read(BootstrapKind::domain).has_value());
}

TEST_CASE("write is atomic and leaves no temporary file behind") {
  TempDirectory directory;
  BootstrapCache cache(directory.path());
  REQUIRE(cache.write(BootstrapKind::domain, sample_entry()));

  CHECK(std::filesystem::exists(directory.path() / "dns.json"));
  CHECK_FALSE(std::filesystem::exists(directory.path() / "dns.json.tmp"));
}

TEST_CASE("write to an unwritable location fails without throwing") {
  TempDirectory directory;
  const auto blocked = directory.path() / "blocked";
  std::ofstream(blocked) << "not a directory";
  BootstrapCache cache(blocked / "bootstrap");

  CHECK_FALSE(cache.write(BootstrapKind::domain, sample_entry()));
  CHECK_FALSE(cache.read(BootstrapKind::domain).has_value());
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("Linux cache directory prefers an absolute XDG_CACHE_HOME") {
  TempDirectory directory;
  EnvironmentVariableGuard xdg("XDG_CACHE_HOME");
  EnvironmentVariableGuard home("HOME");
  const auto xdg_cache_home = directory.path() / "xdg-cache";
  const auto home_directory = directory.path() / "home";
  xdg.set(xdg_cache_home.string());
  home.set(home_directory.string());

  CHECK(default_bootstrap_cache_directory() == xdg_cache_home / "rdap-tui" / "bootstrap");
}

TEST_CASE("Linux cache directory falls back when XDG_CACHE_HOME is empty") {
  TempDirectory directory;
  EnvironmentVariableGuard xdg("XDG_CACHE_HOME");
  EnvironmentVariableGuard home("HOME");
  const auto home_directory = directory.path() / "home";
  xdg.set("");
  home.set(home_directory.string());

  CHECK(default_bootstrap_cache_directory() ==
        home_directory / ".cache" / "rdap-tui" / "bootstrap");
}

TEST_CASE("Linux cache directory ignores a relative XDG_CACHE_HOME") {
  TempDirectory directory;
  EnvironmentVariableGuard xdg("XDG_CACHE_HOME");
  EnvironmentVariableGuard home("HOME");
  const auto home_directory = directory.path() / "home";
  xdg.set("relative/cache");
  home.set(home_directory.string());

  CHECK(default_bootstrap_cache_directory() ==
        home_directory / ".cache" / "rdap-tui" / "bootstrap");
}

TEST_CASE("Linux cache directory is unavailable when HOME is not absolute") {
  EnvironmentVariableGuard xdg("XDG_CACHE_HOME");
  EnvironmentVariableGuard home("HOME");
  xdg.unset();
  home.set("relative/home");

  CHECK(default_bootstrap_cache_directory().empty());
}

TEST_CASE("Linux cache directory is unavailable when HOME is empty") {
  EnvironmentVariableGuard xdg("XDG_CACHE_HOME");
  EnvironmentVariableGuard home("HOME");
  xdg.unset();
  home.set("");

  CHECK(default_bootstrap_cache_directory().empty());
}
#endif
