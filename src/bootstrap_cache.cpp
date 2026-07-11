// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/bootstrap_cache.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace rdap {
namespace {

constexpr int schema_version = 1;

std::string lowercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

std::string trim(std::string_view value) {
  const auto begin = value.find_first_not_of(" \t");
  if (begin == std::string_view::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t");
  return std::string(value.substr(begin, end - begin + 1U));
}

std::string_view filename_for(BootstrapKind kind) {
  switch (kind) {
  case BootstrapKind::domain:
    return "dns.json";
  case BootstrapKind::ipv4:
    return "ipv4.json";
  case BootstrapKind::ipv6:
    return "ipv6.json";
  case BootstrapKind::asn:
    return "asn.json";
  }
  return "unknown.json";
}

std::string source_url_for(BootstrapKind kind) {
  return "https://data.iana.org/rdap/" + std::string(filename_for(kind));
}

} // namespace

BootstrapFreshness classify_bootstrap_freshness(const std::optional<CachedBootstrap> &cached,
                                                std::chrono::system_clock::time_point now) {
  if (!cached.has_value()) {
    return BootstrapFreshness::absent;
  }
  auto age = now - cached->cached_at;
  if (age < std::chrono::system_clock::duration::zero()) {
    age = std::chrono::system_clock::duration::zero();
  }
  return age < cached->max_age ? BootstrapFreshness::fresh : BootstrapFreshness::stale;
}

std::chrono::seconds
parse_cache_control_max_age(const std::map<std::string, std::string, std::less<>> &response_headers,
                            std::chrono::seconds fallback) {
  const auto iterator = response_headers.find("cache-control");
  if (iterator == response_headers.end()) {
    return fallback;
  }
  std::stringstream stream{iterator->second};
  std::string token;
  while (std::getline(stream, token, ',')) {
    const auto trimmed = trim(token);
    const auto lowered = lowercase(trimmed);
    constexpr std::string_view prefix = "max-age=";
    if (lowered.size() > prefix.size() && lowered.compare(0, prefix.size(), prefix) == 0) {
      const auto digits = trimmed.substr(prefix.size());
      long long seconds{};
      const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), seconds);
      if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() && seconds >= 0) {
        return std::chrono::seconds(seconds);
      }
    }
  }
  return fallback;
}

std::vector<std::string> build_conditional_headers(const CachedBootstrap &cached) {
  std::vector<std::string> headers;
  if (cached.etag.has_value()) {
    headers.push_back("If-None-Match: " + *cached.etag);
  }
  if (cached.last_modified.has_value()) {
    headers.push_back("If-Modified-Since: " + *cached.last_modified);
  }
  return headers;
}

BootstrapCache::BootstrapCache(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::filesystem::path BootstrapCache::entry_path(BootstrapKind kind) const {
  return directory_ / filename_for(kind);
}

std::optional<CachedBootstrap> BootstrapCache::read(BootstrapKind kind) const {
  try {
    std::ifstream input(entry_path(kind), std::ios::binary);
    if (!input.is_open()) {
      return std::nullopt;
    }
    nlohmann::json envelope;
    input >> envelope;
    if (!envelope.is_object() || !envelope.contains("schema_version") ||
        envelope["schema_version"] != schema_version || !envelope.contains("body") ||
        !envelope["body"].is_string() || !envelope.contains("cached_at_unix") ||
        !envelope["cached_at_unix"].is_number_integer() || !envelope.contains("max_age_seconds") ||
        !envelope["max_age_seconds"].is_number_integer()) {
      return std::nullopt;
    }

    CachedBootstrap cached;
    cached.body = envelope["body"].get<std::string>();
    cached.cached_at = std::chrono::system_clock::time_point(
        std::chrono::seconds(envelope["cached_at_unix"].get<long long>()));
    cached.max_age = std::chrono::seconds(envelope["max_age_seconds"].get<long long>());
    if (envelope.contains("etag") && envelope["etag"].is_string()) {
      cached.etag = envelope["etag"].get<std::string>();
    }
    if (envelope.contains("last_modified") && envelope["last_modified"].is_string()) {
      cached.last_modified = envelope["last_modified"].get<std::string>();
    }
    return cached;
  } catch (...) {
    return std::nullopt;
  }
}

bool BootstrapCache::write(BootstrapKind kind, const CachedBootstrap &entry) const {
  try {
    std::error_code creation_error;
    std::filesystem::create_directories(directory_, creation_error);

    nlohmann::json envelope;
    envelope["schema_version"] = schema_version;
    envelope["source_url"] = source_url_for(kind);
    envelope["body"] = entry.body;
    envelope["cached_at_unix"] =
        std::chrono::duration_cast<std::chrono::seconds>(entry.cached_at.time_since_epoch())
            .count();
    envelope["max_age_seconds"] = entry.max_age.count();
    if (entry.etag.has_value()) {
      envelope["etag"] = *entry.etag;
    }
    if (entry.last_modified.has_value()) {
      envelope["last_modified"] = *entry.last_modified;
    }

    const auto path = entry_path(kind);
    auto temporary_path = path;
    temporary_path += ".tmp";
    {
      std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
      if (!output.is_open()) {
        return false;
      }
      output << envelope;
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

std::filesystem::path default_bootstrap_cache_directory() {
#if defined(_WIN32)
  const auto *local_app_data = std::getenv("LOCALAPPDATA");
  if (local_app_data == nullptr || *local_app_data == '\0') {
    return {};
  }
  return std::filesystem::path(local_app_data) / "rdap-tui" / "bootstrap";
#elif defined(__APPLE__)
  const auto *home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return {};
  }
  return std::filesystem::path(home) / "Library" / "Caches" / "rdap-tui" / "bootstrap";
#else
  const auto *xdg_cache_home = std::getenv("XDG_CACHE_HOME");
  if (xdg_cache_home != nullptr && *xdg_cache_home != '\0') {
    return std::filesystem::path(xdg_cache_home) / "rdap-tui" / "bootstrap";
  }
  const auto *home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return {};
  }
  return std::filesystem::path(home) / ".cache" / "rdap-tui" / "bootstrap";
#endif
}

} // namespace rdap
