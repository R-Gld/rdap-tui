// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/bootstrap_registry.hpp"

#include <algorithm>
#include <cctype>
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

} // namespace

Result<BootstrapRegistry> BootstrapRegistry::parse(std::string_view document) {
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

  BootstrapRegistry registry;
  for (const auto &service_json : json["services"]) {
    if (!service_json.is_array() || service_json.size() != 2U || !service_json[0].is_array() ||
        !service_json[1].is_array() || service_json[0].empty() || service_json[1].empty()) {
      return bootstrap_error("The IANA bootstrap document contains an invalid service entry.");
    }

    Service service;
    for (const auto &entry_json : service_json[0]) {
      if (!entry_json.is_string()) {
        return bootstrap_error("A bootstrap domain entry is not a string.");
      }
      auto entry = entry_json.get<std::string>();
      std::transform(entry.begin(), entry.end(), entry.begin(),
                     [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
      service.entries.push_back(std::move(entry));
    }
    for (const auto &url_json : service_json[1]) {
      if (!url_json.is_string()) {
        return bootstrap_error("A bootstrap service URL is not a string.");
      }
      auto url = url_json.get<std::string>();
      if (url.empty() || url.back() != '/') {
        return bootstrap_error("A bootstrap service URL does not end with '/'.", url);
      }
      service.urls.push_back(std::move(url));
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
      for (const auto &url : service.urls) {
        if (is_https_url(url) && seen.insert(url).second) {
          candidates.push_back(url);
        }
      }
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

} // namespace rdap
