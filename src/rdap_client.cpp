// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/rdap_client.hpp"

#include <iomanip>
#include <sstream>

namespace rdap {
namespace {

constexpr std::string_view bootstrap_url = "https://data.iana.org/rdap/dns.json";

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

} // namespace

Result<BootstrapRegistry> RdapClient::bootstrap(const CancellationToken &cancellation) {
  std::scoped_lock lock(bootstrap_mutex_);
  if (bootstrap_.has_value()) {
    return *bootstrap_;
  }

  HttpRequest request;
  request.url = std::string(bootstrap_url);
  request.headers = {"Accept: application/json"};
  request.maximum_body_size = 2U * 1024U * 1024U;

  auto response_result = http_client_.get(request, cancellation);
  if (const auto *error = std::get_if<Error>(&response_result)) {
    return *error;
  }
  auto response = std::get<HttpResponse>(std::move(response_result));
  if (response.status != 200L) {
    return http_error(response, "IANA returned an HTTP error for the bootstrap registry.");
  }

  auto registry_result = BootstrapRegistry::parse(response.body);
  if (const auto *error = std::get_if<Error>(&registry_result)) {
    return *error;
  }
  bootstrap_ = std::get<BootstrapRegistry>(std::move(registry_result));
  return *bootstrap_;
}

Result<RdapResponse> RdapClient::lookup_domain(const DomainName &domain,
                                               const CancellationToken &cancellation,
                                               const ProgressCallback &progress) {
  if (progress) {
    progress(LookupStage::loading_bootstrap);
  }
  auto registry_result = bootstrap(cancellation);
  if (const auto *error = std::get_if<Error>(&registry_result)) {
    return *error;
  }

  auto services_result = std::get<BootstrapRegistry>(registry_result).resolve(domain);
  if (const auto *error = std::get_if<Error>(&services_result)) {
    return *error;
  }
  const auto &services = std::get<std::vector<std::string>>(services_result);

  if (progress) {
    progress(LookupStage::querying_registry);
  }
  std::optional<Error> last_retryable_error;
  for (const auto &base_url : services) {
    HttpRequest request;
    request.url = base_url + "domain/" + encode_path_segment(domain.ascii());
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
      return RdapResponse{domain, request_url, std::move(response), std::move(document)};
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

} // namespace rdap
