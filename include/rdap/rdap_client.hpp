// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rdap/bootstrap_cache.hpp"
#include "rdap/bootstrap_registry.hpp"
#include "rdap/http.hpp"

#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace rdap {

enum class LookupStage { loading_bootstrap, querying_registry };
using ProgressCallback = std::function<void(LookupStage)>;

struct RdapResponse {
  ResourceQuery query;
  std::string request_url;
  HttpResponse http;
  nlohmann::json document;
};

class RdapClient {
public:
  // disk_cache is non-owning and may be nullptr to disable disk-backed
  // bootstrap caching entirely (behavior then matches the in-memory-only
  // caching this class always had).
  explicit RdapClient(HttpClient &http_client, BootstrapCache *disk_cache = nullptr)
      : http_client_(http_client), disk_cache_(disk_cache) {}

  Result<RdapResponse> lookup(const ResourceQuery &query, const CancellationToken &cancellation,
                              const ProgressCallback &progress = {});
  Result<RdapResponse> lookup_domain(const DomainName &domain,
                                     const CancellationToken &cancellation,
                                     const ProgressCallback &progress = {});

private:
  Result<BootstrapRegistry> domain_bootstrap(const CancellationToken &cancellation);
  Result<IpBootstrapRegistry> ip_bootstrap(IpFamily family, const CancellationToken &cancellation);
  Result<AsnBootstrapRegistry> asn_bootstrap(const CancellationToken &cancellation);

  HttpClient &http_client_;
  BootstrapCache *disk_cache_{nullptr};
  std::mutex bootstrap_mutex_;
  std::optional<BootstrapRegistry> domain_bootstrap_;
  std::optional<IpBootstrapRegistry> ipv4_bootstrap_;
  std::optional<IpBootstrapRegistry> ipv6_bootstrap_;
  std::optional<AsnBootstrapRegistry> asn_bootstrap_;
};

} // namespace rdap
