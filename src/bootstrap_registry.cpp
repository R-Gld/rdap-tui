// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/bootstrap_registry.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace rdap {
namespace {

Error bootstrap_error(std::string message, std::string detail = {}) {
  return Error{ErrorCode::bootstrap, std::move(message), std::move(detail), std::nullopt,
               std::nullopt};
}

bool suffix_matches(std::string_view domain, std::string_view entry) {
  if (entry.empty()) {
    return true;
  }
  if (domain == entry) {
    return true;
  }
  return domain.size() > entry.size() &&
         domain.compare(domain.size() - entry.size(), entry.size(), entry) == 0 &&
         domain[domain.size() - entry.size() - 1U] == '.';
}

std::size_t label_count(std::string_view entry) {
  if (entry.empty()) {
    return 0U;
  }
  return 1U + static_cast<std::size_t>(std::count(entry.begin(), entry.end(), '.'));
}

bool is_https_url(std::string_view url) {
  constexpr std::string_view prefix = "https://";
  return url.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), url.begin(), [](char left, char right) {
           return std::tolower(static_cast<unsigned char>(left)) ==
                  std::tolower(static_cast<unsigned char>(right));
         });
}

Result<nlohmann::json> parse_document(std::string_view document) {
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(document);
  } catch (const nlohmann::json::exception &exception) {
    return bootstrap_error("The IANA bootstrap document is not valid JSON.", exception.what());
  }
  if (!json.is_object() || !json.contains("version") || json["version"] != "1.0" ||
      !json.contains("services") || !json["services"].is_array()) {
    return bootstrap_error("The IANA bootstrap document has an unsupported structure.");
  }
  return json;
}

Result<std::vector<std::string>> service_urls(const nlohmann::json &service_json) {
  if (!service_json.is_array() || service_json.size() != 2U || !service_json[0].is_array() ||
      !service_json[1].is_array() || service_json[0].empty() || service_json[1].empty()) {
    return bootstrap_error("The IANA bootstrap document contains an invalid service entry.");
  }
  std::vector<std::string> urls;
  for (const auto &url_json : service_json[1]) {
    if (!url_json.is_string()) {
      return bootstrap_error("A bootstrap service URL is not a string.");
    }
    auto url = url_json.get<std::string>();
    if (url.empty() || url.back() != '/') {
      return bootstrap_error("A bootstrap service URL does not end with '/'.", url);
    }
    urls.push_back(std::move(url));
  }
  return urls;
}

void append_https_urls(std::vector<std::string> &target, std::unordered_set<std::string> &seen,
                       const std::vector<std::string> &urls) {
  for (const auto &url : urls) {
    if (is_https_url(url) && seen.insert(url).second) {
      target.push_back(url);
    }
  }
}

bool prefix_matches(const std::array<std::uint8_t, 16> &address,
                    const std::array<std::uint8_t, 16> &prefix, std::uint8_t length) {
  const auto full_bytes = static_cast<std::size_t>(length / 8U);
  const auto remaining = static_cast<unsigned int>(length % 8U);
  if (!std::equal(address.begin(), address.begin() + static_cast<std::ptrdiff_t>(full_bytes),
                  prefix.begin())) {
    return false;
  }
  if (remaining == 0U) {
    return true;
  }
  const auto mask = static_cast<std::uint8_t>(0xFFU << (8U - remaining));
  return (address[full_bytes] & mask) == (prefix[full_bytes] & mask);
}

} // namespace

Result<BootstrapRegistry> BootstrapRegistry::parse(std::string_view document) {
  auto document_result = parse_document(document);
  if (const auto *error = std::get_if<Error>(&document_result)) {
    return *error;
  }
  const auto &json = std::get<nlohmann::json>(document_result);

  BootstrapRegistry registry;
  for (const auto &service_json : json["services"]) {
    auto urls_result = service_urls(service_json);
    if (const auto *error = std::get_if<Error>(&urls_result)) {
      return *error;
    }

    Service service;
    service.urls = std::get<std::vector<std::string>>(std::move(urls_result));
    for (const auto &entry_json : service_json[0]) {
      if (!entry_json.is_string()) {
        return bootstrap_error("A bootstrap domain entry is not a string.");
      }
      auto entry = entry_json.get<std::string>();
      std::transform(entry.begin(), entry.end(), entry.begin(),
                     [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
      service.entries.push_back(std::move(entry));
    }
    registry.services_.push_back(std::move(service));
  }

  return registry;
}

Result<std::vector<std::string>> BootstrapRegistry::resolve(const DomainName &domain) const {
  std::size_t longest_match = 0U;
  bool found = false;
  std::vector<std::string> candidates;
  std::unordered_set<std::string> seen;

  for (const auto &service : services_) {
    for (const auto &entry : service.entries) {
      if (!suffix_matches(domain.ascii(), entry)) {
        continue;
      }
      const auto current_length = label_count(entry);
      if (!found || current_length > longest_match) {
        found = true;
        longest_match = current_length;
        candidates.clear();
        seen.clear();
      }
      if (current_length != longest_match) {
        continue;
      }
      append_https_urls(candidates, seen, service.urls);
    }
  }

  if (!found) {
    return bootstrap_error("No authoritative RDAP service was found for this domain.");
  }
  if (candidates.empty()) {
    return bootstrap_error("The authoritative RDAP service does not advertise an HTTPS endpoint.");
  }
  return candidates;
}

Result<IpBootstrapRegistry> IpBootstrapRegistry::parse(std::string_view document, IpFamily family) {
  auto document_result = parse_document(document);
  if (const auto *error = std::get_if<Error>(&document_result)) {
    return *error;
  }
  const auto &json = std::get<nlohmann::json>(document_result);
  IpBootstrapRegistry registry;
  registry.family_ = family;

  for (const auto &service_json : json["services"]) {
    auto urls_result = service_urls(service_json);
    if (const auto *error = std::get_if<Error>(&urls_result)) {
      return *error;
    }
    Service service;
    service.urls = std::get<std::vector<std::string>>(std::move(urls_result));
    for (const auto &entry_json : service_json[0]) {
      if (!entry_json.is_string()) {
        return bootstrap_error("A bootstrap IP prefix is not a string.");
      }
      auto parsed = IpNetworkQuery::parse(entry_json.get<std::string>());
      if (const auto *error = std::get_if<Error>(&parsed)) {
        return bootstrap_error("The IANA bootstrap document contains an invalid IP prefix.",
                               error->message);
      }
      const auto &prefix = std::get<IpNetworkQuery>(parsed);
      if (prefix.family() != family || !prefix.prefix_length().has_value()) {
        return bootstrap_error(
            "The IANA bootstrap document contains a prefix for the wrong address family.");
      }
      service.prefixes.push_back(Prefix{prefix.bytes(), *prefix.prefix_length()});
    }
    registry.services_.push_back(std::move(service));
  }
  return registry;
}

Result<std::vector<std::string>> IpBootstrapRegistry::resolve(const IpNetworkQuery &query) const {
  if (query.family() != family_) {
    return bootstrap_error("The IP query does not match this bootstrap registry.");
  }
  std::optional<std::uint8_t> longest;
  std::vector<std::string> candidates;
  std::unordered_set<std::string> seen;
  for (const auto &service : services_) {
    for (const auto &prefix : service.prefixes) {
      if (query.prefix_length().has_value() && prefix.length > *query.prefix_length()) {
        continue;
      }
      if (!prefix_matches(query.bytes(), prefix.bytes, prefix.length)) {
        continue;
      }
      if (!longest.has_value() || prefix.length > *longest) {
        longest = prefix.length;
        candidates.clear();
        seen.clear();
      }
      if (prefix.length == *longest) {
        append_https_urls(candidates, seen, service.urls);
      }
    }
  }
  if (!longest.has_value()) {
    return bootstrap_error("No authoritative RDAP service was found for this IP address.");
  }
  if (candidates.empty()) {
    return bootstrap_error("The authoritative RDAP service does not advertise an HTTPS endpoint.");
  }
  return candidates;
}

Result<AsnBootstrapRegistry> AsnBootstrapRegistry::parse(std::string_view document) {
  auto document_result = parse_document(document);
  if (const auto *error = std::get_if<Error>(&document_result)) {
    return *error;
  }
  const auto &json = std::get<nlohmann::json>(document_result);
  AsnBootstrapRegistry registry;
  std::vector<Range> all_ranges;

  for (const auto &service_json : json["services"]) {
    auto urls_result = service_urls(service_json);
    if (const auto *error = std::get_if<Error>(&urls_result)) {
      return *error;
    }
    Service service;
    service.urls = std::get<std::vector<std::string>>(std::move(urls_result));
    for (const auto &entry_json : service_json[0]) {
      if (!entry_json.is_string()) {
        return bootstrap_error("A bootstrap ASN range is not a string.");
      }
      const auto entry = entry_json.get<std::string>();
      const auto dash = entry.find('-');
      if (dash == std::string::npos || entry.find('-', dash + 1U) != std::string::npos) {
        return bootstrap_error("The IANA bootstrap document contains an invalid ASN range.", entry);
      }
      const auto parse_number = [&](std::string_view text) -> std::optional<std::uint32_t> {
        std::uint64_t value{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            value > std::numeric_limits<std::uint32_t>::max()) {
          return std::nullopt;
        }
        return static_cast<std::uint32_t>(value);
      };
      const auto start = parse_number(std::string_view(entry).substr(0U, dash));
      const auto end = parse_number(std::string_view(entry).substr(dash + 1U));
      if (!start.has_value() || !end.has_value() || *start > *end) {
        return bootstrap_error("The IANA bootstrap document contains an invalid ASN range.", entry);
      }
      const Range range{*start, *end};
      if (std::any_of(all_ranges.begin(), all_ranges.end(), [&](const Range &existing) {
            return range.start <= existing.end && existing.start <= range.end;
          })) {
        return bootstrap_error("The IANA bootstrap document contains overlapping ASN ranges.");
      }
      all_ranges.push_back(range);
      service.ranges.push_back(range);
    }
    registry.services_.push_back(std::move(service));
  }
  return registry;
}

Result<std::vector<std::string>> AsnBootstrapRegistry::resolve(const AutnumQuery &query) const {
  std::vector<std::string> candidates;
  std::unordered_set<std::string> seen;
  bool found = false;
  for (const auto &service : services_) {
    for (const auto &range : service.ranges) {
      if (query.number() >= range.start && query.number() <= range.end) {
        found = true;
        append_https_urls(candidates, seen, service.urls);
      }
    }
  }
  if (!found) {
    return bootstrap_error("No authoritative RDAP service was found for this ASN.");
  }
  if (candidates.empty()) {
    return bootstrap_error("The authoritative RDAP service does not advertise an HTTPS endpoint.");
  }
  return candidates;
}

} // namespace rdap
