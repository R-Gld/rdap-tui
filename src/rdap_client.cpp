// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/rdap_client.hpp"

#include <iomanip>
#include <sstream>

namespace rdap {
namespace {

constexpr std::string_view domain_bootstrap_url = "https://data.iana.org/rdap/dns.json";
constexpr std::string_view ipv4_bootstrap_url = "https://data.iana.org/rdap/ipv4.json";
constexpr std::string_view ipv6_bootstrap_url = "https://data.iana.org/rdap/ipv6.json";
constexpr std::string_view asn_bootstrap_url = "https://data.iana.org/rdap/asn.json";

Error http_error(const HttpResponse &response, std::string message) {
  std::optional<std::string> retry_after;
  if (const auto iterator = response.headers.find("retry-after");
      iterator != response.headers.end()) {
    retry_after = iterator->second;
  }
  return Error{ErrorCode::http, std::move(message), response.body, response.status,
               std::move(retry_after)};
}

std::string encode_path_segment(std::string_view value) {
  std::ostringstream output;
  output << std::uppercase << std::hex;
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    const bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                            byte == '_' || byte == '~';
    if (unreserved) {
      output << character;
    } else {
      output << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte);
    }
  }
  return output.str();
}

std::optional<CachedBootstrap> updated_cache_entry(const HttpResponse &response,
                                                   std::chrono::system_clock::time_point now,
                                                   std::optional<std::string> body = std::nullopt) {
  CachedBootstrap entry;
  entry.body = body.has_value() ? std::move(*body) : response.body;
  entry.cached_at = now;
  entry.max_age = parse_cache_control_max_age(response.headers);
  if (const auto iterator = response.headers.find("etag"); iterator != response.headers.end()) {
    entry.etag = iterator->second;
  }
  if (const auto iterator = response.headers.find("last-modified");
      iterator != response.headers.end()) {
    entry.last_modified = iterator->second;
  }
  return entry;
}

// Fetches an IANA bootstrap registry, consulting the disk cache (if any) first:
// a fresh entry is served with zero network I/O, a stale entry is revalidated
// with a conditional GET, a 304 extends the freshness window, and a transport
// error or 5xx falls back to the stale body rather than failing the lookup. A
// 4xx or an absent cache behaves exactly as an unconditional fetch always has.
template <typename Registry>
Result<Registry> load_bootstrap(HttpClient &http_client, BootstrapCache *disk_cache,
                                BootstrapKind kind, std::string_view url,
                                const CancellationToken &cancellation,
                                const std::function<Result<Registry>(std::string_view)> &parser) {
  auto cached = disk_cache != nullptr ? disk_cache->read(kind) : std::nullopt;
  const auto now = std::chrono::system_clock::now();

  if (classify_bootstrap_freshness(cached, now) == BootstrapFreshness::fresh) {
    auto parsed = parser(cached->body);
    if (std::holds_alternative<Registry>(parsed)) {
      return parsed;
    }
    // A schema-valid but unparsable cached body must never fail a lookup;
    // fetch as if no cache were present instead.
    cached.reset();
  }

  HttpRequest request;
  request.url = std::string(url);
  request.headers = {"Accept: application/json"};
  request.maximum_body_size = 2U * 1024U * 1024U;
  if (cached.has_value()) {
    auto conditional_headers = build_conditional_headers(*cached);
    request.headers.insert(request.headers.end(), conditional_headers.begin(),
                           conditional_headers.end());
  }

  auto response_result = http_client.get(request, cancellation);
  if (const auto *error = std::get_if<Error>(&response_result)) {
    if (cached.has_value() && error->code != ErrorCode::cancelled) {
      return parser(cached->body);
    }
    return *error;
  }
  auto response = std::get<HttpResponse>(std::move(response_result));

  if (response.status == 304L && cached.has_value()) {
    auto refreshed = updated_cache_entry(response, now, cached->body);
    refreshed->etag = refreshed->etag.has_value() ? refreshed->etag : cached->etag;
    refreshed->last_modified =
        refreshed->last_modified.has_value() ? refreshed->last_modified : cached->last_modified;
    if (disk_cache != nullptr) {
      disk_cache->write(kind, *refreshed);
    }
    return parser(refreshed->body);
  }
  if (response.status >= 500L && response.status <= 599L && cached.has_value()) {
    return parser(cached->body);
  }
  if (response.status != 200L) {
    return http_error(response, "IANA returned an HTTP error for the bootstrap registry.");
  }

  auto parsed = parser(response.body);
  if (std::holds_alternative<Registry>(parsed) && disk_cache != nullptr) {
    disk_cache->write(kind, *updated_cache_entry(response, now));
  }
  return parsed;
}

} // namespace

Result<BootstrapRegistry> RdapClient::domain_bootstrap(const CancellationToken &cancellation) {
  std::scoped_lock lock(bootstrap_mutex_);
  if (domain_bootstrap_.has_value()) {
    return *domain_bootstrap_;
  }
  auto result = load_bootstrap<BootstrapRegistry>(
      http_client_, disk_cache_, BootstrapKind::domain, domain_bootstrap_url, cancellation,
      [](std::string_view body) { return BootstrapRegistry::parse(body); });
  if (const auto *error = std::get_if<Error>(&result)) {
    return *error;
  }
  domain_bootstrap_ = std::get<BootstrapRegistry>(std::move(result));
  return *domain_bootstrap_;
}

Result<IpBootstrapRegistry> RdapClient::ip_bootstrap(IpFamily family,
                                                     const CancellationToken &cancellation) {
  std::scoped_lock lock(bootstrap_mutex_);
  auto &cache = family == IpFamily::v4 ? ipv4_bootstrap_ : ipv6_bootstrap_;
  if (cache.has_value()) {
    return *cache;
  }
  const auto url = family == IpFamily::v4 ? ipv4_bootstrap_url : ipv6_bootstrap_url;
  const auto kind = family == IpFamily::v4 ? BootstrapKind::ipv4 : BootstrapKind::ipv6;
  auto result = load_bootstrap<IpBootstrapRegistry>(
      http_client_, disk_cache_, kind, url, cancellation,
      [family](std::string_view body) { return IpBootstrapRegistry::parse(body, family); });
  if (const auto *error = std::get_if<Error>(&result)) {
    return *error;
  }
  cache = std::get<IpBootstrapRegistry>(std::move(result));
  return *cache;
}

Result<AsnBootstrapRegistry> RdapClient::asn_bootstrap(const CancellationToken &cancellation) {
  std::scoped_lock lock(bootstrap_mutex_);
  if (asn_bootstrap_.has_value()) {
    return *asn_bootstrap_;
  }
  auto result = load_bootstrap<AsnBootstrapRegistry>(
      http_client_, disk_cache_, BootstrapKind::asn, asn_bootstrap_url, cancellation,
      [](std::string_view body) { return AsnBootstrapRegistry::parse(body); });
  if (const auto *error = std::get_if<Error>(&result)) {
    return *error;
  }
  asn_bootstrap_ = std::get<AsnBootstrapRegistry>(std::move(result));
  return *asn_bootstrap_;
}

Result<RdapResponse> RdapClient::lookup(const ResourceQuery &query,
                                        const CancellationToken &cancellation,
                                        const ProgressCallback &progress) {
  if (progress) {
    progress(LookupStage::loading_bootstrap);
  }

  Result<std::vector<std::string>> services_result = std::visit(
      [&](const auto &value) -> Result<std::vector<std::string>> {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, DomainName>) {
          auto registry = domain_bootstrap(cancellation);
          if (const auto *error = std::get_if<Error>(&registry)) {
            return *error;
          }
          return std::get<BootstrapRegistry>(registry).resolve(value);
        } else if constexpr (std::is_same_v<Value, IpNetworkQuery>) {
          auto registry = ip_bootstrap(value.family(), cancellation);
          if (const auto *error = std::get_if<Error>(&registry)) {
            return *error;
          }
          return std::get<IpBootstrapRegistry>(registry).resolve(value);
        } else {
          auto registry = asn_bootstrap(cancellation);
          if (const auto *error = std::get_if<Error>(&registry)) {
            return *error;
          }
          return std::get<AsnBootstrapRegistry>(registry).resolve(value);
        }
      },
      query);
  if (const auto *error = std::get_if<Error>(&services_result)) {
    return *error;
  }
  const auto &services = std::get<std::vector<std::string>>(services_result);
  const auto path = std::visit(
      [](const auto &value) -> std::string {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, DomainName>) {
          return "domain/" + encode_path_segment(value.ascii());
        } else if constexpr (std::is_same_v<Value, IpNetworkQuery>) {
          return "ip/" + std::string(value.canonical());
        } else {
          return "autnum/" + std::to_string(value.number());
        }
      },
      query);

  if (progress) {
    progress(LookupStage::querying_registry);
  }
  std::optional<Error> last_retryable_error;
  for (const auto &base_url : services) {
    HttpRequest request;
    request.url = base_url + path;
    request.headers = {"Accept: application/rdap+json, application/json"};

    auto response_result = http_client_.get(request, cancellation);
    if (const auto *error = std::get_if<Error>(&response_result)) {
      if (error->code == ErrorCode::cancelled) {
        return *error;
      }
      last_retryable_error = *error;
      continue;
    }

    auto response = std::get<HttpResponse>(std::move(response_result));
    if (response.status >= 500L && response.status <= 599L) {
      last_retryable_error = http_error(response, "The RDAP service returned a server error.");
      continue;
    }
    if (response.status != 200L) {
      return http_error(response, "The RDAP service returned an HTTP error.");
    }

    try {
      auto document = nlohmann::json::parse(response.body);
      if (!document.is_object()) {
        return Error{ErrorCode::invalid_json,
                     "The RDAP response is not a JSON object.",
                     {},
                     response.status,
                     std::nullopt};
      }
      const auto request_url = request.url;
      return RdapResponse{query, request_url, std::move(response), std::move(document)};
    } catch (const nlohmann::json::exception &exception) {
      return Error{ErrorCode::invalid_json, "The RDAP response is not valid JSON.",
                   exception.what(), response.status, std::nullopt};
    }
  }

  if (last_retryable_error.has_value()) {
    return *last_retryable_error;
  }
  return Error{
      ErrorCode::bootstrap, "No usable RDAP service is available.", {}, std::nullopt, std::nullopt};
}

Result<RdapResponse> RdapClient::lookup_domain(const DomainName &domain,
                                               const CancellationToken &cancellation,
                                               const ProgressCallback &progress) {
  return lookup(ResourceQuery{domain}, cancellation, progress);
}

} // namespace rdap
