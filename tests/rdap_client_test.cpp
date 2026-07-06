// SPDX-License-Identifier: GPL-3.0-or-later
#include "rdap/rdap_client.hpp"

#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace rdap;

namespace {

class FakeHttpClient final : public HttpClient {
public:
  Result<HttpResponse> get(const HttpRequest &request, const CancellationToken &) override {
    requests.push_back(request);
    REQUIRE_FALSE(responses.empty());
    auto result = std::move(responses.front());
    responses.pop_front();
    return result;
  }

  std::deque<Result<HttpResponse>> responses;
  std::vector<HttpRequest> requests;
};

HttpResponse response(long status, std::string body,
                      std::string effective_url = "https://registry.example/domain/example.com") {
  return HttpResponse{
      status, std::move(effective_url), "application/rdap+json", std::move(body), {}};
}

std::string bootstrap(std::string urls = R"("https://registry.example/")") {
  return R"({"version":"1.0","services":[[["com"],[)" + urls + R"(]]]})";
}

DomainName example_domain() {
  auto parsed = DomainName::parse("Example.COM");
  REQUIRE(std::holds_alternative<DomainName>(parsed));
  return std::get<DomainName>(std::move(parsed));
}

ResourceQuery query(std::string_view value) {
  auto parsed = ResourceQueryParser::parse(value);
  REQUIRE(std::holds_alternative<ResourceQuery>(parsed));
  return std::get<ResourceQuery>(std::move(parsed));
}

} // namespace

TEST_CASE("client performs bootstrap and domain lookup") {
  FakeHttpClient http;
  http.responses.emplace_back(response(200L, bootstrap(), "https://data.iana.org/rdap/dns.json"));
  http.responses.emplace_back(
      response(200L, R"({"objectClassName":"domain","ldhName":"EXAMPLE.COM"})"));
  RdapClient client(http);
  std::vector<LookupStage> stages;

  auto result = client.lookup_domain(example_domain(), {},
                                     [&](LookupStage stage) { stages.push_back(stage); });

  REQUIRE(std::holds_alternative<RdapResponse>(result));
  const auto &rdap_response = std::get<RdapResponse>(result);
  CHECK(rdap_response.request_url == "https://registry.example/domain/example.com");
  CHECK(rdap_response.document["objectClassName"] == "domain");
  REQUIRE(http.requests.size() == 2U);
  CHECK(http.requests[0].maximum_body_size == 2U * 1024U * 1024U);
  CHECK(http.requests[1].headers.front().find("application/rdap+json") != std::string::npos);
  CHECK(stages ==
        std::vector<LookupStage>{LookupStage::loading_bootstrap, LookupStage::querying_registry});
}

TEST_CASE("bootstrap is cached for subsequent lookups") {
  FakeHttpClient http;
  http.responses.emplace_back(response(200L, bootstrap()));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"domain"})"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"domain"})"));
  RdapClient client(http);

  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup_domain(example_domain(), {})));
  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup_domain(example_domain(), {})));
  CHECK(http.requests.size() == 3U);
}

TEST_CASE("client falls back after a retryable registry failure") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, bootstrap(R"("https://one.example/","https://two.example/")")));
  http.responses.emplace_back(response(503L, R"({"errorCode":503})"));
  http.responses.emplace_back(
      response(200L, R"({"objectClassName":"domain"})", "https://two.example/domain/example.com"));
  RdapClient client(http);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(std::get<RdapResponse>(result).http.effective_url ==
        "https://two.example/domain/example.com");
}

TEST_CASE("429 exposes retry information without trying another service") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, bootstrap(R"("https://one.example/","https://two.example/")")));
  auto limited = response(429L, R"({"errorCode":429})");
  limited.headers["retry-after"] = "60";
  http.responses.emplace_back(std::move(limited));
  RdapClient client(http);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<Error>(result));
  const auto &error = std::get<Error>(result);
  CHECK(error.http_status == 429L);
  CHECK(error.retry_after == "60");
  CHECK(http.requests.size() == 2U);
}

TEST_CASE("invalid RDAP JSON is reported") {
  FakeHttpClient http;
  http.responses.emplace_back(response(200L, bootstrap()));
  http.responses.emplace_back(response(200L, "not json"));
  RdapClient client(http);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<Error>(result));
  CHECK(std::get<Error>(result).code == ErrorCode::invalid_json);
}

TEST_CASE("transport cancellation is not retried") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, bootstrap(R"("https://one.example/","https://two.example/")")));
  http.responses.emplace_back(
      Error{ErrorCode::cancelled, "cancelled", {}, std::nullopt, std::nullopt});
  RdapClient client(http);

  auto result = client.lookup_domain(example_domain(), {});
  REQUIRE(std::holds_alternative<Error>(result));
  CHECK(std::get<Error>(result).code == ErrorCode::cancelled);
  CHECK(http.requests.size() == 2U);
}

TEST_CASE("transport safety errors are propagated") {
  for (const auto code : {ErrorCode::timeout, ErrorCode::response_too_large, ErrorCode::tls}) {
    CAPTURE(code);
    FakeHttpClient http;
    http.responses.emplace_back(response(200L, bootstrap()));
    http.responses.emplace_back(Error{code, "request failed", {}, std::nullopt, std::nullopt});
    RdapClient client(http);

    auto result = client.lookup_domain(example_domain(), {});
    REQUIRE(std::holds_alternative<Error>(result));
    CHECK(std::get<Error>(result).code == code);
  }
}

TEST_CASE("client performs IPv4 lookup through the address bootstrap") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, R"({"version":"1.0","services":[[["0.0.0.0/0"],["https://rir.example/"]]]})",
               "https://data.iana.org/rdap/ipv4.json"));
  http.responses.emplace_back(response(
      200L,
      R"({"objectClassName":"ip network","startAddress":"192.0.2.0","endAddress":"192.0.2.255"})",
      "https://rir.example/ip/192.0.2.1"));
  RdapClient client(http);

  auto result = client.lookup(query("192.0.2.1"), {});

  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(std::get<RdapResponse>(result).request_url == "https://rir.example/ip/192.0.2.1");
  REQUIRE(http.requests.size() == 2U);
  CHECK(http.requests.front().url == "https://data.iana.org/rdap/ipv4.json");
}

TEST_CASE("client preserves normalized CIDR path segments") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, R"({"version":"1.0","services":[[["0.0.0.0/0"],["https://rir.example/"]]]})"));
  http.responses.emplace_back(
      response(200L, R"({"objectClassName":"ip network"})", "https://rir.example/ip/192.0.2.0/24"));
  RdapClient client(http);

  auto result = client.lookup(query("192.0.2.42/24"), {});

  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(std::get<RdapResponse>(result).request_url == "https://rir.example/ip/192.0.2.0/24");
}

TEST_CASE("client performs ASN lookup through the ASN bootstrap") {
  FakeHttpClient http;
  http.responses.emplace_back(response(
      200L, R"({"version":"1.0","services":[[["1-4294967295"],["https://rir.example/"]]]})",
      "https://data.iana.org/rdap/asn.json"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"autnum","startAutnum":13335})",
                                       "https://rir.example/autnum/13335"));
  RdapClient client(http);

  auto result = client.lookup(query("AS13335"), {});

  REQUIRE(std::holds_alternative<RdapResponse>(result));
  CHECK(std::get<RdapResponse>(result).request_url == "https://rir.example/autnum/13335");
  CHECK(http.requests.front().url == "https://data.iana.org/rdap/asn.json");
}

TEST_CASE("number resource bootstraps are cached independently") {
  FakeHttpClient http;
  http.responses.emplace_back(
      response(200L, R"({"version":"1.0","services":[[["0.0.0.0/0"],["https://v4.example/"]]]})"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"ip network"})"));
  http.responses.emplace_back(
      response(200L, R"({"version":"1.0","services":[[["1-100"],["https://asn.example/"]]]})"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"autnum"})"));
  http.responses.emplace_back(response(200L, R"({"objectClassName":"ip network"})"));
  RdapClient client(http);

  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup(query("192.0.2.1"), {})));
  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup(query("AS42"), {})));
  REQUIRE(std::holds_alternative<RdapResponse>(client.lookup(query("198.51.100.1"), {})));
  CHECK(http.requests.size() == 5U);
}
