// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
  DomainName query;
  std::string request_url;
  HttpResponse http;
  nlohmann::json document;
};

class RdapClient {
public:
  explicit RdapClient(HttpClient &http_client) : http_client_(http_client) {}

  Result<RdapResponse> lookup_domain(const DomainName &domain,
                                     const CancellationToken &cancellation,
                                     const ProgressCallback &progress = {});

private:
  Result<BootstrapRegistry> bootstrap(const CancellationToken &cancellation);

  HttpClient &http_client_;
  std::mutex bootstrap_mutex_;
  std::optional<BootstrapRegistry> bootstrap_;
};

} // namespace rdap
