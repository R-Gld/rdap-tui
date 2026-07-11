// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rdap {

enum class BootstrapKind { domain, ipv4, ipv6, asn };

enum class BootstrapFreshness { fresh, stale, absent };

struct CachedBootstrap {
  std::string body;
  std::optional<std::string> etag;
  std::optional<std::string> last_modified;
  std::chrono::system_clock::time_point cached_at;
  std::chrono::seconds max_age{std::chrono::hours(24)};
};

// Pure policy functions: "now" is always an explicit parameter so freshness
// decisions stay unit-testable without touching the system clock.
[[nodiscard]] BootstrapFreshness
classify_bootstrap_freshness(const std::optional<CachedBootstrap> &cached,
                             std::chrono::system_clock::time_point now);

[[nodiscard]] std::chrono::seconds
parse_cache_control_max_age(const std::map<std::string, std::string, std::less<>> &response_headers,
                            std::chrono::seconds fallback = std::chrono::hours(24));

[[nodiscard]] std::vector<std::string> build_conditional_headers(const CachedBootstrap &cached);

// Persists the four IANA bootstrap registries to disk so a fresh process can
// reuse a previous fetch instead of always querying IANA. All failures
// (missing/corrupt file, unwritable directory) are absorbed internally and
// reported as std::nullopt / false -- this is a resilience/performance layer
// only, and must never be the reason a lookup fails.
class BootstrapCache {
public:
  explicit BootstrapCache(std::filesystem::path directory);

  [[nodiscard]] std::optional<CachedBootstrap> read(BootstrapKind kind) const;
  bool write(BootstrapKind kind, const CachedBootstrap &entry) const;

private:
  [[nodiscard]] std::filesystem::path entry_path(BootstrapKind kind) const;

  std::filesystem::path directory_;
};

// Resolves the OS-appropriate default cache directory. Returns an empty path
// if it cannot be determined; callers must treat that as "disk cache
// unavailable" rather than an error.
[[nodiscard]] std::filesystem::path default_bootstrap_cache_directory();

} // namespace rdap
